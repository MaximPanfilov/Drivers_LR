#!/bin/bash

echo "Выгрузка драйвера scull_ring..."

# Удаляем узлы устройств
sudo rm -f /dev/scull_ring0
sudo rm -f /dev/scull_ring1
sudo rm -f /dev/scull_ring2

# Выгружаем модуль
sudo rmmod scull_ring

echo "Драйвер выгружен"