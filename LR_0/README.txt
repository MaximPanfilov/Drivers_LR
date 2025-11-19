# Drivers_LR
For drivers course LR's
main branch will contain folders of LR's from variouse variants
Лабораторная работа №0 Hello World драйвер.
Лабораторная работа не обязательна к выполнению, но рекомендуется для 
знакомства с созданием и монтированием драйвера. Необходимо написать 
программу-драйвер, содержащего макросы LICENSE, AUTOR, DESCRIPTION. 
Драйвер должен выдавать сообщение при монтировании в ОС и демонтировании из ОС.

After pulling, use "make" command to create hello_world_driver.ko
Use "sudo insmod hello_world_driver.ko" to install driver
Use "lsmod | grep hello_world_driver" to check, if module was downloaded 
Use "sudo dmesh | tail -5" to see kernel messages
[NOTE: ] This Messages is OKAY
    "out-of-tree module" - модуль скомпилирован вне дерева исходных кодов ядра
    "taints kernel" - ядро помечено как "загрязненное" сторонним модулем
    "signature missing" - у модуля нет цифровой подписи