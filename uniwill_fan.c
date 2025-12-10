// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal Uniwill fan control module v2
 * For newer TUXEDO/Uniwill laptops with universal EC fan control
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/wmi.h>
#include <linux/acpi.h>
#include <linux/mutex.h>

MODULE_DESCRIPTION("Minimal fan control for Uniwill/TUXEDO laptops");
MODULE_AUTHOR("Community");
MODULE_VERSION("0.2.0");
MODULE_LICENSE("GPL");

/* WMI GUIDs for Uniwill laptops */
#define UNIWILL_WMI_MGMT_GUID_BC "ABBC0F6F-8EA1-11D1-00A0-C90629100000"

MODULE_ALIAS("wmi:" UNIWILL_WMI_MGMT_GUID_BC);

/* EC addresses for custom fan table control (new method) */
#define UW_EC_REG_USE_CUSTOM_FAN_TABLE_0    0x07c5
#define UW_EC_REG_USE_CUSTOM_FAN_TABLE_1    0x07c6

#define UW_EC_REG_CPU_FAN_TABLE_END_TEMP    0x0f00
#define UW_EC_REG_CPU_FAN_TABLE_START_TEMP  0x0f10
#define UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED   0x0f20

#define UW_EC_REG_GPU_FAN_TABLE_END_TEMP    0x0f30
#define UW_EC_REG_GPU_FAN_TABLE_START_TEMP  0x0f40
#define UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED   0x0f50

/* Direct fan control (old method, still used alongside) */
#define UW_EC_REG_FAN1_SPEED   0x1804
#define UW_EC_REG_FAN2_SPEED   0x1809

/* Temperature sensors */
#define UW_EC_REG_FAN1_TEMP    0x043e
#define UW_EC_REG_FAN2_TEMP    0x044f

/* Fan mode register */
#define UW_EC_REG_FAN_MODE     0x0751
#define UW_EC_FAN_MODE_BIT     0x40

/* Manual mode control - KEY REGISTER! */
#define UW_EC_REG_MANUAL_MODE  0x0741

/* Custom profile mode - required for IBP Gen10 and similar */
#define UW_EC_REG_CUSTOM_PROFILE 0x0727
#define UW_EC_CUSTOM_PROFILE_BIT 0x40  /* bit 6 */

#define FAN_SPEED_MAX          0xc8  /* 200 = 100% */
#define FAN_ON_MIN_SPEED       0x19  /* 25 = ~12.5% minimum when on */

static DEFINE_MUTEX(ec_lock);
static struct class *fan_class;
static struct device *fan_device;
static struct cdev fan_cdev;
static dev_t fan_dev;
static bool fans_initialized = false;

/* Read from EC RAM via WMI */
static int uw_ec_read(u16 addr, u8 *value)
{
    acpi_status status;
    union acpi_object *out_obj;
    u32 wmi_arg[10] = {0};
    u8 *arg_bytes = (u8 *)wmi_arg;
    struct acpi_buffer in = { sizeof(wmi_arg), wmi_arg };
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    int ret = 0;

    mutex_lock(&ec_lock);

    /* Set up read command: addr in bytes 0-1, function 1 (read) in byte 5 */
    arg_bytes[0] = addr & 0xff;
    arg_bytes[1] = (addr >> 8) & 0xff;
    arg_bytes[5] = 1;  /* read function */

    status = wmi_evaluate_method(UNIWILL_WMI_MGMT_GUID_BC, 0, 4, &in, &out);
    
    if (ACPI_FAILURE(status)) {
        pr_err("WMI read failed for addr 0x%04x\n", addr);
        ret = -EIO;
        goto out;
    }

    out_obj = (union acpi_object *)out.pointer;
    if (out_obj && out_obj->type == ACPI_TYPE_BUFFER && out_obj->buffer.length >= 1) {
        *value = out_obj->buffer.pointer[0];
    } else {
        ret = -EIO;
    }

    kfree(out_obj);
out:
    mutex_unlock(&ec_lock);
    return ret;
}

/* Write to EC RAM via WMI with retry */
static int uw_ec_write(u16 addr, u8 value)
{
    acpi_status status;
    u32 wmi_arg[10] = {0};
    u8 *arg_bytes = (u8 *)wmi_arg;
    struct acpi_buffer in = { sizeof(wmi_arg), wmi_arg };
    struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
    int ret = 0;
    int retries = 3;

    mutex_lock(&ec_lock);

retry:
    memset(wmi_arg, 0, sizeof(wmi_arg));
    
    /* Set up write command */
    arg_bytes[0] = addr & 0xff;
    arg_bytes[1] = (addr >> 8) & 0xff;
    arg_bytes[2] = value;
    arg_bytes[5] = 0;  /* write function */

    status = wmi_evaluate_method(UNIWILL_WMI_MGMT_GUID_BC, 0, 4, &in, &out);
    
    if (ACPI_FAILURE(status)) {
        if (--retries > 0) {
            msleep(50);
            goto retry;
        }
        pr_err("WMI write failed for addr 0x%04x\n", addr);
        ret = -EIO;
    }

    kfree(out.pointer);
    mutex_unlock(&ec_lock);
    return ret;
}

/* Initialize the custom fan table for full control */
static int init_custom_fan_table(void)
{
    u8 val0, val1;
    int i;
    u8 temp_offset = 115;

    if (fans_initialized)
        return 0;

    pr_info("Initializing custom fan table...\n");

    /* CRITICAL: Set custom profile mode (bit 6 of 0x0727)
     * This is REQUIRED for IBP Gen10 and similar devices!
     * Without this, the EC ignores our fan speed settings.
     */
    uw_ec_read(UW_EC_REG_CUSTOM_PROFILE, &val0);
    val0 &= ~UW_EC_CUSTOM_PROFILE_BIT;  /* Clear first */
    uw_ec_write(UW_EC_REG_CUSTOM_PROFILE, val0);
    msleep(50);
    val0 |= UW_EC_CUSTOM_PROFILE_BIT;   /* Then set */
    uw_ec_write(UW_EC_REG_CUSTOM_PROFILE, val0);

    /* Enable manual mode - this tells EC we're taking over */
    uw_ec_write(UW_EC_REG_MANUAL_MODE, 0x01);

    /* Disable full fan mode (0x40 bit) - required for new fan control */
    uw_ec_read(UW_EC_REG_FAN_MODE, &val0);
    if (val0 & UW_EC_FAN_MODE_BIT) {
        uw_ec_write(UW_EC_REG_FAN_MODE, val0 & ~UW_EC_FAN_MODE_BIT);
    }

    /* Enable custom fan table 0 (bit 7 at 0x07c5) - separates fan tables for both fans */
    uw_ec_read(UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, &val0);
    if (!((val0 >> 7) & 1)) {
        uw_ec_write(UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, val0 | (1 << 7));
    }

    /* Set up the fan table:
     * - Zone 0: 0-115°C with controllable speed
     * - Zones 1-15: dummy zones at unreachable temps with max speed
     */
    
    /* CPU fan table - zone 0 */
    uw_ec_write(UW_EC_REG_CPU_FAN_TABLE_END_TEMP, 115);
    uw_ec_write(UW_EC_REG_CPU_FAN_TABLE_START_TEMP, 0);
    uw_ec_write(UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED, 0x00);

    /* GPU fan table - zone 0 */
    uw_ec_write(UW_EC_REG_GPU_FAN_TABLE_END_TEMP, 120);
    uw_ec_write(UW_EC_REG_GPU_FAN_TABLE_START_TEMP, 0);
    uw_ec_write(UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED, 0x00);

    /* Fill remaining zones with dummy values */
    for (i = 1; i <= 15; i++) {
        uw_ec_write(UW_EC_REG_CPU_FAN_TABLE_END_TEMP + i, temp_offset + i + 1);
        uw_ec_write(UW_EC_REG_CPU_FAN_TABLE_START_TEMP + i, temp_offset + i);
        uw_ec_write(UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED + i, 0xc8);
        
        uw_ec_write(UW_EC_REG_GPU_FAN_TABLE_END_TEMP + i, temp_offset + i + 1);
        uw_ec_write(UW_EC_REG_GPU_FAN_TABLE_START_TEMP + i, temp_offset + i);
        uw_ec_write(UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED + i, 0xc8);
    }

    /* Enable custom fan table 1 (bit 2 at 0x07c6) */
    uw_ec_read(UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, &val1);
    if (!((val1 >> 2) & 1)) {
        uw_ec_write(UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, val1 | (1 << 2));
    }

    fans_initialized = true;
    pr_info("Custom fan table initialized\n");
    return 0;
}

static int fan_get_temp(int fan_idx)
{
    u8 temp;
    u16 addr = (fan_idx == 0) ? UW_EC_REG_FAN1_TEMP : UW_EC_REG_FAN2_TEMP;
    
    if (uw_ec_read(addr, &temp) < 0)
        return -EIO;
    
    return temp;
}

static int fan_get_speed(int fan_idx)
{
    u8 speed;
    u16 addr = (fan_idx == 0) ? UW_EC_REG_FAN1_SPEED : UW_EC_REG_FAN2_SPEED;
    
    if (uw_ec_read(addr, &speed) < 0)
        return -EIO;
    
    return speed;
}

static int fan_set_speed(int fan_idx, u8 speed)
{
    u16 table_addr, direct_addr;
    int i;

    if (!fans_initialized)
        init_custom_fan_table();

    if (fan_idx == 0) {
        table_addr = UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED;
        direct_addr = UW_EC_REG_FAN1_SPEED;
    } else {
        table_addr = UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED;
        direct_addr = UW_EC_REG_FAN2_SPEED;
    }

    /* Clamp speed */
    if (speed > FAN_SPEED_MAX)
        speed = FAN_SPEED_MAX;

    /* Handle fan off - use speed=1 trick to avoid EC spinning up to 30% */
    if (speed == 0)
        speed = 1;
    else if (speed < FAN_ON_MIN_SPEED)
        speed = FAN_ON_MIN_SPEED;

    /* Write to custom fan table zone 0 */
    uw_ec_write(table_addr, speed);

    /* Also write directly to fan speed registers (belt and suspenders) */
    for (i = 0; i < 5; i++) {
        uw_ec_write(direct_addr, speed);
        msleep(10);
    }

    return 0;
}

static int fan_set_auto(void)
{
    u8 val0, val1, mode;

    /* Disable custom fan tables - bit 2 first, then bit 7 */
    uw_ec_read(UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, &val1);
    if ((val1 >> 2) & 1) {
        uw_ec_write(UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, val1 & ~(1 << 2));
    }

    uw_ec_read(UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, &val0);
    if ((val0 >> 7) & 1) {
        uw_ec_write(UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, val0 & ~(1 << 7));
    }

    /* Clear manual fan mode bit */
    uw_ec_read(UW_EC_REG_FAN_MODE, &mode);
    if (mode & UW_EC_FAN_MODE_BIT) {
        uw_ec_write(UW_EC_REG_FAN_MODE, mode & ~UW_EC_FAN_MODE_BIT);
    }

    /* Disable manual mode - give control back to EC */
    uw_ec_write(UW_EC_REG_MANUAL_MODE, 0x00);

    /* Clear custom profile mode bit */
    uw_ec_read(UW_EC_REG_CUSTOM_PROFILE, &val0);
    if (val0 & UW_EC_CUSTOM_PROFILE_BIT) {
        uw_ec_write(UW_EC_REG_CUSTOM_PROFILE, val0 & ~UW_EC_CUSTOM_PROFILE_BIT);
    }

    fans_initialized = false;
    pr_info("Restored automatic fan control\n");
    return 0;
}

static long fan_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    return -ENOTTY;  /* Not implemented - use sysfs */
}

static int fan_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int fan_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations fan_fops = {
    .owner = THIS_MODULE,
    .open = fan_open,
    .release = fan_release,
    .unlocked_ioctl = fan_ioctl,
};

/* Device attributes for sysfs */
static ssize_t fan1_speed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int speed = fan_get_speed(0);
    if (speed < 0)
        return speed;
    return sprintf(buf, "%d\n", speed);
}

static ssize_t fan1_speed_store(struct device *dev, struct device_attribute *attr,
                                const char *buf, size_t count)
{
    int speed;
    if (kstrtoint(buf, 10, &speed) < 0)
        return -EINVAL;
    if (fan_set_speed(0, speed) < 0)
        return -EIO;
    return count;
}
static DEVICE_ATTR_RW(fan1_speed);

static ssize_t fan2_speed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int speed = fan_get_speed(1);
    if (speed < 0)
        return speed;
    return sprintf(buf, "%d\n", speed);
}

static ssize_t fan2_speed_store(struct device *dev, struct device_attribute *attr,
                                const char *buf, size_t count)
{
    int speed;
    if (kstrtoint(buf, 10, &speed) < 0)
        return -EINVAL;
    if (fan_set_speed(1, speed) < 0)
        return -EIO;
    return count;
}
static DEVICE_ATTR_RW(fan2_speed);

static ssize_t temp1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int temp = fan_get_temp(0);
    if (temp < 0)
        return temp;
    return sprintf(buf, "%d\n", temp);
}
static DEVICE_ATTR_RO(temp1);

static ssize_t temp2_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int temp = fan_get_temp(1);
    if (temp < 0)
        return temp;
    return sprintf(buf, "%d\n", temp);
}
static DEVICE_ATTR_RO(temp2);

static ssize_t fan_auto_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
    int val;
    if (kstrtoint(buf, 10, &val) < 0)
        return -EINVAL;
    if (val)
        fan_set_auto();
    return count;
}
static DEVICE_ATTR_WO(fan_auto);

static struct attribute *fan_attrs[] = {
    &dev_attr_fan1_speed.attr,
    &dev_attr_fan2_speed.attr,
    &dev_attr_temp1.attr,
    &dev_attr_temp2.attr,
    &dev_attr_fan_auto.attr,
    NULL,
};

static const struct attribute_group fan_attr_group = {
    .attrs = fan_attrs,
};

static int __init uniwill_fan_init(void)
{
    int ret;

    /* Check if WMI interface exists */
    if (!wmi_has_guid(UNIWILL_WMI_MGMT_GUID_BC)) {
        pr_err("Uniwill WMI GUID not found\n");
        return -ENODEV;
    }

    pr_info("Uniwill WMI interface found\n");

    /* Create character device */
    ret = alloc_chrdev_region(&fan_dev, 0, 1, "uniwill_fan");
    if (ret < 0) {
        pr_err("Failed to allocate chrdev region\n");
        return ret;
    }

    cdev_init(&fan_cdev, &fan_fops);
    ret = cdev_add(&fan_cdev, fan_dev, 1);
    if (ret < 0) {
        pr_err("Failed to add cdev\n");
        goto err_cdev;
    }

    /* Create class */
    fan_class = class_create("uniwill_fan");
    if (IS_ERR(fan_class)) {
        ret = PTR_ERR(fan_class);
        pr_err("Failed to create class\n");
        goto err_class;
    }

    /* Create device node */
    fan_device = device_create(fan_class, NULL, fan_dev, NULL, "uniwill_fan");
    if (IS_ERR(fan_device)) {
        pr_err("Failed to create device\n");
        ret = PTR_ERR(fan_device);
        goto err_device;
    }

    /* Add sysfs attributes */
    ret = sysfs_create_group(&fan_device->kobj, &fan_attr_group);
    if (ret) {
        pr_err("Failed to create sysfs attributes\n");
        goto err_sysfs;
    }

    pr_info("Uniwill fan control v2 loaded\n");
    pr_info("Use /sys/class/uniwill_fan/uniwill_fan/ for control\n");

    return 0;

err_sysfs:
    device_destroy(fan_class, fan_dev);
err_device:
    class_destroy(fan_class);
err_class:
    cdev_del(&fan_cdev);
err_cdev:
    unregister_chrdev_region(fan_dev, 1);
    return ret;
}

static void __exit uniwill_fan_exit(void)
{
    /* Restore auto mode on unload */
    fan_set_auto();

    sysfs_remove_group(&fan_device->kobj, &fan_attr_group);
    device_destroy(fan_class, fan_dev);
    class_destroy(fan_class);
    cdev_del(&fan_cdev);
    unregister_chrdev_region(fan_dev, 1);

    pr_info("Uniwill fan control unloaded\n");
}

module_init(uniwill_fan_init);
module_exit(uniwill_fan_exit);
