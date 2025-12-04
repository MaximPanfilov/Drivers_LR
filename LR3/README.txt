# 1. Выгрузите старый драйвер если есть
sudo rmmod entropy_mouse_driver 2>/dev/null || true
sudo rm -f /dev/entropy_mouse 2>/dev/null || true

# 2. Сохраните новый код в entropy_mouse_driver.c

# 3. Пересоберите
make clean
make

# 4. Загрузите драйвер
sudo insmod entropy_mouse_driver.ko

# 5. Проверьте сообщения
sudo dmesg | tail -15

# 6. Создайте файл устройства
MAJOR=$(cat /proc/devices | grep entropy_mouse | awk '{print $1}')
sudo mknod /dev/entropy_mouse c $MAJOR 0
sudo chmod 444 /dev/entropy_mouse

# 7. ТЕПЕРЬ ДВИГАЙТЕ МЫШЬЮ для накопления энтропии!
echo "Двигайте мышью 10-15 секунд..."
echo "Водите курсором по экрану, кликайте, крутите колесико"

# 8. После движения мыши попробуйте прочитать
sudo dd if=/dev/entropy_mouse bs=16 count=1 2>/dev/null | hexdump -C

# 9. Если "No data available" - значит нужно больше движений мыши
#    Подвигайте еще и попробуйте снова

# 10. Мониторинг событий мыши в реальном времени:
sudo dmesg -w | grep "entropy_mouse"