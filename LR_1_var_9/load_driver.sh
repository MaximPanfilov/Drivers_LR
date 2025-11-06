#!/bin/bash

echo "Загрузка драйвера scull_ring..."

# Загружаем модуль
sudo insmod scull_ring.ko

# Получаем major номер
MAJOR=$(cat /proc/devices | grep scull_ring | awk '{print $1}')

if [ -z "$MAJOR" ]; then
    echo "Ошибка: не удалось получить major номер"
    exit 1
fi

echo "Драйвер загружен с major номером: $MAJOR"

# Создаем узлы устройств
sudo mknod /dev/scull_ring0 c $MAJOR 0
sudo mknod /dev/scull_ring1 c $MAJOR 1
sudo mknod /dev/scull_ring2 c $MAJOR 2

# Устанавливаем права
sudo chmod 666 /dev/scull_ring*

echo "Узлы устройств созданы:"
ls -l /dev/scull_ring*