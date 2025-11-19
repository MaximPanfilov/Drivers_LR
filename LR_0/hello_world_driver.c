#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

// Макросы для информации о модуле
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxim Panfilov");
MODULE_DESCRIPTION("Simple Hello World Linux Driver");
MODULE_VERSION("0.1");

// Функция, вызываемая при загрузке модуля
static int __init hello_init(void) {
    printk(KERN_INFO "Hello World Driver: Module loaded successfully!\n");
    printk(KERN_INFO "Hello World Driver: Initializing...\n");
    return 0;
}

// Функция, вызываемая при выгрузке модуля
static void __exit hello_exit(void) {
    printk(KERN_INFO "Hello World Driver: Module unloaded successfully!\n");
    printk(KERN_INFO "Hello World Driver: Goodbye!\n");
}

// Регистрация функций инициализации и очистки
module_init(hello_init);
module_exit(hello_exit);