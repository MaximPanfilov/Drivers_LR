
// scull_ring.c
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/atomic.h>

#define DEVICE_NAME "scull_ring"
#define SCULL_RING_BUFFER_SIZE 256        // Размер каждого кольцевого буфера
#define SCULL_RING_NR_DEVS 3              // Количество устройств: scull_ring0,1,2

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxim_Panfilov"); 
MODULE_DESCRIPTION("Scull Driver for LR1");

// Структура кольцевого буфера с синхронизацией
struct scull_ring_buffer {
    char *data;              // Указатель на данные буфера
    int size;                // Общий размер буфера
    int read_pos;            // Текущая позиция чтения (голова)
    int write_pos;           // Текущая позиция записи (хвост)
    int data_len;            // Текущее количество данных в буфере
    struct mutex lock;       // Мьютекс для защиты от гонок данных
    wait_queue_head_t read_queue;   // Очередь ожидания для читателей (когда буфер пуст)
    wait_queue_head_t write_queue;  // Очередь ожидания для писателей (когда буфер полон)
    atomic_t read_count;     // Атомарный счетчик операций чтения
    atomic_t write_count;    // Атомарный счетчик операций записи
};

// Структура устройства
struct scull_ring_dev {
    struct scull_ring_buffer *ring_buf;  // Указатель на кольцевой буфер
    struct cdev cdev;                    // Структура символьного устройства
};

static int scull_ring_major = 0;         // Основной номер устройства (0 = автоназначение)
module_param(scull_ring_major, int, S_IRUGO);

// Массив устройств (3 устройства: scull_ring0, scull_ring1, scull_ring2)
static struct scull_ring_dev scull_ring_devices[SCULL_RING_NR_DEVS];

// Определения IOCTL команд для взаимодействия с пользовательским пространством
#define SCULL_RING_IOCTL_GET_STATUS _IOR('s', 1, int[4])      // Получить статус буфера
#define SCULL_RING_IOCTL_GET_COUNTERS _IOR('s', 2, long[2])   // Получить счетчики операций
#define SCULL_RING_IOCTL_PEEK_BUFFER _IOWR('s', 10, char[512]) // Заглянуть в содержимое буфера

// Прототипы функций файловых операций
static int scull_ring_open(struct inode *inode, struct file *filp);
static int scull_ring_release(struct inode *inode, struct file *filp);
static ssize_t scull_ring_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static ssize_t scull_ring_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static long scull_ring_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

// Структура файловых операций (точки входа драйвера)
static struct file_operations scull_ring_fops = {
    .owner = THIS_MODULE,
    .open = scull_ring_open,
    .release = scull_ring_release,
    .read = scull_ring_read,
    .write = scull_ring_write,
    .unlocked_ioctl = scull_ring_ioctl,
};

/**
 * Инициализация кольцевого буфера
 * @buf: указатель на структуру буфера
 * @size: размер буфера в байтах
 * Возвращает 0 при успехе, отрицательный код ошибки при failure
 */
static int scull_ring_buffer_init(struct scull_ring_buffer *buf, int size) {
    // Выделение памяти под данные буфера в пространстве ядра
    buf->data = kmalloc(size, GFP_KERNEL);
    if (!buf->data) {
        printk(KERN_ERR "scull_ring: Failed to allocate buffer memory\n");
        return -ENOMEM;
    }
    
    // Инициализация полей структуры
    buf->size = size;
    buf->read_pos = 0;
    buf->write_pos = 0;
    buf->data_len = 0;
    
    // Инициализация механизмов синхронизации
    mutex_init(&buf->lock);                          // Инициализация мьютекса
    init_waitqueue_head(&buf->read_queue);           // Очередь для читателей
    init_waitqueue_head(&buf->write_queue);          // Очередь для писателей
    
    // Инициализация атомарных счетчиков
    atomic_set(&buf->read_count, 0);
    atomic_set(&buf->write_count, 0);
    
    printk(KERN_INFO "scull_ring: Buffer initialized with size %d\n", size);
    return 0;
}

/**
 * Очистка буфера и освобождение ресурсов
 */
static void scull_ring_buffer_cleanup(struct scull_ring_buffer *buf) {
    kfree(buf->data);  // Освобождение памяти данных буфера
    printk(KERN_INFO "scull_ring: Buffer cleanup completed\n");
}

/**
 * Поиск нуль-терминатора в кольцевом буфере
 * @buf: указатель на буфер
 * @start_pos: начальная позиция поиска
 * @max_len: максимальная длина поиска
 * Возвращает длину сообщения включая нуль-терминатор, или -1 если не найден
 * 
 * Эта функция необходима для обработки строк в кольцевом буфере, так как
 * сообщения разделяются нуль-терминаторами.
 */
static int find_null_terminator(struct scull_ring_buffer *buf, int start_pos, int max_len) {
    int pos = start_pos;
    int bytes_checked = 0;
    
    // Поиск нуль-терминатора в пределах max_len или доступных данных
    while (bytes_checked < max_len && bytes_checked < buf->data_len) {
        if (buf->data[pos] == '\0') {
            return bytes_checked + 1; // Возвращаем длину включая нуль-терминатор
        }
        pos = (pos + 1) % buf->size;  // Циклическое перемещение по буферу
        bytes_checked++;
    }
    return -1; // Нуль-терминатор не найден
}

/**
 * Извлечение всех сообщений из буфера для отладки через IOCTL
 * @buf: указатель на буфер
 * @output: буфер для результата
 * @output_size: размер выходного буфера
 * Возвращает количество извлеченных сообщений
 * 
 * Функция используется командой PEEK_BUFFER для показа содержимого буфера
 * без извлечения данных (только чтение).
 */
static int extract_messages(struct scull_ring_buffer *buf, char *output, int output_size) {
    int pos = buf->read_pos;      // Начинаем с текущей позиции чтения
    int bytes_processed = 0;
    int message_count = 0;
    int output_used = 0;
    
    // Если буфер пуст
    if (buf->data_len == 0) {
        snprintf(output, output_size, "Empty");
        return 0;
    }
    
    // Начало вывода в формате списка
    output_used += snprintf(output + output_used, output_size - output_used, "[");
    
    // Извлечение всех полных сообщений из буфера
    while (bytes_processed < buf->data_len && output_used < output_size - 20) {
        // Поиск следующего сообщения (до нуль-терминатора)
        int message_len = find_null_terminator(buf, pos, buf->data_len - bytes_processed);
        if (message_len < 0) {
            // Полное сообщение не найдено - остались только частичные данные
            int remaining = buf->data_len - bytes_processed;
            if (remaining > 0) {
                // Можно добавить информацию о частичных данных, но закомментировано
                /* output_used += snprintf(output + output_used, output_size - output_used, 
                                      "partial:%dbytes", remaining); */
            }
            break;
        }
        
        // Извлечение сообщения из кольцевого буфера
        char message[20];
        int msg_bytes_copied = 0;
        for (int i = 0; i < message_len - 1 && i < 19; i++) {
            message[i] = buf->data[(pos + i) % buf->size];
            msg_bytes_copied++;
            
            // Защита от случайных нуль-терминаторов в середине сообщения
            if (message[i] == '\0') break;
        }
        message[msg_bytes_copied] = '\0';
        
        // Добавление сообщения в выходной буфер с разделителями
        if (message_count > 0) {
            output_used += snprintf(output + output_used, output_size - output_used, ", ");
        }
        output_used += snprintf(output + output_used, output_size - output_used, "%s", message);
        
        // Переход к следующему сообщению
        pos = (pos + message_len) % buf->size;
        bytes_processed += message_len;
        message_count++;
    }
    
    // Завершение форматированного вывода
    output_used += snprintf(output + output_used, output_size - output_used, "]");
    
    // Информация о непоместившихся данных
    if (bytes_processed < buf->data_len) {
        output_used += snprintf(output + output_used, output_size - output_used, 
                              " +%db more", buf->data_len - bytes_processed);
    }
    
    return message_count;
}

/**
 * Операция чтения из кольцевого буфера
 * @buf: указатель на буфер
 * @user_buf: буфер пользовательского пространства
 * @count: запрошенное количество байт
 * Возвращает количество прочитанных байт или код ошибки
 * 
 * Реализует блокирующее чтение: если данных нет, процесс блокируется
 * до появления данных или получения сигнала.
 */
static int scull_ring_buffer_read(struct scull_ring_buffer *buf, char __user *user_buf, size_t count) {
    int bytes_read = 0;
    int available;
    int to_end;
    int message_len;

    // Логирование начала операции чтения
    printk(KERN_INFO "scull_ring: Process %s (pid %d) attempting to read from buffer\n", 
           current->comm, current->pid);

    // Захват мьютекса с возможностью прерывания
    if (mutex_lock_interruptible(&buf->lock)) {
        printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while waiting for mutex lock (READ)\n", 
               current->comm, current->pid);
        return -ERESTARTSYS;
    }

    printk(KERN_INFO "scull_ring: Process %s (pid %d) acquired mutex lock for reading\n", 
           current->comm, current->pid);

    // БЛОКИРОВКА 1: Читатель ждет данных (буфер пустой)
    while (buf->data_len == 0) {
        printk(KERN_INFO "scull_ring: Process %s (pid %d) BLOCKED - buffer empty, waiting for data (data_len=0)\n", 
               current->comm, current->pid);
        
        // Освобождаем мьютекс перед блокировкой (чтобы писатели могли работать)
        mutex_unlock(&buf->lock);
        
        // Блокировка в очереди ожидания до появления данных
        if (wait_event_interruptible(buf->read_queue, (buf->data_len > 0))) {
            printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while waiting for data\n", 
                   current->comm, current->pid);
            return -ERESTARTSYS;
        }
        
        // Повторный захват мьютекса после пробуждения
        if (mutex_lock_interruptible(&buf->lock)) {
            printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while re-acquiring mutex after wait\n", 
                   current->comm, current->pid);
            return -ERESTARTSYS;
        }
        
        printk(KERN_INFO "scull_ring: Process %s (pid %d) UNBLOCKED - data available (data_len=%d)\n", 
               current->comm, current->pid, buf->data_len);
    }

    // Поиск полного сообщения (до нуль-терминатора)
    message_len = find_null_terminator(buf, buf->read_pos, buf->data_len);
    if (message_len < 0) {
        // Полное сообщение не найдено - читаем доступные данные
        message_len = (count < buf->data_len) ? count : buf->data_len;
    } else {
        // Найдено полное сообщение - ограничиваем чтение его длиной
        if (message_len > count) {
            message_len = count;
        }
    }

    // Проверка доступных данных
    available = buf->data_len;
    if (message_len > available) {
        message_len = available;
    }

    // Копирование данных из кольцевого буфера в пользовательское пространство
    // Обработка случая, когда данные пересекают границу буфера
    to_end = buf->size - buf->read_pos;
    if (message_len > to_end) {
        // Две операции копирования: от read_pos до конца и с начала буфера
        if (copy_to_user(user_buf, buf->data + buf->read_pos, to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
        if (copy_to_user(user_buf + to_end, buf->data, message_len - to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    } else {
        // Одна операция копирования
        if (copy_to_user(user_buf, buf->data + buf->read_pos, message_len)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    }

    // Обновление позиции чтения и количества данных
    buf->read_pos = (buf->read_pos + message_len) % buf->size;
    buf->data_len -= message_len;
    bytes_read = message_len;

    // Увеличение счетчика операций чтения
    atomic_inc(&buf->read_count);
    
    // Пробуждение ожидающих писателей (появилось свободное место)
    printk(KERN_INFO "scull_ring: Process %s (pid %d) read %d bytes, waking up writers (new data_len=%d)\n", 
           current->comm, current->pid, bytes_read, buf->data_len);
    
    wake_up_interruptible(&buf->write_queue);
    mutex_unlock(&buf->lock);
    
    printk(KERN_INFO "scull_ring: Process %s (pid %d) released mutex after reading\n", 
           current->comm, current->pid);
    return bytes_read;
}

/**
 * Операция записи в кольцевой буфер
 * @buf: указатель на буфер
 * @user_buf: буфер пользовательского пространства с данными
 * @count: количество байт для записи
 * Возвращает количество записанных байт или код ошибки
 * 
 * Реализует блокирующую запись: если буфер полон, процесс блокируется
 * до освобождения места или получения сигнала.
 */
static int scull_ring_buffer_write(struct scull_ring_buffer *buf, const char __user *user_buf, size_t count) {
    int bytes_written = 0;
    int available;
    int to_end;

    // Логирование начала операции записи
    printk(KERN_INFO "scull_ring: Process %s (pid %d) attempting to write %zu bytes to buffer\n", 
           current->comm, current->pid, count);

    // Захват мьютекса с возможностью прерывания
    if (mutex_lock_interruptible(&buf->lock)) {
        printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while waiting for mutex lock (WRITE)\n", 
               current->comm, current->pid);
        return -ERESTARTSYS;
    }

    printk(KERN_INFO "scull_ring: Process %s (pid %d) acquired mutex lock for writing\n", 
           current->comm, current->pid);

    // БЛОКИРОВКА 2: Писатель ждет места (буфер полный)
    while (buf->data_len == buf->size) {
        printk(KERN_INFO "scull_ring: Process %s (pid %d) BLOCKED - buffer full, waiting for space (data_len=%d, size=%d)\n", 
               current->comm, current->pid, buf->data_len, buf->size);
        
        // Освобождение мьютекса перед блокировкой (чтобы читатели могли освободить место)
        mutex_unlock(&buf->lock);
        
        // Блокировка в очереди ожидания до появления свободного места
        if (wait_event_interruptible(buf->write_queue, (buf->data_len < buf->size))) {
            printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while waiting for buffer space\n", 
                   current->comm, current->pid);
            return -ERESTARTSYS;
        }
        
        // Повторный захват мьютекса после пробуждения
        if (mutex_lock_interruptible(&buf->lock)) {
            printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while re-acquiring mutex after wait\n", 
                   current->comm, current->pid);
            return -ERESTARTSYS;
        }
        
        printk(KERN_INFO "scull_ring: Process %s (pid %d) UNBLOCKED - space available (data_len=%d)\n", 
               current->comm, current->pid, buf->data_len);
    }

    // Расчет доступного места для записи
    available = buf->size - buf->data_len;
    if (count > available) {
        // Усечение записи если запрашивается больше чем доступно
        count = available;
        printk(KERN_INFO "scull_ring: Process %s (pid %d) write truncated to %zu bytes (buffer almost full)\n", 
               current->comm, current->pid, count);
    }

    // Копирование данных из пользовательского пространства в кольцевой буфер
    // Обработка случая, когда запись пересекает границу буфера
    to_end = buf->size - buf->write_pos;
    if (count > to_end) {
        // Две операции копирования: от write_pos до конца и с начала буфера
        if (copy_from_user(buf->data + buf->write_pos, user_buf, to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
        if (copy_from_user(buf->data, user_buf + to_end, count - to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    } else {
        // Одна операция копирования
        if (copy_from_user(buf->data + buf->write_pos, user_buf, count)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    }

    // Обновление позиции записи и количества данных
    buf->write_pos = (buf->write_pos + count) % buf->size;
    buf->data_len += count;
    bytes_written = count;

    // Увеличение счетчика операций записи
    atomic_inc(&buf->write_count);
    
    // Пробуждение ожидающих читателей (появились новые данные)
    printk(KERN_INFO "scull_ring: Process %s (pid %d) wrote %d bytes, waking up readers (new data_len=%d)\n", 
           current->comm, current->pid, bytes_written, buf->data_len);
    
    wake_up_interruptible(&buf->read_queue);
    mutex_unlock(&buf->lock);
    
    printk(KERN_INFO "scull_ring: Process %s (pid %d) released mutex after writing\n", 
           current->comm, current->pid);
    return bytes_written;
}

/**
 * Операция открытия устройства
 */
static int scull_ring_open(struct inode *inode, struct file *filp) {
    struct scull_ring_dev *dev;
    
    // Получение структуры устройства из inode
    dev = container_of(inode->i_cdev, struct scull_ring_dev, cdev);
    filp->private_data = dev;  // Сохранение для использования в других операциях
    
    printk(KERN_INFO "scull_ring: Process %s (pid %d) opened device\n", 
           current->comm, current->pid);
    return 0;
}

/**
 * Операция закрытия устройства
 */
static int scull_ring_release(struct inode *inode, struct file *filp) {
    printk(KERN_INFO "scull_ring: Process %s (pid %d) closed device\n", 
           current->comm, current->pid);
    return 0;
}

/**
 * Файловая операция read - точка входа из пользовательского пространства
 */
static ssize_t scull_ring_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    struct scull_ring_dev *dev = filp->private_data;
    return scull_ring_buffer_read(dev->ring_buf, buf, count);
}

/**
 * Файловая операция write - точка входа из пользовательского пространства
 */
static ssize_t scull_ring_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
    struct scull_ring_dev *dev = filp->private_data;
    return scull_ring_buffer_write(dev->ring_buf, buf, count);
}

/**
 * IOCTL операции для управления и мониторинга устройства
 * @filp: файловая структура
 * @cmd: команда IOCTL
 * @arg: аргумент команды (указатель в пользовательском пространстве)
 * 
 * Поддерживаемые команды:
 * - GET_STATUS: получение статуса буфера (размер, заполненность)
 * - GET_COUNTERS: получение счетчиков операций чтения/записи
 * - PEEK_BUFFER: просмотр содержимого буфера без извлечения
 */
static long scull_ring_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct scull_ring_dev *dev = filp->private_data;
    struct scull_ring_buffer *buf = dev->ring_buf;
    int status[4];
    long counters[2];
    char peek_buffer[512];
    int message_count;

    printk(KERN_INFO "scull_ring: Process %s (pid %d) calling ioctl cmd=%u\n", 
           current->comm, current->pid, cmd);

    switch (cmd) {
        case SCULL_RING_IOCTL_GET_STATUS:
            // Безопасное получение статуса буфера под мьютексом
            if (mutex_lock_interruptible(&buf->lock)) {
                printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while waiting for mutex lock (IOCTL STATUS)\n", 
                       current->comm, current->pid);
                return -ERESTARTSYS;
            }
            status[0] = buf->data_len;  // Текущее количество данных
            status[1] = buf->size;      // Общий размер буфера
            status[2] = 0;              // Зарезервировано
            status[3] = 0;              // Зарезервировано
            mutex_unlock(&buf->lock);

            // Копирование результатов в пользовательское пространство
            if (copy_to_user((int __user *)arg, status, sizeof(status))) {
                return -EFAULT;
            }
            break;
            
        case SCULL_RING_IOCTL_GET_COUNTERS:
            // Получение атомарных счетчиков (не требует мьютекса)
            counters[0] = atomic_read(&buf->read_count);
            counters[1] = atomic_read(&buf->write_count);
            
            if (copy_to_user((long __user *)arg, counters, sizeof(counters))) {
                return -EFAULT;
            }
            break;
            
        case SCULL_RING_IOCTL_PEEK_BUFFER:
            // БЛОКИРОВКА 3: IOCTL ждет мьютекс для чтения содержимого буфера
            if (mutex_lock_interruptible(&buf->lock)) {
                printk(KERN_INFO "scull_ring: Process %s (pid %d) interrupted while waiting for mutex lock (IOCTL PEEK)\n", 
                       current->comm, current->pid);
                return -ERESTARTSYS;
            }
            
            printk(KERN_INFO "scull_ring: Process %s (pid %d) acquired mutex for ioctl peek\n", 
                   current->comm, current->pid);
            
            // Извлечение всех сообщений для отладки
            message_count = extract_messages(buf, peek_buffer, sizeof(peek_buffer));
            
            mutex_unlock(&buf->lock);
            
            printk(KERN_INFO "scull_ring: Process %s (pid %d) released mutex after ioctl peek\n", 
                   current->comm, current->pid);
            
            // Копирование результатов просмотра в пользовательское пространство
            if (copy_to_user((char __user *)arg, peek_buffer, sizeof(peek_buffer))) {
                return -EFAULT;
            }
            break;
            
        default:
            // Неизвестная команда IOCTL
            return -ENOTTY;
    }
    return 0;
}

/**
 * Инициализация модуля драйвера
 * 
 * Регистрирует устройства, выделяет ресурсы, инициализирует структуры данных.
 * Создает три устройства: /dev/scull_ring0, /dev/scull_ring1, /dev/scull_ring2
 */
static int __init scull_ring_init(void) {
    dev_t dev = 0;
    int err, i;

    // Регистрация диапазона символьных устройств
    if (scull_ring_major) {
        // Использование указанного основного номера
        dev = MKDEV(scull_ring_major, 0);
        err = register_chrdev_region(dev, SCULL_RING_NR_DEVS, DEVICE_NAME);
    } else {
        // Автоматическое выделение основного номера
        err = alloc_chrdev_region(&dev, 0, SCULL_RING_NR_DEVS, DEVICE_NAME);
        scull_ring_major = MAJOR(dev);
    }
    if (err < 0) {
        printk(KERN_WARNING "scull_ring: can't get major %d\n", scull_ring_major);
        return err;
    }

    // Инициализация каждого из трех устройств
    for (i = 0; i < SCULL_RING_NR_DEVS; i++) {
        struct scull_ring_dev *scull_dev = &scull_ring_devices[i];
        
        // Выделение памяти для структуры буфера
        scull_dev->ring_buf = kmalloc(sizeof(struct scull_ring_buffer), GFP_KERNEL);
        if (!scull_dev->ring_buf) {
            err = -ENOMEM;
            goto fail;
        }
        
        // Инициализация кольцевого буфера
        err = scull_ring_buffer_init(scull_dev->ring_buf, SCULL_RING_BUFFER_SIZE);
        if (err) {
            kfree(scull_dev->ring_buf);
            goto fail;
        }

        // Инициализация и регистрация символьного устройства
        cdev_init(&scull_dev->cdev, &scull_ring_fops);
        scull_dev->cdev.owner = THIS_MODULE;
        err = cdev_add(&scull_dev->cdev, MKDEV(scull_ring_major, i), 1);
        if (err) {
            printk(KERN_NOTICE "Error %d adding scull_ring%d", err, i);
            scull_ring_buffer_cleanup(scull_dev->ring_buf);
            kfree(scull_dev->ring_buf);
            goto fail;
        }
    }

    // Успешная загрузка модуля
    printk(KERN_INFO "scull_ring: driver loaded with major %d\n", scull_ring_major);
    printk(KERN_INFO "scull_ring: buffer size is %d bytes\n", SCULL_RING_BUFFER_SIZE);
    printk(KERN_ALERT "The process is \"%s\" (pid %i) \n", current->comm, current->pid);
    return 0;

fail:
    // Очистка при ошибке инициализации
    while (--i >= 0) {
        cdev_del(&scull_ring_devices[i].cdev);
        scull_ring_buffer_cleanup(scull_ring_devices[i].ring_buf);
        kfree(scull_ring_devices[i].ring_buf);
    }
    unregister_chrdev_region(MKDEV(scull_ring_major, 0), SCULL_RING_NR_DEVS);
    return err;
}

/**
 * Выгрузка модуля драйвера
 * 
 * Освобождает все ресурсы, удаляет устройства, очищает память.
 */
static void __exit scull_ring_exit(void) {
    int i;
    dev_t dev = MKDEV(scull_ring_major, 0);

    // Очистка всех устройств
    for (i = 0; i < SCULL_RING_NR_DEVS; i++) {
        cdev_del(&scull_ring_devices[i].cdev);
        scull_ring_buffer_cleanup(scull_ring_devices[i].ring_buf);
        kfree(scull_ring_devices[i].ring_buf);
    }

    // Освобождение диапазона устройств
    unregister_chrdev_region(dev, SCULL_RING_NR_DEVS);
    printk(KERN_INFO "scull_ring: driver unloaded\n");
}

// Точки входа и выхода модуля
module_init(scull_ring_init);
module_exit(scull_ring_exit);
