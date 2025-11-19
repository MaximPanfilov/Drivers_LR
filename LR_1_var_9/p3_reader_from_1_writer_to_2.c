
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

// Пути к устройствам для этого процесса
#define DEV_SCULL1 "/dev/scull_ring1"  // Устройство для чтения (вход)
#define DEV_SCULL2 "/dev/scull_ring2"  // Устройство для записи (выход)
#define BUFFER_SIZE 512                // Размер буфера для операций ввода-вывода

// Глобальные переменные для управления выполнением
volatile sig_atomic_t keep_running = 1;  // Флаг продолжения работы
int final_counter = 0;                    // Счетчик финальных чисел

/**
 * Обработчик сигнала для graceful shutdown
 */
void signal_handler(int sig) {
    keep_running = 0;
}

/**
 * Получение текущего времени в микросекундах
 */
long get_current_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Печать информации о времени выполнения операции
 */
void print_timing_info(const char* process, const char* operation, int number, long elapsed_us) {
    printf("[%ld] %s: %s number %d (took %ld us)\n", 
           get_current_time_us(), process, operation, number, elapsed_us);
}

/**
 * Основная функция процесса P3
 * 
 * Логика работы:
 * 1. Открывает scull1 для чтения и scull2 для записи
 * 2. В бесконечном цикле:
 *    a. Читает 1 число из scull1
 *    b. Обрабатывает его (генерирует 2 новых числа)
 *    c. Записывает 2 числа в scull2
 *    d. Ждет 1 секунду
 * 
 * Этот процесс завершает цепочку обработки:
 * P1 -> scull0 -> P2 -> scull1 -> P3 -> scull2 -> P1
 */
int main() {
    int fd_read, fd_write;           // Дескрипторы для чтения и записи
    char read_buf[BUFFER_SIZE];      // Буфер для чтения чисел
    char write_buf[BUFFER_SIZE];     // Буфер для записи чисел
    ssize_t n;                       // Результат операций ввода-вывода
    long start_time, end_time;       // Переменные для измерения времени

    // Установка обработчика сигнала
    signal(SIGINT, signal_handler);

    // Открытие устройства scull1 для чтения (вход от P2)
    fd_read = open(DEV_SCULL1, O_RDONLY);
    if (fd_read < 0) {
        perror("P3: Failed to open " DEV_SCULL1);
        exit(EXIT_FAILURE);
    }

    // Открытие устройства scull2 для записи (выход к P1)
    fd_write = open(DEV_SCULL2, O_WRONLY);
    if (fd_write < 0) {
        perror("P3: Failed to open " DEV_SCULL2);
        close(fd_read);  // Закрыть уже открытый дескриптор при ошибке
        exit(EXIT_FAILURE);
    }

    printf("P3: Started (Reading from %s, Writing to %s). Press Ctrl+C to stop.\n", 
           DEV_SCULL1, DEV_SCULL2);

    // Основной рабочий цикл
    while (keep_running) {
        // Фаза чтения: чтение 1 числа из scull1
        start_time = get_current_time_us();
        
        // Чтение из устройства (максимум BUFFER_SIZE-1 байт)
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        
        end_time = get_current_time_us();
        
        // Обработка результата чтения
        if (n < 0) {
            perror("P3: Read from scull1 failed");
        } else if (n > 0) {
            // Успешное чтение - преобразуем данные в число
            read_buf[n] = '\0';  // Гарантируем нуль-терминацию
            int received_number = atoi(read_buf);
            print_timing_info("P3-READ", "read from scull1", received_number, end_time - start_time);

            // Фаза обработки и записи: генерируем 2 новых числа в scull2
            // Каждое прочитанное число преобразуется в 2 новых (финальных)
            for (int i = 0; i < 2 && keep_running; i++) {
                start_time = get_current_time_us();
                
                // Генерация финального числа
                snprintf(write_buf, BUFFER_SIZE, "%d", final_counter);
                
                // Запись в устройство (включая нуль-терминатор)
                n = write(fd_write, write_buf, strlen(write_buf) + 1);
                
                end_time = get_current_time_us();
                
                // Обработка результата записи
                if (n < 0) {
                    perror("P3: Write to scull2 failed");
                } else {
                    // Успешная запись - логируем информацию
                    print_timing_info("P3-WRITE", "wrote to scull2", final_counter, end_time - start_time);
                }
                final_counter++;  // Следующее финальное число
            }
        }

        // Пауза между итерациями
        usleep(1000000); // 1 секунда между итерациями
    }

    // Грациозное завершение
    printf("P3: Shutting down...\n");
    close(fd_read);
    close(fd_write);
    return 0;
}
