# Uniwill Fan Control

Minimal, silent fan control for TUXEDO/Uniwill laptops.

> **⚠️ Hardware Notice:** This project has only been tested on a **TUXEDO InfinityBook Pro AMD Gen10** with a **Ryzen AI 9 HX 370** processor. It may work on other Uniwill-based laptops with the same WMI interface, but this is untested. Use at your own risk.

## Why?

The stock kernel has no fan control for Uniwill-based laptops. TUXEDO provides their Control Center and custom kernel modules, but Control Center is a heavy Electron app and the tuxedo-drivers caused issues on my system - including the CPU randomly getting stuck at 600MHz. 

This project provides just fan control with no other baggage, keeping the rest native:

- **Minimal footprint** - ~17KB daemon + ~400KB kernel module
- **No dependencies** - works standalone without TUXEDO Control Center or tuxedo-drivers
- **Compatible with power-profiles-daemon** - handles only fan control, nothing else
- **Silent by default** - fans stay quiet when idle, ramp smoothly under load

## Recommended Setup

- Stock Arch Linux kernel (no tuxedo-drivers)
- `power-profiles-daemon` for power management
- This project for fan control

## How It Works

```
┌─────────────────────────────────────────────────────────────┐
│                      User Space                             │
│                                                             │
│  uniwill-fanctl (daemon)                                    │
│      │                                                      │
│      ├──reads temps──▶ /sys/class/hwmon/ (k10temp, amdgpu) │
│      │                                                      │
│      └──writes speed─▶ /sys/class/uniwill_fan/fan1_speed   │
│                              │                              │
└──────────────────────────────│──────────────────────────────┘
                               │ sysfs
┌──────────────────────────────│──────────────────────────────┐
│  Kernel                      ▼                              │
│                    uniwill_fan.ko                           │
│                         │                                   │
│                         │ WMI calls                         │
│                         ▼                                   │
│                 ACPI WMI Interface ──▶ Embedded Controller  │
│                                              │              │
└──────────────────────────────────────────────│──────────────┘
                                               ▼
                                          Fans 1 & 2
```

**The daemon loop:**

1. Read CPU temp from `k10temp`, GPU temp from `amdgpu` (EC fallback if unavailable)
2. Take maximum of CPU and GPU temps
3. Calculate target speed using interpolated fan curve
4. Apply hysteresis (6°C gap prevents oscillation)
5. Write target speed to sysfs
6. Sleep 1s (active) or 5s (idle, <50°C at minimum speed)

**Fan curve:**

```
Fan %
100│                                    ┌────
 75│                            ┌───────┘
 50│                    ┌───────┘
 25│            ┌───────┘
~13│────────────┘ (minimum, prevents EC fighting)
    └────────┬───────┬───────┬───────┬───────┬──▶ Temp °C
            62      70      78      86      92
```

## Features

- **Silent fan curve**: Smooth, quiet operation with hysteresis
- **Direct EC control**: Communicates with EC via WMI interface
- **Dual fan support**: Controls both CPU and GPU fans
- **Real hwmon integration**: Reads temps from k10temp and amdgpu sensors
- **EC fallback**: Uses EC temperature sensor if hwmon unavailable
- **Adaptive polling**: 5s intervals when idle, 1s when active
- **Systemd service**: Runs automatically on boot
- **No runtime dependencies**: Single binary, only links to libc

## Compatibility

**Tested on:**

- TUXEDO InfinityBook Pro AMD Gen10 (Ryzen AI 9 HX 370)
- Arch Linux, kernel 6.x
- WMI GUID `ABBC0F6F-8EA1-11D1-00A0-C90629100000`

**Untested but may work on:**

- Other Uniwill-based laptops with the same WMI GUID

## Installation

### Prerequisites

```bash
sudo pacman -S base-devel linux-headers dkms
```

### Step 1: Build and Test the Module

```bash
git clone https://github.com/timohubois/uniwill-fan-control.git
cd uniwill-fan-control

# Build
make

# Load module for testing
sudo make load

# Verify it works
cat /sys/class/uniwill_fan/uniwill_fan/temp1      # EC temp (degrees C)
cat /sys/class/uniwill_fan/uniwill_fan/fan1_speed # Fan speed (0-200)

# Test manual control
echo 100 | sudo tee /sys/class/uniwill_fan/uniwill_fan/fan1_speed  # Set 50%
echo 50 | sudo tee /sys/class/uniwill_fan/uniwill_fan/fan1_speed   # Set 25%
echo 1 | sudo tee /sys/class/uniwill_fan/uniwill_fan/fan_auto      # Restore auto
```

If this works, proceed to step 2.

### Step 2: Test the Daemon

```bash
# Run daemon manually (Ctrl+C to stop)
sudo ./uniwill-fanctl
```

You should see temperature and fan speed updating. Run `./uniwill-fanctl -h` for help. If this works, proceed to step 3.

### Step 3: Install Permanently

#### Option A: DKMS (Recommended)

DKMS automatically rebuilds the module when you update your kernel:

```bash
sudo make uniwill-fan-control-install-dkms
sudo systemctl enable --now uniwill-fan.service
```

#### Option B: Manual Installation

Without DKMS, you'll need to rebuild manually after kernel updates:

```bash
sudo make install-all
sudo systemctl enable --now uniwill-fan.service
```

### Manual Installation (Step-by-Step)

If you prefer step-by-step:

```bash
# Install module via DKMS (or use 'make install' for non-DKMS)
sudo make uniwill-fan-control-dkms-install

# Auto-load on boot
sudo make install-autoload

# Install and enable service
sudo make install-service
sudo systemctl enable --now uniwill-fan.service
```

## Usage

### Manual Control

```bash
# Load module
sudo modprobe uniwill_fan

# Check current values
cat /sys/class/uniwill_fan/uniwill_fan/temp1      # EC temp (degrees C)
cat /sys/class/uniwill_fan/uniwill_fan/fan1_speed # Fan speed (0-200)

# Set fan speed (0-200, where 200 = 100%)
echo 100 | sudo tee /sys/class/uniwill_fan/uniwill_fan/fan1_speed

# Restore automatic control
echo 1 | sudo tee /sys/class/uniwill_fan/uniwill_fan/fan_auto
```

### Fan Curve Daemon

```bash
# Run manually (interactive mode with status display)
sudo ./uniwill-fanctl

# Show help and configuration
./uniwill-fanctl -h

# Or use the systemd service
sudo systemctl start uniwill-fan.service
sudo systemctl status uniwill-fan.service
```

### Configuration

The fan curve thresholds are compiled into the binary. To customize, edit `uniwill-fanctl.c` and rebuild:

```c
/* Temperature thresholds (C) */
#define TEMP_SILENT     62      /* Below this: minimum speed */
#define TEMP_LOW        70      /* Start of low speed */
#define TEMP_MED        78      /* Medium speed */
#define TEMP_HIGH       86      /* High speed */
#define TEMP_MAX        92      /* Maximum speed */

/* Fan speeds (0-200) */
#define SPEED_MIN       25      /* 12.5% - minimum to prevent EC fighting */
#define SPEED_LOW       50      /* 25% */
#define SPEED_MED       100     /* 50% */
#define SPEED_HIGH      150     /* 75% */
#define SPEED_MAX       200     /* 100% */
```

> **Note:** `SPEED_MIN` is 25 because values below 25 cause the EC's safety logic to periodically override the fan speed, resulting in annoying start/stop cycling.

Then rebuild and reinstall:

```bash
make uniwill-fanctl
sudo make install-service
```

## Uninstallation

### DKMS

```bash
sudo make uniwill-fan-control-uninstall-dkms
```

### Non-DKMS

```bash
sudo make uninstall-all
```

Or manually:

```bash
sudo systemctl disable --now uniwill-fan.service
sudo make uninstall-service
sudo make uninstall-autoload
sudo make uninstall
```

## Troubleshooting

### Module won't load

Check if WMI interface exists:

```bash
ls /sys/bus/wmi/devices/ | grep ABBC0F6
```

Check kernel logs:

```bash
dmesg | grep uniwill
```

### Fans not responding

Verify the sysfs interface exists:

```bash
ls /sys/class/uniwill_fan/uniwill_fan/
cat /sys/class/uniwill_fan/uniwill_fan/fan1_speed
```

### Service not starting

Check if module is loaded:

```bash
lsmod | grep uniwill_fan
```

Check service status:

```bash
sudo systemctl status uniwill-fan.service
sudo journalctl -u uniwill-fan.service
```

### Fan never fully stops

This is intentional. The daemon keeps the fan at a minimum of 12.5% to prevent the EC from fighting for control, which would cause annoying start/stop cycling. The minimum speed is barely audible.

## Technical Details

### Sysfs Interface

| Path | Access | Description |
|------|--------|-------------|
| `/sys/class/uniwill_fan/uniwill_fan/fan1_speed` | RW | Fan 1 speed (0-200) |
| `/sys/class/uniwill_fan/uniwill_fan/fan2_speed` | RW | Fan 2 speed (0-200) |
| `/sys/class/uniwill_fan/uniwill_fan/temp1` | RO | EC temperature sensor 1 |
| `/sys/class/uniwill_fan/uniwill_fan/temp2` | RO | EC temperature sensor 2 |
| `/sys/class/uniwill_fan/uniwill_fan/fan_auto` | WO | Write 1 to restore auto mode |

### EC Registers

The module uses the Uniwill WMI interface to communicate with the EC:

- Custom fan table: `0x0f00-0x0f5f`
- Direct fan control: `0x1804`, `0x1809`
- Fan mode: `0x0751`
- Custom profile mode: `0x0727` (bit 6) - required for IBP Gen10
- Manual mode: `0x0741`
- Custom fan table enable: `0x07c5` (bit 7)

### Fan Speed Values

The fan speed range is 0-200 (where 200 = 100%):

| Value | Behavior |
|-------|----------|
| 0 | Fan off |
| 1-24 | Clamped to 25 (minimum running speed) |
| 25-200 | Direct PWM control |

**Note:** The EC ramps fan speed smoothly, so changes aren't instant. When the daemon shows "Actual" different from "Target", the EC is still transitioning.

## License

GPL-2.0+

## Credits

- Based on reverse engineering of [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers)
- WMI interface discovery from TUXEDO Control Center
