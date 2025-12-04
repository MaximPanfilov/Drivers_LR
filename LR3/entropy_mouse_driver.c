/*
 * entropy_mouse_driver.c - Драйвер энтропии с исправлениями
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/jiffies.h>
#include <linux/input.h>
#include <linux/spinlock.h>

#define DEVICE_NAME "entropy_mouse"
#define CLASS_NAME "entropy"
#define POOL_SIZE 256
#define MIN_ENTROPY_BITS 8  // Очень низкий порог для тестирования

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Mouse entropy collector driver");
MODULE_VERSION("3.0");

static int major_num;
static struct class* entropy_class = NULL;
static struct device* entropy_device = NULL;
static struct cdev entropy_cdev;

// Структура состояния драйвера
struct entropy_state {
    unsigned char pool[POOL_SIZE];
    int pool_index;
    unsigned int entropy_count;
    spinlock_t lock;
    struct input_handler input_handler;
    int mouse_events;  // Счетчик событий мыши
};

static struct entropy_state *state;

// Прототипы функций
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *);
static void entropy_event(struct input_handle *handle, unsigned int type,
                         unsigned int code, int value);
static int entropy_connect(struct input_handler *handler,
                          struct input_dev *dev,
                          const struct input_device_id *id);
static void entropy_disconnect(struct input_handle *handle);

// Операции с файлом устройства
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
};

// ID устройств мыши - более широкий фильтр
static const struct input_device_id entropy_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = { BIT_MASK(EV_REL) },
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = { BIT_MASK(EV_KEY) },
    },
    { }, // Завершающий элемент
};

// Обработчик событий мыши - УПРОЩЕННЫЙ
static void entropy_event(struct input_handle *handle, unsigned int type,
                         unsigned int code, int value)
{
    unsigned long flags;
    static unsigned int event_counter = 0;
    
    if (!state) return;
    
    event_counter++;
    
    // Логируем ВСЕ события для отладки
    printk(KERN_DEBUG "entropy_mouse: Event #%u: type=%u code=%u value=%d\n",
           event_counter, type, code, value);
    
    // Принимаем ЛЮБЫЕ события (не только REL_X/REL_Y)
    spin_lock_irqsave(&state->lock, flags);
    
    // Добавляем в пул байты из события
    state->pool[state->pool_index] ^= (type & 0xFF);
    state->pool[state->pool_index] ^= (code & 0xFF);
    state->pool[state->pool_index] ^= (value & 0xFF);
    state->pool[state->pool_index] ^= (jiffies & 0xFF);
    
    state->pool_index = (state->pool_index + 1) % POOL_SIZE;
    state->mouse_events++;
    
    // Увеличиваем счетчик энтропии
    state->entropy_count += 8; // 1 байт энтропии за событие
    
    // Простое перемешивание
    if (state->pool_index % 8 == 0) {
        int i;
        for (i = 0; i < POOL_SIZE - 1; i++) {
            state->pool[i] ^= state->pool[i + 1];
        }
        if (POOL_SIZE > 0) {
            state->pool[POOL_SIZE - 1] ^= state->pool[0];
        }
    }
    
    spin_unlock_irqrestore(&state->lock, flags);
    
    printk(KERN_INFO "entropy_mouse: Added entropy from mouse event. Total events: %d, entropy: %u bits\n",
           state->mouse_events, state->entropy_count);
}

static int entropy_connect(struct input_handler *handler,
                          struct input_dev *dev,
                          const struct input_device_id *id)
{
    struct input_handle *handle;
    int error;
    
    printk(KERN_INFO "entropy_mouse: CONNECTING to: %s (EV bits: %lx)\n", 
           dev->name, dev->evbit[0]);
    
    handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
    if (!handle)
        return -ENOMEM;
    
    handle->dev = dev;
    handle->handler = handler;
    handle->name = "entropy_mouse";
    
    error = input_register_handle(handle);
    if (error) {
        printk(KERN_ERR "entropy_mouse: Failed to register handle: %d\n", error);
        kfree(handle);
        return error;
    }
    
    error = input_open_device(handle);
    if (error) {
        printk(KERN_ERR "entropy_mouse: Failed to open device: %d\n", error);
        input_unregister_handle(handle);
        kfree(handle);
        return error;
    }
    
    printk(KERN_INFO "entropy_mouse: SUCCESSFULLY connected to %s\n", dev->name);
    return 0;
}

static void entropy_disconnect(struct input_handle *handle)
{
    printk(KERN_INFO "entropy_mouse: Disconnecting from %s\n", handle->dev->name);
    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

// Функции файловых операций
static int device_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "entropy_mouse: Device opened (entropy: %u bits)\n", 
           state ? state->entropy_count : 0);
    return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "entropy_mouse: Device closed\n");
    return 0;
}

static ssize_t device_read(struct file *filp, char __user *buffer,
                          size_t length, loff_t *offset)
{
    unsigned char *temp_buf;
    unsigned long flags;
    ssize_t bytes_to_read;
    int i;
    
    if (!state) {
        printk(KERN_ERR "entropy_mouse: No state!\n");
        return -ENODEV;
    }
    
    spin_lock_irqsave(&state->lock, flags);
    
    // Если нет энтропии, добавляем немного "искусственной"
    if (state->entropy_count < MIN_ENTROPY_BITS) {
        printk(KERN_INFO "entropy_mouse: Low entropy (%u/%u). Using fallback.\n",
               state->entropy_count, MIN_ENTROPY_BITS);
        
        // Добавляем немного системной энтропии как fallback
        get_random_bytes(&state->pool[state->pool_index], 16);
        state->pool_index = (state->pool_index + 16) % POOL_SIZE;
        state->entropy_count = MIN_ENTROPY_BITS; // Устанавливаем минимум
    }
    
    // Ограничиваем размер запроса
    if (length > POOL_SIZE)
        length = POOL_SIZE;
    
    if (length == 0) {
        spin_unlock_irqrestore(&state->lock, flags);
        return 0;
    }
    
    bytes_to_read = length;
    
    // Выделяем временный буфер
    spin_unlock_irqrestore(&state->lock, flags);
    
    temp_buf = kmalloc(bytes_to_read, GFP_KERNEL);
    if (!temp_buf) {
        return -ENOMEM;
    }
    
    spin_lock_irqsave(&state->lock, flags);
    
    // Перемешиваем перед чтением
    for (i = 0; i < POOL_SIZE - 1; i++) {
        state->pool[i] ^= state->pool[i + 1];
    }
    if (POOL_SIZE > 0) {
        state->pool[POOL_SIZE - 1] ^= state->pool[0];
    }
    
    // Копируем данные из пула
    for (i = 0; i < bytes_to_read; i++) {
        temp_buf[i] = state->pool[state->pool_index];
        state->pool_index = (state->pool_index + 1) % POOL_SIZE;
    }
    
    // Уменьшаем счетчик энтропии
    if (state->entropy_count > bytes_to_read * 8)
        state->entropy_count -= bytes_to_read * 8;
    else
        state->entropy_count = 0;
    
    spin_unlock_irqrestore(&state->lock, flags);
    
    // Копируем в пользовательское пространство
    if (copy_to_user(buffer, temp_buf, bytes_to_read)) {
        kfree(temp_buf);
        return -EFAULT;
    }
    
    kfree(temp_buf);
    
    printk(KERN_INFO "entropy_mouse: Read %zd bytes (events: %d, entropy left: %u)\n", 
           bytes_to_read, state->mouse_events, state->entropy_count);
    return bytes_to_read;
}

// Инициализация модуля
static int __init entropy_driver_init(void)
{
    int retval;
    dev_t dev_num;
    
    printk(KERN_INFO "entropy_mouse: Initializing driver (DEBUG VERSION)...\n");
    
    // Выделяем состояние драйвера
    state = kzalloc(sizeof(struct entropy_state), GFP_KERNEL);
    if (!state) {
        printk(KERN_ERR "entropy_mouse: Failed to allocate state\n");
        return -ENOMEM;
    }
    
    // Инициализируем состояние
    spin_lock_init(&state->lock);
    state->pool_index = 0;
    state->entropy_count = MIN_ENTROPY_BITS; // Начинаем с минимума
    state->mouse_events = 0;
    
    // Заполняем пул начальными данными
    get_random_bytes(state->pool, POOL_SIZE);
    
    // Настраиваем обработчик ввода
    state->input_handler.event = entropy_event;
    state->input_handler.connect = entropy_connect;
    state->input_handler.disconnect = entropy_disconnect;
    state->input_handler.name = "entropy_mouse";
    state->input_handler.id_table = entropy_ids;
    
    // Регистрируем обработчик ввода
    retval = input_register_handler(&state->input_handler);
    if (retval) {
        printk(KERN_ERR "entropy_mouse: Failed to register input handler: %d\n", retval);
        goto free_state;
    }
    
    // Регистрируем символьное устройство
    retval = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (retval < 0) {
        printk(KERN_ERR "entropy_mouse: Failed to allocate device number\n");
        goto unregister_handler;
    }
    
    major_num = MAJOR(dev_num);
    
    // Инициализируем cdev
    cdev_init(&entropy_cdev, &fops);
    entropy_cdev.owner = THIS_MODULE;
    
    // Добавляем cdev в систему
    retval = cdev_add(&entropy_cdev, dev_num, 1);
    if (retval < 0) {
        printk(KERN_ERR "entropy_mouse: Failed to add cdev\n");
        goto unregister_chrdev;
    }
    
    // Создаем класс устройства
    entropy_class = class_create(CLASS_NAME);
    if (IS_ERR(entropy_class)) {
        retval = PTR_ERR(entropy_class);
        printk(KERN_ERR "entropy_mouse: Failed to create class\n");
        goto del_cdev;
    }
    
    // Создаем устройство
    entropy_device = device_create(entropy_class, NULL, dev_num, 
                                   NULL, DEVICE_NAME);
    if (IS_ERR(entropy_device)) {
        retval = PTR_ERR(entropy_device);
        printk(KERN_ERR "entropy_mouse: Failed to create device\n");
        goto destroy_class;
    }
    
    printk(KERN_INFO "entropy_mouse: DEBUG Driver initialized (major: %d)\n", major_num);
    printk(KERN_INFO "entropy_mouse: Device: /dev/%s\n", DEVICE_NAME);
    printk(KERN_INFO "entropy_mouse: Initial entropy: %u bits\n", state->entropy_count);
    
    return 0;
    
destroy_class:
    class_destroy(entropy_class);
del_cdev:
    cdev_del(&entropy_cdev);
unregister_chrdev:
    unregister_chrdev_region(dev_num, 1);
unregister_handler:
    input_unregister_handler(&state->input_handler);
free_state:
    kfree(state);
    state = NULL;
    
    return retval;
}

// Выгрузка модуля
static void __exit entropy_driver_exit(void)
{
    dev_t dev_num = MKDEV(major_num, 0);
    
    printk(KERN_INFO "entropy_mouse: Unloading DEBUG driver...\n");
    
    // Отменяем регистрацию обработчика ввода
    input_unregister_handler(&state->input_handler);
    
    // Удаляем устройство
    if (entropy_device) {
        device_destroy(entropy_class, dev_num);
    }
    
    // Удаляем класс
    if (entropy_class) {
        class_destroy(entropy_class);
    }
    
    // Удаляем cdev
    cdev_del(&entropy_cdev);
    
    // Освобождаем номер устройства
    unregister_chrdev_region(dev_num, 1);
    
    // Освобождаем состояние
    if (state) {
        // Очищаем чувствительные данные
        memset(state->pool, 0, POOL_SIZE);
        printk(KERN_INFO "entropy_mouse: Total mouse events captured: %d\n", state->mouse_events);
        kfree(state);
        state = NULL;
    }
    
    printk(KERN_INFO "entropy_mouse: DEBUG Driver unloaded\n");
}

module_init(entropy_driver_init);
module_exit(entropy_driver_exit);