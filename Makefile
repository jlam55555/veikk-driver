ifneq ($(KERNELRELEASE),)
	obj-m := veikk.o
	veikk-objs := hid-veikk.o

else
	KDIR ?= /lib/modules/$(shell uname -r)/build

build:
	$(MAKE) $(EXTRA_FLAGS) -C $(KDIR) M=$(CURDIR) modules

debug: EXTRA_FLAGS+=EXTRA_CFLAGS=-DDEBUG
debug: build

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod
	modprobe veikk

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

uninstall:
	modprobe -r veikk

endif