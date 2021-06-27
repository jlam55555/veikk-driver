ifneq ($(KERNELRELEASE),)
	obj-m := veikk.o
	veikk-objs := hid-veikk.o

else
	KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod
	modprobe veikk

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

uninstall:
	modprobe -r veikk

endif