
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
#define SCULL_RING_BUFFER_SIZE 256
#define SCULL_RING_NR_DEVS 3

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxim_Panfilov"); 
MODULE_DESCRIPTION("Scull Driver for LR1");

struct scull_ring_buffer {
    char *data;
    int size;
    int read_pos;
    int write_pos;
    int data_len;
    struct mutex lock;
    wait_queue_head_t read_queue;
    wait_queue_head_t write_queue;
    atomic_t read_count;
    atomic_t write_count;
};

struct scull_ring_dev {
    struct scull_ring_buffer *ring_buf;
    struct cdev cdev;
};

static int scull_ring_major = 0;
module_param(scull_ring_major, int, S_IRUGO);

static struct scull_ring_dev scull_ring_devices[SCULL_RING_NR_DEVS];

#define SCULL_RING_IOCTL_GET_STATUS _IOR('s', 1, int[4])
#define SCULL_RING_IOCTL_GET_COUNTERS _IOR('s', 2, long[2])
#define SCULL_RING_IOCTL_PEEK_BUFFER _IOWR('s', 10, char[512])  // Increased size for formatted output

static int scull_ring_open(struct inode *inode, struct file *filp);
static int scull_ring_release(struct inode *inode, struct file *filp);
static ssize_t scull_ring_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static ssize_t scull_ring_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static long scull_ring_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static struct file_operations scull_ring_fops = {
    .owner = THIS_MODULE,
    .open = scull_ring_open,
    .release = scull_ring_release,
    .read = scull_ring_read,
    .write = scull_ring_write,
    .unlocked_ioctl = scull_ring_ioctl,
};

static int scull_ring_buffer_init(struct scull_ring_buffer *buf, int size) {
    buf->data = kmalloc(size, GFP_KERNEL);
    if (!buf->data) {
        return -ENOMEM;
    }
    buf->size = size;
    buf->read_pos = 0;
    buf->write_pos = 0;
    buf->data_len = 0;
    mutex_init(&buf->lock);
    init_waitqueue_head(&buf->read_queue);
    init_waitqueue_head(&buf->write_queue);
    
    atomic_set(&buf->read_count, 0);
    atomic_set(&buf->write_count, 0);
    
    return 0;
}

static void scull_ring_buffer_cleanup(struct scull_ring_buffer *buf) {
    kfree(buf->data);
}

// Helper function to find null terminator in circular buffer
static int find_null_terminator(struct scull_ring_buffer *buf, int start_pos, int max_len) {
    int pos = start_pos;
    int bytes_checked = 0;
    
    while (bytes_checked < max_len && bytes_checked < buf->data_len) {
        if (buf->data[pos] == '\0') {
            return bytes_checked + 1; // Include the null terminator
        }
        pos = (pos + 1) % buf->size;
        bytes_checked++;
    }
    return -1; // No null terminator found within max_len
}

// Helper function to extract individual messages for peeking
static int extract_messages(struct scull_ring_buffer *buf, char *output, int output_size) {
    int pos = buf->read_pos;
    int bytes_processed = 0;
    int message_count = 0;
    int output_used = 0;
    
    while (bytes_processed < buf->data_len && output_used < output_size - 50) {
        int message_len = find_null_terminator(buf, pos, buf->data_len - bytes_processed);
        if (message_len < 0) {
            // No complete message found, show remaining data
            int remaining = buf->data_len - bytes_processed;
            if (remaining > 0) {
                output_used += snprintf(output + output_used, output_size - output_used, 
                                      "[partial:%d] ", remaining);
            }
            break;
        }
        
        // Extract the message
        char message[50];
        int msg_bytes_copied = 0;
        for (int i = 0; i < message_len - 1 && i < 49; i++) { // -1 to exclude null terminator
            message[i] = buf->data[(pos + i) % buf->size];
            msg_bytes_copied++;
        }
        message[msg_bytes_copied] = '\0';
        
        // Add to output
        output_used += snprintf(output + output_used, output_size - output_used, 
                              "%s ", message);
        
        pos = (pos + message_len) % buf->size;
        bytes_processed += message_len;
        message_count++;
        
        // Limit to 5 messages to avoid overflow
        if (message_count >= 5) {
            output_used += snprintf(output + output_used, output_size - output_used, 
                                  "...(%d more) ", (buf->data_len - bytes_processed) / 20);
            break;
        }
    }
    
    if (message_count == 0 && buf->data_len > 0) {
        snprintf(output, output_size, "[%d bytes, no null terminators]", buf->data_len);
    } else if (message_count == 0) {
        snprintf(output, output_size, "[empty]");
    }
    
    return message_count;
}

static int scull_ring_buffer_read(struct scull_ring_buffer *buf, char __user *user_buf, size_t count) {
    int bytes_read = 0;
    int available;
    int to_end;
    int message_len;

    if (mutex_lock_interruptible(&buf->lock)) {
        return -ERESTARTSYS;
    }

    while (buf->data_len == 0) {
        mutex_unlock(&buf->lock);
        if (wait_event_interruptible(buf->read_queue, (buf->data_len > 0))) {
            return -ERESTARTSYS;
        }
        if (mutex_lock_interruptible(&buf->lock)) {
            return -ERESTARTSYS;
        }
    }

    // Find the length of the first complete message (up to null terminator)
    message_len = find_null_terminator(buf, buf->read_pos, buf->data_len);
    if (message_len < 0) {
        // No complete message found, read what we can
        message_len = (count < buf->data_len) ? count : buf->data_len;
    } else {
        // We found a complete message, don't read more than the message length
        if (message_len > count) {
            message_len = count;
        }
    }

    available = buf->data_len;
    if (message_len > available) {
        message_len = available;
    }

    to_end = buf->size - buf->read_pos;
    if (message_len > to_end) {
        if (copy_to_user(user_buf, buf->data + buf->read_pos, to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
        if (copy_to_user(user_buf + to_end, buf->data, message_len - to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    } else {
        if (copy_to_user(user_buf, buf->data + buf->read_pos, message_len)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    }

    buf->read_pos = (buf->read_pos + message_len) % buf->size;
    buf->data_len -= message_len;
    bytes_read = message_len;

    atomic_inc(&buf->read_count);
    
    wake_up_interruptible(&buf->write_queue);
    mutex_unlock(&buf->lock);
    return bytes_read;
}

static int scull_ring_buffer_write(struct scull_ring_buffer *buf, const char __user *user_buf, size_t count) {
    int bytes_written = 0;
    int available;
    int to_end;

    if (mutex_lock_interruptible(&buf->lock)) {
        return -ERESTARTSYS;
    }

    while (buf->data_len == buf->size) {
        mutex_unlock(&buf->lock);
        if (wait_event_interruptible(buf->write_queue, (buf->data_len < buf->size))) {
            return -ERESTARTSYS;
        }
        if (mutex_lock_interruptible(&buf->lock)) {
            return -ERESTARTSYS;
        }
    }

    available = buf->size - buf->data_len;
    if (count > available) {
        count = available;
    }

    to_end = buf->size - buf->write_pos;
    if (count > to_end) {
        if (copy_from_user(buf->data + buf->write_pos, user_buf, to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
        if (copy_from_user(buf->data, user_buf + to_end, count - to_end)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    } else {
        if (copy_from_user(buf->data + buf->write_pos, user_buf, count)) {
            mutex_unlock(&buf->lock);
            return -EFAULT;
        }
    }

    buf->write_pos = (buf->write_pos + count) % buf->size;
    buf->data_len += count;
    bytes_written = count;

    atomic_inc(&buf->write_count);
    
    wake_up_interruptible(&buf->read_queue);
    mutex_unlock(&buf->lock);
    return bytes_written;
}

static int scull_ring_open(struct inode *inode, struct file *filp) {
    struct scull_ring_dev *dev;
    
    dev = container_of(inode->i_cdev, struct scull_ring_dev, cdev);
    filp->private_data = dev;
    
    return 0;
}

static int scull_ring_release(struct inode *inode, struct file *filp) {
    return 0;
}

static ssize_t scull_ring_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    struct scull_ring_dev *dev = filp->private_data;
    return scull_ring_buffer_read(dev->ring_buf, buf, count);
}

static ssize_t scull_ring_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
    struct scull_ring_dev *dev = filp->private_data;
    return scull_ring_buffer_write(dev->ring_buf, buf, count);
}

static long scull_ring_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct scull_ring_dev *dev = filp->private_data;
    struct scull_ring_buffer *buf = dev->ring_buf;
    int status[4];
    long counters[2];
    char peek_buffer[512];  // Larger buffer for formatted output
    int message_count;

    switch (cmd) {
        case SCULL_RING_IOCTL_GET_STATUS:
            if (mutex_lock_interruptible(&buf->lock)) {
                return -ERESTARTSYS;
            }
            status[0] = buf->data_len;
            status[1] = buf->size;
            status[2] = 0;
            status[3] = 0;
            mutex_unlock(&buf->lock);

            if (copy_to_user((int __user *)arg, status, sizeof(status))) {
                return -EFAULT;
            }
            break;
            
        case SCULL_RING_IOCTL_GET_COUNTERS:
            counters[0] = atomic_read(&buf->read_count);
            counters[1] = atomic_read(&buf->write_count);
            
            if (copy_to_user((long __user *)arg, counters, sizeof(counters))) {
                return -EFAULT;
            }
            break;
            
        case SCULL_RING_IOCTL_PEEK_BUFFER:
            if (mutex_lock_interruptible(&buf->lock)) {
                return -ERESTARTSYS;
            }
            
            // Extract individual messages for display
            if (buf->data_len > 0) {
                message_count = extract_messages(buf, peek_buffer, sizeof(peek_buffer));
            } else {
                strcpy(peek_buffer, "[empty]");
            }
            
            mutex_unlock(&buf->lock);
            
            if (copy_to_user((char __user *)arg, peek_buffer, sizeof(peek_buffer))) {
                return -EFAULT;
            }
            break;
            
        default:
            return -ENOTTY;
    }
    return 0;
}

static int __init scull_ring_init(void) {
    dev_t dev = 0;
    int err, i;

    if (scull_ring_major) {
        dev = MKDEV(scull_ring_major, 0);
        err = register_chrdev_region(dev, SCULL_RING_NR_DEVS, DEVICE_NAME);
    } else {
        err = alloc_chrdev_region(&dev, 0, SCULL_RING_NR_DEVS, DEVICE_NAME);
        scull_ring_major = MAJOR(dev);
    }
    if (err < 0) {
        printk(KERN_WARNING "scull_ring: can't get major %d\n", scull_ring_major);
        return err;
    }

    for (i = 0; i < SCULL_RING_NR_DEVS; i++) {
        struct scull_ring_dev *scull_dev = &scull_ring_devices[i];
        
        scull_dev->ring_buf = kmalloc(sizeof(struct scull_ring_buffer), GFP_KERNEL);
        if (!scull_dev->ring_buf) {
            err = -ENOMEM;
            goto fail;
        }
        err = scull_ring_buffer_init(scull_dev->ring_buf, SCULL_RING_BUFFER_SIZE);
        if (err) {
            kfree(scull_dev->ring_buf);
            goto fail;
        }

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

    printk(KERN_INFO "scull_ring: driver loaded with major %d\n", scull_ring_major);
    printk(KERN_INFO "scull_ring: buffer size is %d bytes\n", SCULL_RING_BUFFER_SIZE);
    printk(KERN_INFO "scull_ring: PEEK IOCTL available (cmd=10)\n");
    printk(KERN_ALERT "The process is \"%s\" (pid %i) \n", current->comm, current->pid);
    return 0;

fail:
    while (--i >= 0) {
        cdev_del(&scull_ring_devices[i].cdev);
        scull_ring_buffer_cleanup(scull_ring_devices[i].ring_buf);
        kfree(scull_ring_devices[i].ring_buf);
    }
    unregister_chrdev_region(MKDEV(scull_ring_major, 0), SCULL_RING_NR_DEVS);
    return err;
}

static void __exit scull_ring_exit(void) {
    int i;
    dev_t dev = MKDEV(scull_ring_major, 0);

    for (i = 0; i < SCULL_RING_NR_DEVS; i++) {
        cdev_del(&scull_ring_devices[i].cdev);
        scull_ring_buffer_cleanup(scull_ring_devices[i].ring_buf);
        kfree(scull_ring_devices[i].ring_buf);
    }

    unregister_chrdev_region(dev, SCULL_RING_NR_DEVS);
    printk(KERN_INFO "scull_ring: driver unloaded\n");
}

module_init(scull_ring_init);
module_exit(scull_ring_exit);
