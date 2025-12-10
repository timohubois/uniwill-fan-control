obj-m += uniwill_fan.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PREFIX ?= /usr/local

# DKMS variables
DKMS_NAME := uniwill-fan-control
DKMS_VERSION := 0.1.0
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

.PHONY: all clean install uninstall install-service uninstall-service load unload uniwill-fanctl \
        uniwill-fan-control-dkms-install uniwill-fan-control-dkms-uninstall uniwill-fan-control-dkms-status

all: modules uniwill-fanctl

modules:
	make -C $(KDIR) M=$(PWD) modules

uniwill-fanctl: uniwill-fanctl.c
	$(CC) $(CFLAGS) -o uniwill-fanctl uniwill-fanctl.c

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f uniwill-fanctl

# Module installation (run 'make' first as normal user)
install:
	install -d /lib/modules/$(KVER)/extra/
	install -m 644 uniwill_fan.ko /lib/modules/$(KVER)/extra/
	depmod -a
	@echo "Module installed. Load with: modprobe uniwill_fan"

uninstall:
	rm -f /lib/modules/$(KVER)/extra/uniwill_fan.ko
	depmod -a
	@echo "Module uninstalled"

# Service installation (run 'make' first as normal user)
install-service:
	install -m 755 uniwill-fanctl $(PREFIX)/bin/uniwill-fanctl
	install -m 644 uniwill-fan.service /etc/systemd/system/
	sed -i 's|ExecStart=.*|ExecStart=$(PREFIX)/bin/uniwill-fanctl|' /etc/systemd/system/uniwill-fan.service
	systemctl daemon-reload
	@echo ""
	@echo "Service installed. Enable with:"
	@echo "  systemctl enable --now uniwill-fan.service"

uninstall-service:
	-systemctl stop uniwill-fan.service 2>/dev/null
	-systemctl disable uniwill-fan.service 2>/dev/null
	rm -f /etc/systemd/system/uniwill-fan.service
	rm -f $(PREFIX)/bin/uniwill-fanctl
	systemctl daemon-reload
	@echo "Service uninstalled"

# Module loading (for testing)
load: all
	-rmmod uniwill_fan 2>/dev/null
	insmod ./uniwill_fan.ko
	@echo "Module loaded"

unload:
	-rmmod uniwill_fan 2>/dev/null
	@echo "Module unloaded"

# Auto-load on boot
install-autoload:
	echo "uniwill_fan" > /etc/modules-load.d/uniwill-fan.conf
	@echo "Module will load on boot"

uninstall-autoload:
	rm -f /etc/modules-load.d/uniwill-fan.conf
	@echo "Auto-load disabled"

# Full install (module + autoload + service)
install-all: install install-autoload install-service
	@echo ""
	@echo "=== Installation Complete ==="
	@echo "The module will load on boot and fan control will start automatically."

uninstall-all: uninstall-service uninstall-autoload uninstall
	@echo ""
	@echo "=== Uninstallation Complete ==="

# DKMS installation (auto-rebuilds on kernel updates)
uniwill-fan-control-dkms-install: uniwill-fanctl
	@echo "Installing DKMS module source..."
	install -d $(DKMS_SRC)
	install -m 644 uniwill_fan.c $(DKMS_SRC)/
	install -m 644 dkms.conf $(DKMS_SRC)/
	@echo "obj-m += uniwill_fan.o" > $(DKMS_SRC)/Makefile
	dkms add -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms build -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms install -m $(DKMS_NAME) -v $(DKMS_VERSION)
	@echo ""
	@echo "=== DKMS Installation Complete ==="
	@echo "Module will auto-rebuild on kernel updates."
	@echo "Load with: modprobe uniwill_fan"

uniwill-fan-control-dkms-uninstall:
	-dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all 2>/dev/null
	rm -rf $(DKMS_SRC)
	@echo "DKMS module removed"

uniwill-fan-control-dkms-status:
	dkms status $(DKMS_NAME)

# Full DKMS install (module + autoload + service)
uniwill-fan-control-install-dkms: uniwill-fan-control-dkms-install install-autoload install-service
	@echo ""
	@echo "=== Full DKMS Installation Complete ==="
	@echo "The module will auto-rebuild on kernel updates and fan control starts on boot."

uniwill-fan-control-uninstall-dkms: uninstall-service uninstall-autoload uniwill-fan-control-dkms-uninstall
	@echo ""
	@echo "=== Full DKMS Uninstallation Complete ==="
