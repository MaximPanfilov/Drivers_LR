/**
 * netstat_driver.c - Драйвер для сбора статистики сетевого трафика
 * Для ядра Linux 6.x
 * Компиляция: make
 * Загрузка: sudo insmod netstat_driver.ko
 * Вывод статистики: cat /proc/net_traffic_stats
 * Выгрузка: sudo rmmod netstat_driver
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/utsname.h>  /* Добавлено для utsname */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
#include <linux/ktime.h>
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ваше Имя");
MODULE_DESCRIPTION("Драйвер для сбора статистики сетевого трафика");
MODULE_VERSION("1.0");

/* Структура для хранения статистики по IP-адресу */
struct ip_stat {
    __be32 ip_addr;           // IP-адрес в сетевом порядке байт
    unsigned long packets;    // Количество пакетов
    unsigned long bytes;      // Количество байт
    struct ip_stat *next;     // Следующий элемент в списке
};

/* Структура для хранения статистики по протоколам */
struct proto_stat {
    unsigned long packets;    // Пакеты по протоколу
    unsigned long bytes;      // Байты по протоколу
};

/* Глобальные переменные статистики */
static struct ip_stat *ip_list = NULL;           // Список статистики по IP
static struct proto_stat tcp_stat = {0, 0};      // Статистика TCP
static struct proto_stat udp_stat = {0, 0};      // Статистика UDP
static struct proto_stat icmp_stat = {0, 0};     // Статистика ICMP
static struct proto_stat other_stat = {0, 0};    // Статистика других протоколов

/* Спинлок для защиты от конкурентного доступа */
static DEFINE_SPINLOCK(stat_lock);

/* Имя proc-файла */
#define PROC_FILENAME "net_traffic_stats"

/**
 * Функция поиска статистики по IP-адресу в списке
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
 * Функция добавления/обновления статистики по IP-адресу
 */
static void update_ip_stat(__be32 ip_addr, unsigned int packet_len) {
    struct ip_stat *entry;
    unsigned long flags;
    
    /* Захватываем спинлок */
    spin_lock_irqsave(&stat_lock, flags);
    
    /* Ищем существующую запись */
    entry = find_ip_stat(ip_addr);
    
    if (entry) {
        /* Обновляем существующую запись */
        entry->packets++;
        entry->bytes += packet_len;
    } else {
        /* Создаём новую запись */
        entry = kmalloc(sizeof(struct ip_stat), GFP_ATOMIC);
        if (entry) {
            entry->ip_addr = ip_addr;
            entry->packets = 1;
            entry->bytes = packet_len;
            entry->next = ip_list;
            ip_list = entry;
        }
    }
    
    /* Освобождаем спинлок */
    spin_unlock_irqrestore(&stat_lock, flags);
}

/**
 * Обработчик сетевых пакетов (хук)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static unsigned int netstat_hook(void *priv, struct sk_buff *skb,
                                 const struct nf_hook_state *state) {
#else
static unsigned int netstat_hook(const struct nf_hook_ops *ops,
                                 struct sk_buff *skb,
                                 const struct net_device *in,
                                 const struct net_device *out,
                                 const struct nf_hook_state *state) {
#endif
    struct iphdr *ip_header;
    unsigned int packet_len;
    
    /* Проверяем, что пакет содержит IP-заголовок */
    if (!skb || !skb_network_header(skb)) {
        return NF_ACCEPT;
    }
    
    ip_header = ip_hdr(skb);
    
    /* Проверяем, что это IPv4 пакет */
    if (ip_header->version != 4) {
        return NF_ACCEPT;
    }
    
    /* Получаем длину пакета */
    packet_len = ntohs(ip_header->tot_len);
    
    /* Обновляем статистику по протоколам */
    spin_lock(&stat_lock);
    
    switch (ip_header->protocol) {
        case IPPROTO_TCP:
            tcp_stat.packets++;
            tcp_stat.bytes += packet_len;
            break;
            
        case IPPROTO_UDP:
            udp_stat.packets++;
            udp_stat.bytes += packet_len;
            break;
            
        case IPPROTO_ICMP:
            icmp_stat.packets++;
            icmp_stat.bytes += packet_len;
            break;
            
        default:
            other_stat.packets++;
            other_stat.bytes += packet_len;
            break;
    }
    
    spin_unlock(&stat_lock);
    
    /* Обновляем статистику по IP-адресам (источник и назначение) */
    update_ip_stat(ip_header->saddr, packet_len);
    update_ip_stat(ip_header->daddr, packet_len);
    
    /* Пропускаем пакет дальше по стеку */
    return NF_ACCEPT;
}

/* Структура для netfilter хука */
static struct nf_hook_ops netstat_ops[] = {
    {
        .hook = netstat_hook,
        .pf = NFPROTO_IPV4,
        .hooknum = NF_INET_LOCAL_IN,
        .priority = NF_IP_PRI_FIRST,
    },
    {
        .hook = netstat_hook,
        .pf = NFPROTO_IPV4,
        .hooknum = NF_INET_LOCAL_OUT,
        .priority = NF_IP_PRI_FIRST,
    }
};

/**
 * Функция для вывода статистики в proc-файл
 */
static int netstat_proc_show(struct seq_file *m, void *v) {
    struct ip_stat *entry;
    unsigned long flags;
    char ip_str[16];  /* Для IPv4 адреса */
    
    seq_puts(m, "=== СТАТИСТИКА СЕТЕВОГО ТРАФИКА ===\n\n");
    
    /* Вывод статистики по протоколам */
    seq_puts(m, "1. Статистика по протоколам:\n");
    seq_puts(m, "----------------------------\n");
    
    spin_lock_irqsave(&stat_lock, flags);
    
    seq_printf(m, "TCP:  %10lu пакетов, %10lu байт\n",
               tcp_stat.packets, tcp_stat.bytes);
    seq_printf(m, "UDP:  %10lu пакетов, %10lu байт\n",
               udp_stat.packets, udp_stat.bytes);
    seq_printf(m, "ICMP: %10lu пакетов, %10lu байт\n",
               icmp_stat.packets, icmp_stat.bytes);
    seq_printf(m, "Other:%10lu пакетов, %10lu байт\n\n",
               other_stat.packets, other_stat.bytes);
    
    /* Вывод статистики по IP-адресам */
    seq_puts(m, "2. Статистика по IP-адресам:\n");
    seq_puts(m, "---------------------------\n");
    
    entry = ip_list;
    while (entry != NULL) {
        /* Преобразуем IP в строку */
        snprintf(ip_str, sizeof(ip_str), "%pI4", &entry->ip_addr);
        
        seq_printf(m, "IP: %15s - %10lu пакетов, %10lu байт\n",
                   ip_str, entry->packets, entry->bytes);
        entry = entry->next;
    }
    
    spin_unlock_irqrestore(&stat_lock, flags);
    
    seq_puts(m, "\n=== КОНЕЦ СТАТИСТИКИ ===\n");
    return 0;
}

/**
 * Открытие proc-файла
 */
static int netstat_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, netstat_proc_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops netstat_proc_fops = {
    .proc_open = netstat_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations netstat_proc_fops = {
    .owner = THIS_MODULE,
    .open = netstat_proc_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};
#endif

/**
 * Инициализация модуля
 */
static int __init netstat_driver_init(void) {
    int err;
    
    /* Регистрируем netfilter хуки */
    err = nf_register_net_hooks(&init_net, netstat_ops, ARRAY_SIZE(netstat_ops));
    if (err) {
        pr_err("netstat_driver: Не удалось зарегистрировать хуки (ошибка %d)\n", err);
        return err;
    }
    
    /* Создаём proc-файл */
    if (!proc_create(PROC_FILENAME, 0, NULL, &netstat_proc_fops)) {
        nf_unregister_net_hooks(&init_net, netstat_ops, ARRAY_SIZE(netstat_ops));
        pr_err("netstat_driver: Не удалось создать /proc/%s\n", PROC_FILENAME);
        return -ENOMEM;
    }
    
    /* Исправленная строка - используем init_utsname() для получения структуры */
    pr_info("netstat_driver: Драйвер загружен (ядро %s)\n", init_utsname()->release);
    pr_info("netstat_driver: Статистика доступна в /proc/%s\n", PROC_FILENAME);
    
    return 0;
}

/**
 * Выгрузка модуля
 */
static void __exit netstat_driver_exit(void) {
    struct ip_stat *entry, *next;
    
    /* Удаляем proc-файл */
    remove_proc_entry(PROC_FILENAME, NULL);
    
    /* Удаляем netfilter хуки */
    nf_unregister_net_hooks(&init_net, netstat_ops, ARRAY_SIZE(netstat_ops));
    
    /* Освобождаем память, выделенную под статистику IP */
    spin_lock(&stat_lock);
    entry = ip_list;
    while (entry != NULL) {
        next = entry->next;
        kfree(entry);
        entry = next;
    }
    ip_list = NULL;
    spin_unlock(&stat_lock);
    
    pr_info("netstat_driver: Драйвер выгружен\n");
}

/* Регистрация точек входа/выхода */
module_init(netstat_driver_init);
module_exit(netstat_driver_exit);