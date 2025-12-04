
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
#define MIN_ENTROPY_BITS 8          // Минимальное количество бит энтропии перед чтением

MODULE_LICENSE("GPL");            
MODULE_AUTHOR("Maxim Panfilov");    
MODULE_DESCRIPTION("Mouse entropy collector driver");  
MODULE_VERSION("3.0");              

// Глобальные переменные драйвера
static int major_num;                 // Старший номер устройства 
static struct class* entropy_class = NULL;  // Класс устройства в sysfs
static struct device* entropy_device = NULL; // Само устройство в sysfs
static struct cdev entropy_cdev;     // Структура символьного устройства

/*
 * Структура состояния драйвера.
 */
struct entropy_state {
    unsigned char pool[POOL_SIZE];   // Пул энтропии (256 байт)
    int pool_index;                  // Текущая позиция в пуле
    unsigned int entropy_count;      // Счетчик бит энтропии (оценка)
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
 * Определяет, к каким устройствам подключаться.
 * Используем два фильтра:
 * 1. Устройства, генерирующие события относительного движения (EV_REL) - мыши
 * 2. Устройства, генерирующие события клавиш (EV_KEY) - кнопки мыши
 */
static const struct input_device_id entropy_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,  // Фильтр по типам событий
        .evbit = { BIT_MASK(EV_REL) },         // События относительного движения (движение мыши)
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = { BIT_MASK(EV_KEY) },         // События клавиш (клики мыши)
    },
    { }, // Завершающий элемент (обязательно)
};

/*
 * Обработчик событий мыши.
 * Вызывается каждый раз, когда мышь генерирует событие.
 * Эта функция собирает энтропию из событий мыши.
 */
static void entropy_event(struct input_handle *handle, unsigned int type,
                         unsigned int code, int value)
{
    unsigned long flags;              // Флаги для сохранения состояния прерываний
    static unsigned int event_counter = 0;  // Статический счетчик событий для отладки
    
    // Проверяем, инициализирован ли state
    if (!state) return;
    
    event_counter++;  // Увеличиваем счетчик событий
    
    // Логируем все события для отладки
    printk(KERN_DEBUG "entropy_mouse: Event #%u: type=%u code=%u value=%d\n",
           event_counter, type, code, value);
    
    /*
     * Блокируем спинлок для безопасного доступа к общим данным.
     * spin_lock_irqsave сохраняет текущее состояние прерываний.
     * Это нужно, чтобы событие могло прервать выполнение в любом месте.
     */
    spin_lock_irqsave(&state->lock, flags);
    
    /*
     * Добавляем энтропию в пул:
     * 1. type - тип события (движение или клик)
     * 2. code - код события (REL_X для движения по X)
     * 3. value - значение события (величина движения)
     * 4. jiffies - текущее время системы (добавляет случайность)
     * 
     * Используем операцию XOR для смешивания битов.
     * Каждый байт маскируется 0xFF для взятия только младшего байта.
     */
    state->pool[state->pool_index] ^= (type & 0xFF);
    state->pool[state->pool_index] ^= (code & 0xFF);
    state->pool[state->pool_index] ^= (value & 0xFF);
    state->pool[state->pool_index] ^= (jiffies & 0xFF);
    
    // Перемещаем индекс по кругу (циклический буфер)
    state->pool_index = (state->pool_index + 1) % POOL_SIZE;
    state->mouse_events++;  // Увеличиваем счетчик событий
    
    /*
     * Увеличиваем счетчик энтропии.
     * простая эвристика: 8 бит (1 байт) энтропии на событие.
     */
    state->entropy_count += 8;
    
    /*
     * Простое перемешивание пула.
     * Выполняем каждые 8 событий для лучшего распределения энтропии.
     * Проходим по всему пулу и XOR'им каждый элемент со следующим.
     */
    if (state->pool_index % 8 == 0) {
        int i;
        for (i = 0; i < POOL_SIZE - 1; i++) {
            state->pool[i] ^= state->pool[i + 1];
        }
        // Последний элемент XOR'им с первым для замыкания цикла
        if (POOL_SIZE > 0) {
            state->pool[POOL_SIZE - 1] ^= state->pool[0];
        }
    }
    
    // Разблокируем спинлок и восстанавливаем состояние прерываний
    spin_unlock_irqrestore(&state->lock, flags);
    
    // Логируем успешное добавление энтропии
    printk(KERN_INFO "entropy_mouse: Added entropy from mouse event. Total events: %d, entropy: %u bits\n",
           state->mouse_events, state->entropy_count);
}

/*
 * Функция подключения к устройству ввода.
 * Вызывается, когда ядро находит устройство, соответствующее нашим фильтрам.
 */
static int entropy_connect(struct input_handler *handler,
                          struct input_dev *dev,
                          const struct input_device_id *id)
{
    struct input_handle *handle;  // Структура для связи с устройством
    int error;                    // Код ошибки
    
    printk(KERN_INFO "entropy_mouse: CONNECTING to: %s (EV bits: %lx)\n", 
           dev->name, dev->evbit[0]);
    
    // Выделяем память для handle
    handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
    if (!handle)
        return -ENOMEM;  // Ошибка: не хватило памяти
    
    // Настраиваем handle
    handle->dev = dev;              // Указатель на устройство
    handle->handler = handler;      // Наш обработчик
    handle->name = "entropy_mouse"; // Имя для отладки
    
    // Регистрируем handle в подсистеме ввода
    error = input_register_handle(handle);
    if (error) {
        printk(KERN_ERR "entropy_mouse: Failed to register handle: %d\n", error);
        kfree(handle);  // Освобождаем память
        return error;
    }
    
    // Открываем устройство для получения событий
    error = input_open_device(handle);
    if (error) {
        printk(KERN_ERR "entropy_mouse: Failed to open device: %d\n", error);
        input_unregister_handle(handle);  // Отменяем регистрацию
        kfree(handle);                    // Освобождаем память
        return error;
    }
    
    printk(KERN_INFO "entropy_mouse: SUCCESSFULLY connected to %s\n", dev->name);
    return 0;  // Успех
}

/*
 * Функция отключения от устройства ввода.
 * Вызывается при отключении устройства или выгрузке драйвера.
 */
static void entropy_disconnect(struct input_handle *handle)
{
    printk(KERN_INFO "entropy_mouse: Disconnecting from %s\n", handle->dev->name);
    input_close_device(handle);      // Закрываем устройство
    input_unregister_handle(handle); // Отменяем регистрацию
    kfree(handle);                   // Освобождаем память
}

/*
 * Функция открытия устройства.
 */
static int device_open(struct inode *inode, struct file *file)
{
    // Просто логируем открытие устройства
    printk(KERN_INFO "entropy_mouse: Device opened (entropy: %u bits)\n", 
           state ? state->entropy_count : 0);
    return 0;  // Всегда успех
}

/*
 * Функция закрытия устройства.
 */
static int device_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "entropy_mouse: Device closed\n");
    return 0;  // Всегда успех
}

/*
 * Функция чтения из устройства.
 * Возвращает случайные данные, собранные из событий мыши.
 */
static ssize_t device_read(struct file *filp, char __user *buffer,
                          size_t length, loff_t *offset)
{
    unsigned char *temp_buf;     // Временный буфер в ядре
    unsigned long flags;         // Флаги для спинлока
    ssize_t bytes_to_read;       // Сколько байт будем читать
    int i;                       // Счетчик цикла
    
    // Проверяем, инициализирован ли state
    if (!state) {
        printk(KERN_ERR "entropy_mouse: No state!\n");
        return -ENODEV;  // Ошибка: устройство не существует
    }
    
    // Блокируем доступ к пулу
    spin_lock_irqsave(&state->lock, flags);
    
    
    // Ограничиваем размер запроса размером пула
    if (length > POOL_SIZE)
        length = POOL_SIZE;
    
    // Проверяем нулевой запрос
    if (length == 0) {
        spin_unlock_irqrestore(&state->lock, flags);
        return 0;  // Ничего не читаем
    }
    
    bytes_to_read = length;  // Определяем, сколько будем читать
    
    /*
     * Важно: выделяем память вне спинлока!
     * kmalloc может спать (вызывать планировщик), а спинлоки нельзя держать долго.
     */
    
    temp_buf = kmalloc(bytes_to_read, GFP_KERNEL);
    if (!temp_buf) {
        return -ENOMEM;  // Ошибка: не хватило памяти
    }
    
    
    // Копируем данные из пула во временный буфер
    for (i = 0; i < bytes_to_read; i++) {
        temp_buf[i] = state->pool[state->pool_index];
        state->pool_index = (state->pool_index + 1) % POOL_SIZE;  // Двигаем индекс
    }
    
    
    spin_unlock_irqrestore(&state->lock, flags);
    
    /*
     * Копируем данные из ядра в пользовательское пространство.
     * copy_to_user проверяет валидность указателя пользователя.
     */
    if (copy_to_user(buffer, temp_buf, bytes_to_read)) {
        kfree(temp_buf);  // Освобождаем память
        return -EFAULT;   // Ошибка: неверный адрес в пользовательском пространстве
    }
    
    kfree(temp_buf);  // Освобождаем временный буфер
    
    printk(KERN_INFO "entropy_mouse: Read %zd bytes (events: %d, entropy left: %u)\n", 
           bytes_to_read, state->mouse_events, state->entropy_count);
    return bytes_to_read;  // Возвращаем количество прочитанных байт
}

/*
 * Функция инициализации модуля.
 * Вызывается при загрузке модуля (insmod).
 * Макрос __init указывает, что функция используется только при инициализации.
 */
static int __init entropy_driver_init(void)
{
    int retval;    // Переменная для кодов возврата
    dev_t dev_num; // Номер устройства (старший + младший)
    
    printk(KERN_INFO "entropy_mouse: Initializing driver (DEBUG VERSION)...\n");
    
    /*
     * 1. Выделяем память для состояния драйвера.
     * kzalloc выделяет и обнуляет память.
     * GFP_KERNEL - флаг
     */
    state = kzalloc(sizeof(struct entropy_state), GFP_KERNEL);
    if (!state) {
        printk(KERN_ERR "entropy_mouse: Failed to allocate state\n");
        return -ENOMEM;  // Ошибка выделения памяти
    }
    
    /*
     * 2. Инициализируем состояние драйвера.
     */
    spin_lock_init(&state->lock);          // Инициализируем спинлок
    state->pool_index = 0;                 // Начинаем с начала пула
    state->entropy_count = MIN_ENTROPY_BITS;  // Начальное значение энтропии
    state->mouse_events = 0;               // Пока событий не было
    
    // 3. Заполняем пул начальными случайными данными
    get_random_bytes(state->pool, POOL_SIZE);
    
    /*
     * 4. Настраиваем обработчик событий ввода.
     * Указываем функции-обработчики для разных событий.
     */
    state->input_handler.event = entropy_event;      // Обработчик событий мыши
    state->input_handler.connect = entropy_connect;  // Подключение к устройству
    state->input_handler.disconnect = entropy_disconnect;  // Отключение
    state->input_handler.name = "entropy_mouse";     // Имя обработчика
    state->input_handler.id_table = entropy_ids;     // Таблица фильтров устройств
    
    // 5. Регистрируем обработчик в подсистеме ввода
    retval = input_register_handler(&state->input_handler);
    if (retval) {
        printk(KERN_ERR "entropy_mouse: Failed to register input handler: %d\n", retval);
        goto free_state;  // Переходим к очистке
    }
    
    /*
     * 6. Регистрируем символьное устройство.
     * alloc_chrdev_region выделяет диапазон номеров устройств.
     * Параметры: &dev_num (возвращаемый номер), 0 (начальный минор),
     *            1 (сколько миноров), DEVICE_NAME (имя).
     */
    retval = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (retval < 0) {
        printk(KERN_ERR "entropy_mouse: Failed to allocate device number\n");
        goto unregister_handler;  // Очистка: отмена регистрации обработчика
    }
    
    major_num = MAJOR(dev_num);  // Сохраняем старший номер
    
    /*
     * 7. Инициализируем структуру символьного устройства (cdev).
     * cdev_init связывает cdev с файловыми операциями.
     */
    cdev_init(&entropy_cdev, &fops);
    entropy_cdev.owner = THIS_MODULE;  // Владелец модуля
    
    /*
     * 8. Добавляем cdev в систему.
     * cdev_add делает устройство доступным в системе.
     */
    retval = cdev_add(&entropy_cdev, dev_num, 1);
    if (retval < 0) {
        printk(KERN_ERR "entropy_mouse: Failed to add cdev\n");
        goto unregister_chrdev;  // Очистка: освобождение номера устройства
    }
    
    /*
     * 9. Создаем класс устройства в sysfs.
     * Класс создает директорию в /sys/class/.
     */
    entropy_class = class_create(CLASS_NAME);
    if (IS_ERR(entropy_class)) {
        retval = PTR_ERR(entropy_class);
        printk(KERN_ERR "entropy_mouse: Failed to create class\n");
        goto del_cdev;  // Очистка: удаление cdev
    }
    
    /*
     * 10. Создаем само устройство в sysfs.
     * device_create создает файл устройства и связывает с классом.
     */
    entropy_device = device_create(entropy_class, NULL, dev_num, 
                                   NULL, DEVICE_NAME);
    if (IS_ERR(entropy_device)) {
        retval = PTR_ERR(entropy_device);
        printk(KERN_ERR "entropy_mouse: Failed to create device\n");
        goto destroy_class;  // Очистка: удаление класса
    }
    
    // 11. Успешная инициализация
    printk(KERN_INFO "entropy_mouse: DEBUG Driver initialized (major: %d)\n", major_num);
    printk(KERN_INFO "entropy_mouse: Device: /dev/%s\n", DEVICE_NAME);
    printk(KERN_INFO "entropy_mouse: Initial entropy: %u bits\n", state->entropy_count);
    
    return 0;  // Успех

/*
 * Метки для отката (rollback) при ошибках.
 * Выполняются в обратном порядке инициализации.
 */
destroy_class:
    class_destroy(entropy_class);  // Удаляем класс
del_cdev:
    cdev_del(&entropy_cdev);       // Удаляем cdev
unregister_chrdev:
    unregister_chrdev_region(dev_num, 1);  // Освобождаем номер устройства
unregister_handler:
    input_unregister_handler(&state->input_handler);  // Отменяем регистрацию обработчика
free_state:
    kfree(state);  // Освобождаем память состояния
    state = NULL;  // Обнуляем указатель
    
    return retval;  // Возвращаем код ошибки
}

/*
 * Функция выгрузки модуля.
 * Вызывается при выгрузке модуля (rmmod).
 * Макрос __exit указывает, что функция используется только при выгрузке.
 */
static void __exit entropy_driver_exit(void)
{
    // Создаем номер устройства из сохраненного старшего номера
    dev_t dev_num = MKDEV(major_num, 0);
    
    printk(KERN_INFO "entropy_mouse: Unloading DEBUG driver...\n");
    
    // 1. Отменяем регистрацию обработчика ввода
    input_unregister_handler(&state->input_handler);
    
    // 2. Удаляем устройство из sysfs
    if (entropy_device) {
        device_destroy(entropy_class, dev_num);
    }
    
    // 3. Удаляем класс из sysfs
    if (entropy_class) {
        class_destroy(entropy_class);
    }
    
    // 4. Удаляем символьное устройство из системы
    cdev_del(&entropy_cdev);
    
    // 5. Освобождаем номер устройства
    unregister_chrdev_region(dev_num, 1);
    
    // 6. Освобождаем состояние драйвера
    if (state) {
        /*
         * Важно: очищаем чувствительные данные (пул энтропии).
         * memset гарантирует, что случайные данные не останутся в памяти.
         */
        memset(state->pool, 0, POOL_SIZE);
        
        // Выводим статистику
        printk(KERN_INFO "entropy_mouse: Total mouse events captured: %d\n", state->mouse_events);
        
        kfree(state);  // Освобождаем память
        state = NULL;  // Обнуляем указатель
    }
    
    printk(KERN_INFO "entropy_mouse: DEBUG Driver unloaded\n");
}

/*
 * Макросы для указания функций инициализации и выгрузки.
 * module_init указывает функцию, вызываемую при загрузке модуля.
 * module_exit указывает функцию, вызываемую при выгрузке модуля.
 */
module_init(entropy_driver_init);
module_exit(entropy_driver_exit);