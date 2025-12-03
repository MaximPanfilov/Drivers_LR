obj-m += netstat_driver.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD ?= $(shell pwd)

# Для ядра 6.x
ccflags-y := -std=gnu11 -Wno-declaration-after-statement

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	sudo insmod netstat_driver.ko

uninstall:
	sudo rmmod netstat_driver

test: install
	@echo "Драйвер загружен. Проверяем:"
	cat /proc/net_traffic_stats