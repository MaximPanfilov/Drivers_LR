# 1. Компиляция
make

# 2. Загрузка
sudo insmod netstat_driver.ko

# 3. Проверка
cat /proc/loopback_stats

# 4. Генерация трафика
ping -c 3 127.0.0.1

# 5. Проверка изменений
cat /proc/loopback_stats

# 6. Выгрузка
sudo rmmod netstat_driver

# 7. Wireshark
sudo wireshark