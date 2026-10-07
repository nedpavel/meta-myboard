obj-m := pixy-mvblli.o

# Yocto (module.bbclass) uebergibt KERNEL_SRC. Von Hand gebaut gilt der
# laufende Kernel, oder ein anderer Baum per KDIR=...
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
KDIR       ?= $(KERNEL_SRC)
PWD        := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
