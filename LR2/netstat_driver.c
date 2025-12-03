/**
 * netstat_driver.c - Драйвер для сбора статистики loopback трафика
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

/* Статистика по протоколам */
static unsigned long tcp_packets = 0, tcp_bytes = 0;
static unsigned long udp_packets = 0, udp_bytes = 0;
static unsigned long icmp_packets = 0, icmp_bytes = 0;
static unsigned long other_packets = 0, other_bytes = 0;

static DEFINE_SPINLOCK(stat_lock);
#define PROC_FILENAME "loopback_stats"

/**
 * Проверка, является ли IP-адрес loopback (127.x.x.x)
 */
static int is_loopback_ip(__be32 ip_addr) {
    unsigned char *bytes = (unsigned char *)&ip_addr;
    return (bytes[0] == 127);
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
    
    packet_len = ntohs(ip_header->tot_len);
    
    /* Обновляем статистику */
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
    seq_puts(m, "=== СТАТИСТИКА LOOPBACK ТРАФИКА ===\n\n");
    
    spin_lock(&stat_lock);
    
    seq_printf(m, "TCP:   %10lu пакетов, %10lu байт\n", tcp_packets, tcp_bytes);
    seq_printf(m, "UDP:   %10lu пакетов, %10lu байт\n", udp_packets, udp_bytes);
    seq_printf(m, "ICMP:  %10lu пакетов, %10lu байт\n", icmp_packets, icmp_bytes);
    seq_printf(m, "Other: %10lu пакетов, %10lu байт\n", other_packets, other_bytes);
    
    spin_unlock(&stat_lock);
    
    seq_puts(m, "\n=== Только loopback трафик (127.x.x.x) ===\n");
    
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
    printk(KERN_INFO "netstat_driver: Статистика в /proc/%s\n", PROC_FILENAME);
    
    return 0;
}

/**
 * Выгрузка
 */
static void __exit netstat_driver_exit(void) {
    remove_proc_entry(PROC_FILENAME, NULL);
    nf_unregister_net_hook(&init_net, &loopback_ops);
    
    printk(KERN_INFO "netstat_driver: Драйвер выгружен\n");
}

module_init(netstat_driver_init);
module_exit(netstat_driver_exit);