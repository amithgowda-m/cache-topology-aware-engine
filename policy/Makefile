# CTAE Policy Module Makefile

obj-m := ctae_policy.o

KERNEL_DIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Paths to dependencies
CORE_DIR := $(PWD)/../core
MONITOR_DIR := $(PWD)/../monitor

# Include symbols from other modules
KBUILD_EXTRA_SYMBOLS := $(CORE_DIR)/Module.symvers $(MONITOR_DIR)/Module.symvers

.PHONY: all clean install uninstall reload info test help

all:
	@echo "Building CTAE Policy Module..."
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	@echo "Cleaning CTAE Policy Module..."
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f Module.symvers modules.order

install: all
	@echo "Installing CTAE Policy Module..."
	sudo insmod ctae_policy.ko

uninstall:
	@echo "Removing CTAE Policy Module..."
	sudo rmmod ctae_policy 2>/dev/null || true

reload: uninstall install
	@echo "Module reloaded"

info:
	@echo "Module Information:"
	@modinfo ctae_policy.ko 2>/dev/null || echo "Module not built yet"

test:
	@echo "=== Module Status ==="
	@lsmod | grep ctae_policy || echo "Module not loaded"
	@echo ""
	@echo "=== Recent Kernel Messages ==="
	@sudo dmesg | grep "CTAE Policy" | tail -40

help:
	@echo "CTAE Policy Module Build Targets:"
	@echo "  all       - Build the module (default)"
	@echo "  clean     - Remove build artifacts"
	@echo "  install   - Load module into kernel"
	@echo "  uninstall - Remove module from kernel"
	@echo "  reload    - Unload and reload module"
	@echo "  info      - Display module information"
	@echo "  test      - Show module status and logs"
	@echo "  help      - Show this message"
