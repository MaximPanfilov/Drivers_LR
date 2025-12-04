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
#define POOL_SIZE 256                // Размер пула энтропии (256 байт)

MODULE_LICENSE("GPL");            
MODULE_AUTHOR("Maxim Panfilov");    
MODULE_DESCRIPTION("Mouse entropy collector driver");  
MODULE_VERSION("4.0");              // Обновляем версию

// Глобальные переменные драйвера
static int major_num;                 // Старший номер устройства 
static struct class* entropy_class = NULL;  // Класс устройства в sysfs
static struct device* entropy_device = NULL; // Само устройство в sysfs
static struct cdev entropy_cdev;     // Структура символьного устройства

/*
 * Структура состояния драйвера.
 * УПРОЩЕННАЯ: без счетчика энтропии
 */
struct entropy_state {
    unsigned char pool[POOL_SIZE];   // Пул энтропии (256 байт)
    int pool_index;                  // Текущая позиция в пуле
    spinlock_t lock;                 // Спинлок для синхронизации доступа к пулу
    struct input_handler input_handler;  // Обработчик событий ввода (мыши)
    int mouse_events;                // Счетчик событий мыши для статистики
};

// Указатель на состояние драйвера 
static struct entropy_state *state;

// Объявления функций 
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *);
static void entropy_event(struct input_handle *handle, unsigned int type,
                         unsigned int code, int value);
static int entropy_connect(struct input_handler *handler,
                          struct input_dev *dev,
                          const struct input_device_id *id);
static void entropy_disconnect(struct input_handle *handle);

/*
 * Структура файловых операций.
 */
static struct file_operations fops = {
    .owner = THIS_MODULE,        
    .open = device_open,            
    .release = device_release,     
    .read = device_read,           
};

/*
 * Таблица идентификаторов устройств ввода.
 */
static const struct input_device_id entropy_ids[] = {
    // Устройства с относительным движением (мыши, тачпады)
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = { BIT_MASK(EV_REL) },
    },
    // Устройства с кнопками мыши (на всякий случай)
    {
        .flags = INPUT_DEVICE_ID_MATCH_KEYBIT,
        .keybit = { [BIT_WORD(BTN_LEFT)] = BIT_MASK(BTN_LEFT) },
    },
    { },
};
/*
 * Обработчик событий мыши.
 */
static void entropy_event(struct input_handle *handle, unsigned int type,
                         unsigned int code, int value)
{
    unsigned long flags;
    static unsigned int event_counter = 0;
    
    if (!state) return;
    
    event_counter++;
    
    if (type == 0) {
        return;  // Не добавляем энтропию из SYN events
    }
    
    // Логируем только интересные события
    printk(KERN_DEBUG "entropy_mouse: Event #%u: type=%u code=%u value=%d\n",
           event_counter, type, code, value);
    
    // Блокируем спинлок для безопасного доступа
    spin_lock_irqsave(&state->lock, flags);
    
    /*
     * Добавляем энтропию в пул.
     * Используем XOR для смешивания битов.
     */
    state->pool[state->pool_index] ^= (type & 0xFF);
    state->pool[state->pool_index] ^= (code & 0xFF);
    state->pool[state->pool_index] ^= (value & 0xFF);
    
    // Перемещаем индекс по кругу (циклический буфер)
    state->pool_index = (state->pool_index + 1) % POOL_SIZE;
    state->mouse_events++;  // Увеличиваем счетчик событий для статистики
    
    /*
     * Простое перемешивание пула.
     * Выполняем каждые 16 событий для лучшего распределения энтропии.
     */
    if (state->pool_index % 16 == 0) {
        int i;
        for (i = 0; i < POOL_SIZE - 1; i++) {
            state->pool[i] ^= state->pool[i + 1];
        }
        // Последний элемент XOR'им с первым для замыкания цикла
        if (POOL_SIZE > 0) {
            state->pool[POOL_SIZE - 1] ^= state->pool[0];
        }
    }
    
    // Разблокируем спинлок
    spin_unlock_irqrestore(&state->lock, flags);
    
    // Логируем добавление данных
    printk(KERN_INFO "entropy_mouse: Added data from mouse event. Total events: %d\n",
           state->mouse_events);
}

/*
 * Функция подключения к устройству ввода.
 */
static int entropy_connect(struct input_handler *handler,
                          struct input_dev *dev,
                          const struct input_device_id *id)
{
    struct input_handle *handle;
    int error;
    
    printk(KERN_INFO "entropy_mouse: Connecting to: %s\n", dev->name);
    
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
    
    printk(KERN_INFO "entropy_mouse: Successfully connected to %s\n", dev->name);
    return 0;
}

/*
 * Функция отключения от устройства ввода.
 */
static void entropy_disconnect(struct input_handle *handle)
{
    printk(KERN_INFO "entropy_mouse: Disconnecting from %s\n", handle->dev->name);
    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);
}

/*
 * Функция открытия устройства.
 */
static int device_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "entropy_mouse: Device opened (total events: %d)\n", 
           state ? state->mouse_events : 0);
    return 0;
}

/*
 * Функция закрытия устройства.
 */
static int device_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "entropy_mouse: Device closed\n");
    return 0;
}

/*
 * Функция чтения из устройства.
 */
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
    
    // Блокируем доступ к пулу
    spin_lock_irqsave(&state->lock, flags);
    
    // Ограничиваем размер запроса размером пула
    if (length > POOL_SIZE)
        length = POOL_SIZE;
    
    if (length == 0) {
        spin_unlock_irqrestore(&state->lock, flags);
        return 0;
    }
    
    bytes_to_read = length;
    

    temp_buf = kmalloc(bytes_to_read, GFP_KERNEL);
    if (!temp_buf) {
        return -ENOMEM;
    }
    
    
    // Копируем данные из пула во временный буфер
    for (i = 0; i < bytes_to_read; i++) {
        temp_buf[i] = state->pool[state->pool_index];
        state->pool_index = (state->pool_index + 1) % POOL_SIZE;
    }
    
    spin_unlock_irqrestore(&state->lock, flags);
    
    // Копируем в пользовательское пространство
    if (copy_to_user(buffer, temp_buf, bytes_to_read)) {
        kfree(temp_buf);
        return -EFAULT;
    }
    
    kfree(temp_buf);
    
    printk(KERN_INFO "entropy_mouse: Read %zd bytes (total events: %d)\n", 
           bytes_to_read, state->mouse_events);
    return bytes_to_read;
}

/*
 * Функция инициализации модуля.
 */
static int __init entropy_driver_init(void)
{
    int retval;
    dev_t dev_num;
    
    printk(KERN_INFO "entropy_mouse: Initializing driver (Simplified version 4.0)...\n");
    
    // Выделяем память для состояния драйвера
    state = kzalloc(sizeof(struct entropy_state), GFP_KERNEL);
    if (!state) {
        printk(KERN_ERR "entropy_mouse: Failed to allocate state\n");
        return -ENOMEM;
    }
    
    // Инициализируем состояние драйвера
    spin_lock_init(&state->lock);
    state->pool_index = 0;
    state->mouse_events = 0;
    
    // Заполняем пул начальными случайными данными
    get_random_bytes(state->pool, POOL_SIZE);
    
    // Настраиваем обработчик событий ввода
    state->input_handler.event = entropy_event;
    state->input_handler.connect = entropy_connect;
    state->input_handler.disconnect = entropy_disconnect;
    state->input_handler.name = "entropy_mouse";
    state->input_handler.id_table = entropy_ids;
    
    // Регистрируем обработчик в подсистеме ввода
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
    
    // Создаем класс устройства в sysfs
    entropy_class = class_create(CLASS_NAME);
    if (IS_ERR(entropy_class)) {
        retval = PTR_ERR(entropy_class);
        printk(KERN_ERR "entropy_mouse: Failed to create class\n");
        goto del_cdev;
    }
    
    // Создаем само устройство в sysfs
    entropy_device = device_create(entropy_class, NULL, dev_num, 
                                   NULL, DEVICE_NAME);
    if (IS_ERR(entropy_device)) {
        retval = PTR_ERR(entropy_device);
        printk(KERN_ERR "entropy_mouse: Failed to create device\n");
        goto destroy_class;
    }
    
    printk(KERN_INFO "entropy_mouse: Driver initialized (major: %d)\n", major_num);
    printk(KERN_INFO "entropy_mouse: Device: /dev/%s\n", DEVICE_NAME);
    printk(KERN_INFO "entropy_mouse: Pool size: %d bytes\n", POOL_SIZE);
    
    return 0;

/*
 * Откат при ошибках (ROLLBACK)
 */
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

/*
 * Функция выгрузки модуля.
 */
static void __exit entropy_driver_exit(void)
{
    dev_t dev_num = MKDEV(major_num, 0);
    
    printk(KERN_INFO "entropy_mouse: Unloading driver...\n");
    
    // Отменяем регистрацию обработчика ввода
    input_unregister_handler(&state->input_handler);
    
    // Удаляем устройство из sysfs
    if (entropy_device) {
        device_destroy(entropy_class, dev_num);
    }
    
    // Удаляем класс из sysfs
    if (entropy_class) {
        class_destroy(entropy_class);
    }
    
    // Удаляем символьное устройство из системы
    cdev_del(&entropy_cdev);
    
    // Освобождаем номер устройства
    unregister_chrdev_region(dev_num, 1);
    
    // Освобождаем состояние драйвера
    if (state) {
        // Очищаем чувствительные данные
        memset(state->pool, 0, POOL_SIZE);
        
        // Выводим статистику
        printk(KERN_INFO "entropy_mouse: Total mouse events captured: %d\n", 
               state->mouse_events);
        
        kfree(state);
        state = NULL;
    }
    
    printk(KERN_INFO "entropy_mouse: Driver unloaded\n");
}

module_init(entropy_driver_init);
module_exit(entropy_driver_exit);