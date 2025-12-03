/**
 * netstat_driver.c - Драйвер для сбора статистики loopback трафика
 * Собирает статистику по протоколам и IP-адресам
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
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ваше Имя");
MODULE_DESCRIPTION("Драйвер для сбора статистики loopback трафика");

/* Структура для хранения статистики по IP-адресу */
struct ip_stat {
    __be32 ip_addr;           /* IP-адрес */
    unsigned long src_count;  /* Сколько раз был источником */
    unsigned long dst_count;  /* Сколько раз был назначением */
    unsigned long bytes;      /* Всего байт с этого адреса */
    struct ip_stat *next;     /* Следующий элемент списка */
};

/* Статистика по протоколам */
static unsigned long tcp_packets = 0, tcp_bytes = 0;
static unsigned long udp_packets = 0, udp_bytes = 0;
static unsigned long icmp_packets = 0, icmp_bytes = 0;
static unsigned long other_packets = 0, other_bytes = 0;

/* Статистика по IP-адресам */
static struct ip_stat *ip_list = NULL;
static DEFINE_SPINLOCK(stat_lock);
#define PROC_FILENAME "loopback_stats"
#define MAX_IP_ENTRIES 50     /* Максимальное количество IP в списке */

/**
 * Проверка, является ли IP-адрес loopback (127.x.x.x)
 */
static int is_loopback_ip(__be32 ip_addr) {
    unsigned char *bytes = (unsigned char *)&ip_addr;
    return (bytes[0] == 127);
}

/**
 * Поиск статистики по IP-адресу
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
 * Добавление или обновление статистики по IP-адресу
 */
static void update_ip_stat(__be32 ip_addr, unsigned int packet_len, int is_src) {
    struct ip_stat *entry;
    unsigned long flags;
    static int count = 0;
    
    spin_lock_irqsave(&stat_lock, flags);
    
    entry = find_ip_stat(ip_addr);
    
    if (entry) {
        /* Обновляем существующую запись */
        if (is_src) {
            entry->src_count++;
        } else {
            entry->dst_count++;
        }
        entry->bytes += packet_len;
    } else {
        /* Создаем новую запись, если не превышен лимит */
        if (count < MAX_IP_ENTRIES) {
            entry = kmalloc(sizeof(struct ip_stat), GFP_ATOMIC);
            if (entry) {
                entry->ip_addr = ip_addr;
                entry->src_count = is_src ? 1 : 0;
                entry->dst_count = is_src ? 0 : 1;
                entry->bytes = packet_len;
                entry->next = ip_list;
                ip_list = entry;
                count++;
            }
        }
    }
    
    spin_unlock_irqrestore(&stat_lock, flags);
}

/**
 * Хук для перехвата трафика
 */
static unsigned int loopback_hook(void *priv, 
                                  struct sk_buff *skb,
                                  const struct nf_hook_state *state) {
    struct iphdr *ip_header;
    unsigned int packet_len;
    
    if (!skb || !skb_network_header(skb))
        return NF_ACCEPT;
    
    ip_header = ip_hdr(skb);
    
    /* Только IPv4 */
    if (ip_header->version != 4)
        return NF_ACCEPT;
    
    /* Проверяем loopback адреса (127.x.x.x) */
    if (!is_loopback_ip(ip_header->saddr) && !is_loopback_ip(ip_header->daddr))
        return NF_ACCEPT;
    
    packet_len = skb->len;  /* Используем полную длину пакета */
    
    /* Обновляем статистику по протоколам */
    spin_lock(&stat_lock);
    
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
    
    spin_unlock(&stat_lock);
    
    /* Обновляем статистику по IP-адресам (источник и назначение) */
    if (is_loopback_ip(ip_header->saddr)) {
        update_ip_stat(ip_header->saddr, packet_len, 1);  /* 1 = источник */
    }
    
    if (is_loopback_ip(ip_header->daddr)) {
        update_ip_stat(ip_header->daddr, packet_len, 0);  /* 0 = назначение */
    }
    
    return NF_ACCEPT;
}

/* Netfilter хуки */
static struct nf_hook_ops loopback_ops = {
    .hook = loopback_hook,
    .pf = NFPROTO_IPV4,
    .hooknum = NF_INET_LOCAL_IN,
    .priority = NF_IP_PRI_FIRST,
};

/**
 * Вывод статистики
 */
static int loopback_stats_show(struct seq_file *m, void *v) {
    struct ip_stat *entry;
    char ip_str[16];
    unsigned long total_packets, total_bytes;
    int ip_count = 0;
    
    seq_puts(m, "=== СТАТИСТИКА LOOPBACK ТРАФИКА ===\n\n");
    
    /* Статистика по протоколам */
    seq_puts(m, "1. Статистика по протоколам:\n");
    seq_puts(m, "----------------------------\n");
    
    spin_lock(&stat_lock);
    
    seq_printf(m, "TCP:    %10lu пакетов, %10lu байт\n", tcp_packets, tcp_bytes);
    seq_printf(m, "UDP:    %10lu пакетов, %10lu байт\n", udp_packets, udp_bytes);
    seq_printf(m, "ICMP:   %10lu пакетов, %10lu байт\n", icmp_packets, icmp_bytes);
    seq_printf(m, "Other:  %10lu пакетов, %10lu байт\n", other_packets, other_bytes);
    
    /* Общая статистика */
    total_packets = tcp_packets + udp_packets + icmp_packets + other_packets;
    total_bytes = tcp_bytes + udp_bytes + icmp_bytes + other_bytes;
    
    seq_puts(m, "------------------------------------\n");
    seq_printf(m, "Всего:  %10lu пакетов, %10lu байт\n\n", total_packets, total_bytes);
    
    /* Статистика по IP-адресам */
    seq_puts(m, "2. Статистика по IP-адресам:\n");
    seq_puts(m, "---------------------------\n");
    
    if (ip_list == NULL) {
        seq_puts(m, "   (пока нет данных)\n");
    } else {
        /* Сначала подсчитаем общее количество */
        entry = ip_list;
        while (entry != NULL) {
            ip_count++;
            entry = entry->next;
        }
        
        seq_printf(m, "Всего уникальных IP-адресов: %d\n\n", ip_count);
        
        /* Теперь выводим детали по каждому IP */
        entry = ip_list;
        while (entry != NULL) {
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
    
    spin_unlock(&stat_lock);
    
    return 0;
}

static int loopback_stats_open(struct inode *inode, struct file *file) {
    return single_open(file, loopback_stats_show, NULL);
}

/* Совместимость с разными версиями ядра */
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
 * Инициализация
 */
static int __init netstat_driver_init(void) {
    int ret;
    
    printk(KERN_INFO "netstat_driver: Загрузка драйвера loopback статистики\n");
    
    /* Регистрируем хук */
    ret = nf_register_net_hook(&init_net, &loopback_ops);
    if (ret) {
        printk(KERN_ERR "netstat_driver: Ошибка регистрации хука: %d\n", ret);
        return ret;
    }
    
    /* Создаем proc файл */
    if (!proc_create(PROC_FILENAME, 0, NULL, &loopback_stats_fops)) {
        nf_unregister_net_hook(&init_net, &loopback_ops);
        printk(KERN_ERR "netstat_driver: Ошибка создания /proc/%s\n", PROC_FILENAME);
        return -ENOMEM;
    }
    
    printk(KERN_INFO "netstat_driver: Драйвер загружен успешно!\n");
    printk(KERN_INFO "netstat_driver: Собирает статистику по IP-адресам\n");
    printk(KERN_INFO "netstat_driver: Статистика в /proc/%s\n", PROC_FILENAME);
    
    return 0;
}

/**
 * Выгрузка
 */
static void __exit netstat_driver_exit(void) {
    struct ip_stat *entry, *next;
    
    printk(KERN_INFO "netstat_driver: Выгрузка драйвера...\n");
    
    remove_proc_entry(PROC_FILENAME, NULL);
    nf_unregister_net_hook(&init_net, &loopback_ops);
    
    spin_lock(&stat_lock);
    
    entry = ip_list;
    while (entry != NULL) {
        next = entry->next;
        kfree(entry);
        entry = next;
    }
    ip_list = NULL;
    
    tcp_packets = tcp_bytes = 0;
    udp_packets = udp_bytes = 0;
    icmp_packets = icmp_bytes = 0;
    other_packets = other_bytes = 0;
    
    spin_unlock(&stat_lock);
    
    printk(KERN_INFO "netstat_driver: Драйвер выгружен\n");
}

module_init(netstat_driver_init);
module_exit(netstat_driver_exit);