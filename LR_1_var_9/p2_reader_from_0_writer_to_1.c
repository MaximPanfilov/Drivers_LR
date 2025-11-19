
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

// Пути к устройствам для этого процесса
#define DEV_SCULL0 "/dev/scull_ring0"  // Устройство для чтения (вход)
#define DEV_SCULL1 "/dev/scull_ring1"  // Устройство для записи (выход)
#define BUFFER_SIZE 512                // Размер буфера для операций ввода-вывода

// Глобальные переменные для управления выполнением
volatile sig_atomic_t keep_running = 1;  // Флаг продолжения работы
int processed_counter = 0;                // Счетчик обработанных чисел

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
 * Основная функция процесса P2
 * 
 * Логика работы:
 * 1. Открывает scull0 для чтения и scull1 для записи
 * 2. В бесконечном цикле:
 *    a. Читает 1 число из scull0
 *    b. Обрабатывает его (генерирует 2 новых числа)
 *    c. Записывает 2 числа в scull1
 *    d. Ждет 1 секунду
 * 
 * Этот процесс является промежуточным звеном в цепочке:
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

    // Открытие устройства scull0 для чтения (вход от P1)
    fd_read = open(DEV_SCULL0, O_RDONLY);
    if (fd_read < 0) {
        perror("P2: Failed to open " DEV_SCULL0);
        exit(EXIT_FAILURE);
    }

    // Открытие устройства scull1 для записи (выход к P3)
    fd_write = open(DEV_SCULL1, O_WRONLY);
    if (fd_write < 0) {
        perror("P2: Failed to open " DEV_SCULL1);
        close(fd_read);  // Закрыть уже открытый дескриптор при ошибке
        exit(EXIT_FAILURE);
    }

    printf("P2: Started (Reading from %s, Writing to %s). Press Ctrl+C to stop.\n", 
           DEV_SCULL0, DEV_SCULL1);

    // Основной рабочий цикл
    while (keep_running) {
        // Фаза чтения: чтение 1 числа из scull0
        start_time = get_current_time_us();
        
        // Чтение из устройства (максимум BUFFER_SIZE-1 байт)
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        
        end_time = get_current_time_us();
        
        // Обработка результата чтения
        if (n < 0) {
            perror("P2: Read from scull0 failed");
        } else if (n > 0) {
            // Успешное чтение - преобразуем данные в число
            read_buf[n] = '\0';  // Гарантируем нуль-терминацию
            int received_number = atoi(read_buf);
            print_timing_info("P2-READ", "read from scull0", received_number, end_time - start_time);

            // Фаза обработки и записи: генерируем 2 новых числа в scull1
            // Каждое прочитанное число преобразуется в 2 новых
            for (int i = 0; i < 2 && keep_running; i++) {
                start_time = get_current_time_us();
                
                // Генерация нового числа (просто инкремент счетчика)
                snprintf(write_buf, BUFFER_SIZE, "%d", processed_counter);
                
                // Запись в устройство (включая нуль-терминатор)
                n = write(fd_write, write_buf, strlen(write_buf) + 1);
                
                end_time = get_current_time_us();
                
                // Обработка результата записи
                if (n < 0) {
                    perror("P2: Write to scull1 failed");
                } else {
                    // Успешная запись - логируем информацию
                    print_timing_info("P2-WRITE", "wrote to scull1", processed_counter, end_time - start_time);
                }
                processed_counter++;  // Следующее число для записи
            }
        }

        // Пауза между итерациями
        usleep(1000000); // 1 секунда между итерациями
    }

    // Грациозное завершение
    printf("P2: Shutting down...\n");
    close(fd_read);
    close(fd_write);
    return 0;
}
