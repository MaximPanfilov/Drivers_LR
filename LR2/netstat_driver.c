/**
 * netstat_driver.c - Драйвер для сбора статистики loopback трафика
 * Собирает статистику по протоколам и IP-адресам
 */

#include <linux/module.h>      /* Для создания модулей ядра */
#include <linux/kernel.h>      /* Основные функции ядра (printk и др.) */
#include <linux/netdevice.h>   /* Работа с сетевыми устройствами */
#include <linux/in.h>          /* Определения интернет-протоколов */
#include <linux/proc_fs.h>     /* Файловая система /proc */
#include <linux/seq_file.h>    /* Последовательные файлы для /proc */
#include <linux/slab.h>        /* Функции управления памятью (kmalloc/kfree) */
#include <linux/spinlock.h>    /* Спинлоки для синхронизации */
#include <linux/ip.h>          /* Заголовки IP-пакетов */
#include <linux/tcp.h>         /* Заголовки TCP */
#include <linux/udp.h>         /* Заголовки UDP */
#include <linux/icmp.h>        /* Заголовки ICMP */
#include <linux/netfilter.h>   /* Netfilter - система фильтрации пакетов */
#include <linux/netfilter_ipv4.h> /* Netfilter для IPv4 */
#include <linux/string.h>      /* Строковые функции */
#include <linux/version.h>     /* Информация о версии ядра */

MODULE_LICENSE("GPL");                      
MODULE_AUTHOR("Ваше Имя");                   
MODULE_DESCRIPTION("Драйвер для сбора статистики loopback трафика"); 

/* СТРУКТУРА ДАННЫХ: хранит статистику для одного IP-адреса */
struct ip_stat {
    __be32 ip_addr;           /* IP-адрес в сетевом порядке байтов */
    unsigned long src_count;  /* Количество раз, когда адрес был источником */
    unsigned long dst_count;  /* Количество раз, когда адрес был получателем */
    unsigned long bytes;      /* Общее количество байт, связанных с этим адресом */
    struct ip_stat *next;     /* Указатель на следующий элемент в односвязном списке */
};

/* ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ СТАТИСТИКИ ПО ПРОТОКОЛАМ */
/* Каждая переменная хранит количество пакетов и байт для соответствующего протокола */
static unsigned long tcp_packets = 0, tcp_bytes = 0;
static unsigned long udp_packets = 0, udp_bytes = 0;
static unsigned long icmp_packets = 0, icmp_bytes = 0;
static unsigned long other_packets = 0, other_bytes = 0;

/* ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ СТАТИСТИКИ ПО IP-АДРЕСАМ */
static struct ip_stat *ip_list = NULL;       /* Голова односвязного списка IP-статистики */
static DEFINE_SPINLOCK(stat_lock);           /* Спинлок для защиты доступа к статистике */
#define PROC_FILENAME "loopback_stats"       /* Имя файла в /proc для вывода статистики */
#define MAX_IP_ENTRIES 50     /* Максимальное количество IP-адресов в списке */

/**
 * is_loopback_ip - Проверяет, является ли IP-адрес loopback (127.x.x.x)
 * @ip_addr: IP-адрес для проверки
 * 
 * Возвращает: 1 если адрес loopback, 0 в противном случае
 */
static int is_loopback_ip(__be32 ip_addr) {
    unsigned char *bytes = (unsigned char *)&ip_addr;
    return (bytes[0] == 127); /* Первый октет равен 127 для loopback адресов */
}

/**
 * find_ip_stat - Ищет запись статистики по IP-адресу в списке
 * @ip_addr: IP-адрес для поиска
 * 
 * Возвращает: указатель на найденную запись или NULL если не найдена
 * 
 * Функция линейно проходит по связному списку для поиска записи.
 */
static struct ip_stat *find_ip_stat(__be32 ip_addr) {
    struct ip_stat *entry = ip_list;
    
    while (entry != NULL) {
        if (entry->ip_addr == ip_addr) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/**
 * update_ip_stat - Добавляет или обновляет статистику по IP-адресу
 * @ip_addr: IP-адрес для обновления
 * @packet_len: длина пакета в байтах
 * @is_src: флаг (1 = адрес источник, 0 = адрес получатель)
 * 
 * Функция атомарно обновляет статистику:
 * 1. Если запись существует - обновляет счетчики
 * 2. Если записи нет и не превышен лимит - создает новую запись
 * 3. Использует GFP_ATOMIC для выделения памяти в атомарном контексте
 */
static void update_ip_stat(__be32 ip_addr, unsigned int packet_len, int is_src) {
    struct ip_stat *entry;
    unsigned long flags;
    static int count = 0;  /* Статический счетчик количества записей в списке */
    
    /* Захватываем спинлок с сохранением флагов прерываний */
    spin_lock_irqsave(&stat_lock, flags);
    
    entry = find_ip_stat(ip_addr);
    
    if (entry) {
        /* Запись существует - обновляем счетчики */
        if (is_src) {
            entry->src_count++;
        } else {
            entry->dst_count++;
        }
        entry->bytes += packet_len;
    } else {
        /* Создаем новую запись, если не превышен лимит */
        if (count < MAX_IP_ENTRIES) {
            /* выделяем память */
            entry = kmalloc(sizeof(struct ip_stat), GFP_ATOMIC);
            if (entry) {
                entry->ip_addr = ip_addr;
                entry->src_count = is_src ? 1 : 0;
                entry->dst_count = is_src ? 0 : 1;
                entry->bytes = packet_len;
                /* Добавляем новую запись в начало списка */
                entry->next = ip_list;
                ip_list = entry;
                count++;
            }
        }
    }
    
    /* Освобождаем спинлок и восстанавливаем флаги прерываний */
    spin_unlock_irqrestore(&stat_lock, flags);
}

/**
 * loopback_hook - Функция-обработчик (hook) для перехвата сетевых пакетов
 * @skb: указатель на структуру sk_buff (сетевой пакет)
 * @state: состояние Netfilter hook
 * 
 * Возвращает: NF_ACCEPT (пропускаем пакет дальше по цепочке)
 * 
 * Этот hook вызывается для каждого сетевого пакета, проходящего через систему
 */
static unsigned int loopback_hook(void *priv, 
                                  struct sk_buff *skb,
                                  const struct nf_hook_state *state) {
    struct iphdr *ip_header;
    unsigned int packet_len;
    
    /* Проверяем валидность skb и наличие сетевого заголовка */
    if (!skb || !skb_network_header(skb))
        return NF_ACCEPT;
    
    ip_header = ip_hdr(skb);  /* Получаем указатель на IP-заголовок */
    
    /* Только IPv4 пакеты */
    if (ip_header->version != 4)
        return NF_ACCEPT;
    
    /* только loopback трафик (исходящий или входящий) */
    if (!is_loopback_ip(ip_header->saddr) && !is_loopback_ip(ip_header->daddr))
        return NF_ACCEPT;
    
    packet_len = skb->len;  /* Полная длина пакета (включая заголовки) */
    
    /* ОБНОВЛЕНИЕ СТАТИСТИКИ ПО ПРОТОКОЛАМ */
    spin_lock(&stat_lock);  /* Захватываем спинлок для доступа к глобальным переменным */
    
    switch (ip_header->protocol) {
        case IPPROTO_TCP:
            tcp_packets++;
            tcp_bytes += packet_len;
            break;
        case IPPROTO_UDP:
            udp_packets++;
            udp_bytes += packet_len;
            break;
        case IPPROTO_ICMP:
            icmp_packets++;
            icmp_bytes += packet_len;
            break;
        default:
            other_packets++;
            other_bytes += packet_len;
            break;
    }
    
    spin_unlock(&stat_lock);  /* Освобождаем спинлок */
    
    /* ОБНОВЛЕНИЕ СТАТИСТИКИ ПО IP-АДРЕСАМ */
    if (is_loopback_ip(ip_header->saddr)) {
        update_ip_stat(ip_header->saddr, packet_len, 1);  /* 1 = адрес источник */
    }
    
    if (is_loopback_ip(ip_header->daddr)) {
        update_ip_stat(ip_header->daddr, packet_len, 0);  /* 0 = адрес получатель */
    }
    
    return NF_ACCEPT;  /* Пропускаем пакет дальше по сетевому стеку */
}

/* ОПРЕДЕЛЕНИЕ NETFILTER HOOK */
static struct nf_hook_ops loopback_ops = {
    .hook = loopback_hook,          /* Функция-обработчик */
    .pf = NFPROTO_IPV4,             /* Протокол: IPv4 */
    .hooknum = NF_INET_LOCAL_IN,    /* Хук на входные пакеты (после маршрутизации) */
    .priority = NF_IP_PRI_FIRST,    /* Приоритет хука (высокий - выполняется рано) */
};

/**
 * loopback_stats_show - Функция отображения статистики в /proc файле
 * @m: seq_file структура для последовательного вывода
 * 
 * Возвращает: 0 при успехе
 * 
 * Эта функция вызывается при чтении /proc/loopback_stats
 */
static int loopback_stats_show(struct seq_file *m, void *v) {
    struct ip_stat *entry;
    char ip_str[16];  /* Буфер для строкового представления IP-адреса */
    unsigned long total_packets, total_bytes;
    int ip_count = 0;
    
    /* Заголовок вывода */
    seq_puts(m, "=== СТАТИСТИКА LOOPBACK ТРАФИКА ===\n\n");
    
    /* Секция 1: Статистика по протоколам */
    seq_puts(m, "1. Статистика по протоколам:\n");
    seq_puts(m, "----------------------------\n");
    
    /* Захватываем спинлок для атомарного чтения статистики */
    spin_lock(&stat_lock);
    
    /* Вывод статистики по каждому протоколу */
    seq_printf(m, "TCP:    %10lu пакетов, %10lu байт\n", tcp_packets, tcp_bytes);
    seq_printf(m, "UDP:    %10lu пакетов, %10lu байт\n", udp_packets, udp_bytes);
    seq_printf(m, "ICMP:   %10lu пакетов, %10lu байт\n", icmp_packets, icmp_bytes);
    seq_printf(m, "Other:  %10lu пакетов, %10lu байт\n", other_packets, other_bytes);
    
    /* Расчет общей статистики */
    total_packets = tcp_packets + udp_packets + icmp_packets + other_packets;
    total_bytes = tcp_bytes + udp_bytes + icmp_bytes + other_bytes;
    
    seq_puts(m, "------------------------------------\n");
    seq_printf(m, "Всего:  %10lu пакетов, %10lu байт\n\n", total_packets, total_bytes);
    
    /* Секция 2: Статистика по IP-адресам */
    seq_puts(m, "2. Статистика по IP-адресам:\n");
    seq_puts(m, "---------------------------\n");
    
    if (ip_list == NULL) {
        seq_puts(m, "   (пока нет данных)\n");
    } else {
        /* Подсчет количества уникальных IP-адресов */
        entry = ip_list;
        while (entry != NULL) {
            ip_count++;
            entry = entry->next;
        }
        
        seq_printf(m, "Всего уникальных IP-адресов: %d\n\n", ip_count);
        
        /* Детальный вывод по каждому IP-адресу */
        entry = ip_list;
        while (entry != NULL) {
            /* Преобразуем IP-адрес в строку (формат xxx.xxx.xxx.xxx) */
            snprintf(ip_str, sizeof(ip_str), "%pI4", &entry->ip_addr);
            seq_printf(m, "IP: %15s\n", ip_str);
            seq_printf(m, "   Источником:     %6lu раз\n", entry->src_count);
            seq_printf(m, "   Назначением:    %6lu раз\n", entry->dst_count);
            seq_printf(m, "   Всего пакетов:  %6lu\n", entry->src_count + entry->dst_count);
            seq_printf(m, "   Всего байт:     %10lu\n\n", entry->bytes);
            entry = entry->next;
        }
    }
    
    seq_puts(m, "=== Только loopback трафик (127.x.x.x) ===\n");
    
    spin_unlock(&stat_lock);  /* Освобождаем спинлок */
    
    return 0;
}

/* Функция открытия /proc файла */
static int loopback_stats_open(struct inode *inode, struct file *file) {
    /* single_open создает seq_file с нашей функцией отображения */
    return single_open(file, loopback_stats_show, NULL);
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops loopback_stats_fops = {
    .proc_open = loopback_stats_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations loopback_stats_fops = {
    .owner = THIS_MODULE,
    .open = loopback_stats_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};
#endif

/**
 * netstat_driver_init - Функция инициализации модуля
 * 
 * Вызывается при загрузке модуля (insmod/modprobe)
 * Возвращает: 0 при успехе, код ошибки при неудаче
 */
static int __init netstat_driver_init(void) {
    int ret;
    
    printk(KERN_INFO "netstat_driver: Загрузка драйвера loopback статистики\n");
    
    /* 1. РЕГИСТРАЦИЯ NETFILTER HOOK */
    /* Регистрируем наш hook в сети init_net (основная сетьвая область имен) */
    ret = nf_register_net_hook(&init_net, &loopback_ops);
    if (ret) {
        printk(KERN_ERR "netstat_driver: Ошибка регистрации хука: %d\n", ret);
        return ret;
    }
    
    /* 2. СОЗДАНИЕ /PROC ФАЙЛА */
    /* Создаем виртуальный файл /proc/loopback_stats для чтения статистики */
    if (!proc_create(PROC_FILENAME, 0, NULL, &loopback_stats_fops)) {
        /* При ошибке отменяем регистрацию хука */
        nf_unregister_net_hook(&init_net, &loopback_ops);
        printk(KERN_ERR "netstat_driver: Ошибка создания /proc/%s\n", PROC_FILENAME);
        return -ENOMEM;
    }
    
    /* УСПЕШНАЯ ЗАГРУЗКА */
    printk(KERN_INFO "netstat_driver: Драйвер загружен успешно!\n");
    printk(KERN_INFO "netstat_driver: Собирает статистику по IP-адресам\n");
    printk(KERN_INFO "netstat_driver: Статистика в /proc/%s\n", PROC_FILENAME);
    
    return 0;
}

/**
 * netstat_driver_exit - Функция деинициализации модуля
 * 
 * Вызывается при выгрузке модуля (rmmod)
 */
static void __exit netstat_driver_exit(void) {
    struct ip_stat *entry, *next;
    
    printk(KERN_INFO "netstat_driver: Выгрузка драйвера...\n");
    
    /* 1. УДАЛЕНИЕ /PROC ФАЙЛА */
    remove_proc_entry(PROC_FILENAME, NULL);
    
    /* 2. ОТМЕНА РЕГИСТРАЦИИ HOOK */
    nf_unregister_net_hook(&init_net, &loopback_ops);
    
    /* 3. ОЧИСТКА ПАМЯТИ И СБРОС СТАТИСТИКИ */
    spin_lock(&stat_lock);
    
    /* Освобождаем память для всех записей IP-статистики */
    entry = ip_list;
    while (entry != NULL) {
        next = entry->next;
        kfree(entry);
        entry = next;
    }
    ip_list = NULL;
    
    /* Сбрасываем счетчики протоколов */
    tcp_packets = tcp_bytes = 0;
    udp_packets = udp_bytes = 0;
    icmp_packets = icmp_bytes = 0;
    other_packets = other_bytes = 0;
    
    spin_unlock(&stat_lock);
    
    printk(KERN_INFO "netstat_driver: Драйвер выгружен\n");
}

/* МАКРОСЫ ДЛЯ РЕГИСТРАЦИИ ФУНКЦИЙ МОДУЛЯ */
module_init(netstat_driver_init);  /* Указывает функцию инициализации модуля */
module_exit(netstat_driver_exit);  /* Указывает функцию выгрузки модуля */