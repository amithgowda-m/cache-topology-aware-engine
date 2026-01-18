# CTAE Policy Module Makefile

obj-m := ctae_policy.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	sudo insmod ctae_policy.ko
	@echo "Policy module loaded. Check dmesg for output."

uninstall:
	sudo rmmod ctae_policy 2>/dev/null || true

test:
	@echo "=== Policy Module Status ==="
	@lsmod | grep ctae_policy || echo "Module not loaded"
	@echo ""
	@echo "=== Recent Policy Logs ==="
	@dmesg | grep "CTAE Policy" | tail -20

.PHONY: all clean install uninstall test
