# Makefile для netstat_driver
obj-m += netstat_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install: all
	sudo insmod netstat_driver.ko

uninstall:
	sudo rmmod netstat_driver

test: install
	@echo "=== Драйвер загружен ==="
	@sleep 1
	@echo "Статистика доступна в: /proc/loopback_stats"
	@cat /proc/loopback_stats

help:
	@echo "Доступные команды:"
	@echo "  make        - скомпилировать драйвер"
	@echo "  make clean  - очистить сборочные файлы"
	@echo "  make install- скомпилировать и загрузить"
	@echo "  make test   - протестировать драйвер"
	@echo "  make uninstall - выгрузить драйвер"