make

# Загрузить
sudo insmod entropy_mouse.ko

# Проверить
ls -la /dev/entropy_mouse
sudo dmesg | tail -5

# Читать данные
sudo dd if=/dev/entropy_mouse bs=16 count=1 2>/dev/null | hexdump -C

# Двигайте мышью и читайте снова
sudo dd if=/dev/entropy_mouse bs=32 count=1 2>/dev/null | hexdump -C

sudo rmmod entropy_mouse
sudo dmesg | tail -5sudo rmmod entropy_mouse
sudo dmesg | tail -5

sudo dmesg -w

sudo rmmod entropy_mouse
make clean
