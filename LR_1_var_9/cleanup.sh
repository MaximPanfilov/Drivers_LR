#!/bin/bash

echo "=== Emergency Cleanup ==="

# Убиваем все пользовательские процессы
echo "Stopping user processes..."
pkill -f process1
pkill -f process2
pkill -f process3
pkill -f monitor

# Ждем завершения процессов
sleep 2

# Принудительно убиваем если остались
pkill -9 -f process1
pkill -9 -f process2
pkill -9 -f process3
pkill -9 -f monitor

# Выгружаем драйвер
echo "Unloading driver..."
sudo rmmod scull_driver 2>/dev/null

# Если драйвер заблокирован, пробуем принудительно
if lsmod | grep -q scull_driver; then
    echo "Driver is busy, trying force unload..."
    sudo rmmod -f scull_driver 2>/dev/null
fi

# Удаляем device files
echo "Removing device files..."
sudo rm -f /dev/scull0
sudo rm -f /dev/scull1
sudo rm -f /dev/scull2

# Проверяем остались ли процессы использующие драйвер
echo "Checking for processes using driver..."
sudo lsof /dev/scull* 2>/dev/null

# Очищаем сообщения ядра
echo "Clearing kernel messages..."
sudo dmesg -c > /dev/null

echo "=== Cleanup complete ==="