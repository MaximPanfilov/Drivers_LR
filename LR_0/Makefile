obj-m += hello_world_driver.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo insmod hello_world_driver.ko

uninstall:
	sudo rmmod hello_world_driver

check:
	dmesg | tail -20