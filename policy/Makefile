KBUILD_EXTRA_SYMBOLS := $(PWD)/../core/Module.symvers $(PWD)/../monitor/Module.symvers
obj-m := ctae_policy.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean