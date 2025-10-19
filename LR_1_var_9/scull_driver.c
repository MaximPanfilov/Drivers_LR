#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>

#define DEVICE_NAME "scull"
#define SCULL_MAJOR 0
#define SCULL_MINORS 3
#define BUFFER_SIZE 1024

struct scull_buffer {
    char *data;
    int read_pos;
    int write_pos;
    int count;
    int size;
    struct mutex lock;
    wait_queue_head_t readq;
    wait_queue_head_t writeq;
    struct cdev cdev;
};

static int scull_major = SCULL_MAJOR;
static int scull_minors = SCULL_MINORS;
static struct scull_buffer *scull_devices[SCULL_MINORS];

module_param(scull_major, int, S_IRUGO);
module_param(scull_minors, int, S_IRUGO);

static int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_buffer *dev;
    int minor = iminor(inode);
    
    if (minor >= scull_minors)
        return -ENODEV;
    
    dev = scull_devices[minor];
    filp->private_data = dev;
    
    return 0;
}

static int scull_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_buffer *dev = filp->private_data;
    ssize_t retval = 0;
    int bytes_to_read;
    
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    
    while (dev->count == 0) {
        mutex_unlock(&dev->lock);
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
            
        if (wait_event_interruptible(dev->readq, (dev->count > 0)))
            return -ERESTARTSYS;
            
        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;
    }
    
    bytes_to_read = count;
    if (bytes_to_read > dev->count)
        bytes_to_read = dev->count;
    
    if (bytes_to_read > (dev->size - dev->read_pos))
        bytes_to_read = dev->size - dev->read_pos;
    
    if (copy_to_user(buf, dev->data + dev->read_pos, bytes_to_read)) {
        retval = -EFAULT;
        goto out;
    }
    
    dev->read_pos += bytes_to_read;
    if (dev->read_pos >= dev->size)
        dev->read_pos = 0;
    
    dev->count -= bytes_to_read;
    retval = bytes_to_read;
    
    wake_up_interruptible(&dev->writeq);
    
out:
    mutex_unlock(&dev->lock);
    return retval;
}

static ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_buffer *dev = filp->private_data;
    ssize_t retval = 0;
    int bytes_to_write;
    
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;
    
    while (dev->count == dev->size) {
        mutex_unlock(&dev->lock);
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
            
        if (wait_event_interruptible(dev->writeq, (dev->count < dev->size)))
            return -ERESTARTSYS;
            
        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;
    }
    
    bytes_to_write = count;
    if (bytes_to_write > (dev->size - dev->count))
        bytes_to_write = dev->size - dev->count;
    
    if (bytes_to_write > (dev->size - dev->write_pos))
        bytes_to_write = dev->size - dev->write_pos;
    
    if (copy_from_user(dev->data + dev->write_pos, buf, bytes_to_write)) {
        retval = -EFAULT;
        goto out;
    }
    
    dev->write_pos += bytes_to_write;
    if (dev->write_pos >= dev->size)
        dev->write_pos = 0;
    
    dev->count += bytes_to_write;
    retval = bytes_to_write;
    
    wake_up_interruptible(&dev->readq);
    
out:
    mutex_unlock(&dev->lock);
    return retval;
}

static long scull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct scull_buffer *dev = filp->private_data;
    int retval = 0;
    
    switch (cmd) {
        case 0x1001: // GET_BUFFER_INFO
            {
                struct buffer_info {
                    int count;
                    int size;
                    int read_pos;
                    int write_pos;
                } info;
                
                if (mutex_lock_interruptible(&dev->lock))
                    return -ERESTARTSYS;
                    
                info.count = dev->count;
                info.size = dev->size;
                info.read_pos = dev->read_pos;
                info.write_pos = dev->write_pos;
                
                mutex_unlock(&dev->lock);
                
                if (copy_to_user((void __user *)arg, &info, sizeof(info)))
                    return -EFAULT;
            }
            break;
        default:
            retval = -ENOTTY;
            break;
    }
    
    return retval;
}

static struct file_operations scull_fops = {
    .owner = THIS_MODULE,
    .open = scull_open,
    .release = scull_release,
    .read = scull_read,
    .write = scull_write,
    .unlocked_ioctl = scull_ioctl,
};

static void scull_cleanup_module(void)
{
    int i;
    dev_t devno = MKDEV(scull_major, 0);
    
    if (scull_devices) {
        for (i = 0; i < scull_minors; i++) {
            if (scull_devices[i]) {
                cdev_del(&scull_devices[i]->cdev);
                kfree(scull_devices[i]->data);
                kfree(scull_devices[i]);
            }
        }
        kfree(scull_devices);
    }
    
    unregister_chrdev_region(devno, scull_minors);
}

static int __init scull_init_module(void)
{
    int i, err;
    dev_t dev = 0;
    
    if (scull_major) {
        dev = MKDEV(scull_major, 0);
        err = register_chrdev_region(dev, scull_minors, DEVICE_NAME);
    } else {
        err = alloc_chrdev_region(&dev, 0, scull_minors, DEVICE_NAME);
        scull_major = MAJOR(dev);
    }
    
    if (err < 0) {
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return err;
    }
    
    scull_devices = kmalloc(scull_minors * sizeof(struct scull_buffer *), GFP_KERNEL);
    if (!scull_devices) {
        err = -ENOMEM;
        goto fail;
    }
    
    for (i = 0; i < scull_minors; i++) {
        scull_devices[i] = kmalloc(sizeof(struct scull_buffer), GFP_KERNEL);
        if (!scull_devices[i]) {
            err = -ENOMEM;
            goto fail;
        }
        
        scull_devices[i]->data = kmalloc(BUFFER_SIZE, GFP_KERNEL);
        if (!scull_devices[i]->data) {
            kfree(scull_devices[i]);
            err = -ENOMEM;
            goto fail;
        }
        
        scull_devices[i]->read_pos = 0;
        scull_devices[i]->write_pos = 0;
        scull_devices[i]->count = 0;
        scull_devices[i]->size = BUFFER_SIZE;
        
        mutex_init(&scull_devices[i]->lock);
        init_waitqueue_head(&scull_devices[i]->readq);
        init_waitqueue_head(&scull_devices[i]->writeq);
        
        cdev_init(&scull_devices[i]->cdev, &scull_fops);
        scull_devices[i]->cdev.owner = THIS_MODULE;
        
        err = cdev_add(&scull_devices[i]->cdev, MKDEV(scull_major, i), 1);
        if (err) {
            kfree(scull_devices[i]->data);
            kfree(scull_devices[i]);
            goto fail;
        }
    }
    
    printk(KERN_INFO "Scull driver loaded with major %d and %d minors\n", scull_major, scull_minors);
    return 0;
    
fail:
    scull_cleanup_module();
    return err;
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Scull driver with circular buffer");