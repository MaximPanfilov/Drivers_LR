obj-m += scull_driver.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo insmod scull_driver.ko

uninstall:
	sudo rmmod scull_driver

test:
	sudo dmesg | tail -20