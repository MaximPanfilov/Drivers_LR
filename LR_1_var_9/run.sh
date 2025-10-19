#!/bin/bash

# Компиляция драйвера
echo "Compiling driver..."
make

# Загрузка драйвера
echo "Loading driver..."
sudo insmod scull_driver.ko

# Создание device files
echo "Creating device files..."
sudo mknod /dev/scull0 c 240 0
sudo mknod /dev/scull1 c 240 1
sudo mknod /dev/scull2 c 240 2

sudo chmod 666 /dev/scull*

# Компиляция пользовательских программ
echo "Compiling user programs..."
gcc -o process1 process1.c
gcc -o process2 process2.c
gcc -o process3 process3.c
gcc -o monitor monitor.c

echo "Starting processes..."
./monitor &
MONITOR_PID=$!

./process1 &
PROC1_PID=$!

./process2 &
PROC2_PID=$!

./process3 &
PROC3_PID=$!

echo "All processes started. Press Enter to stop..."
read

echo "Stopping processes..."
kill $PROC1_PID $PROC2_PID $PROC3_PID $MONITOR_PID

echo "Unloading driver..."
sudo rmmod scull_driver

echo "Cleaning up..."
make clean
rm -f process1 process2 process3 monitor
sudo rm -f /dev/scull*

echo "Done."