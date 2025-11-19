
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

// Пути к устройствам для этого процесса
#define DEV_SCULL0 "/dev/scull_ring0"  // Устройство для записи
#define DEV_SCULL2 "/dev/scull_ring2"  // Устройство для чтения
#define BUFFER_SIZE 512                // Размер буфера для операций ввода-вывода

// Глобальные переменные для управления выполнением
volatile sig_atomic_t keep_running = 1;  // Флаг продолжения работы
int number_counter = 0;                   // Счетчик генерируемых чисел

/**
 * Обработчик сигнала для graceful shutdown
 */
void signal_handler(int sig) {
    keep_running = 0;
}

/**
 * Получение текущего времени в микросекундах
 * Используется для измерения производительности операций
 * Возвращает: время в микросекундах с эпохи
 */
long get_current_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Печать информации о времени выполнения операции
 * @process: идентификатор процесса (P1-WRITE, P1-READ)
 * @operation: тип операции ("wrote to scull0", "read from scull2")
 * @number: число, которое было обработано
 * @elapsed_us: время операции в микросекундах
 */
void print_timing_info(const char* process, const char* operation, int number, long elapsed_us) {
    printf("[%ld] %s: %s number %d (took %ld us)\n", 
           get_current_time_us(), process, operation, number, elapsed_us);
}

/**
 * Основная функция процесса P1
 * 
 * Логика работы:
 * 1. Открывает scull0 для записи и scull2 для чтения
 * 2. В бесконечном цикле:
 *    a. Записывает 2 числа в scull0
 *    b. Читает 1 число из scull2
 *    c. Ждет 2 секунды
 * 3. Завершается по Ctrl+C
 * 
 * Этот процесс начинает цепочку обработки чисел:
 * P1 -> scull0 -> P2 -> scull1 -> P3 -> scull2 -> P1
 */
int main() {
    int fd_write, fd_read;           // Дескрипторы для записи и чтения
    char write_buf[BUFFER_SIZE];     // Буфер для записи чисел
    char read_buf[BUFFER_SIZE];      // Буфер для чтения чисел
    ssize_t n;                       // Результат операций ввода-вывода
    long start_time, end_time;       // Переменные для измерения времени

    // Установка обработчика сигнала
    signal(SIGINT, signal_handler);

    // Открытие устройства scull0 для записи (начало цепочки)
    fd_write = open(DEV_SCULL0, O_WRONLY);
    if (fd_write < 0) {
        perror("P1: Failed to open " DEV_SCULL0);
        exit(EXIT_FAILURE);
    }

    // Открытие устройства scull2 для чтения (конец цепочки)
    fd_read = open(DEV_SCULL2, O_RDONLY);
    if (fd_read < 0) {
        perror("P1: Failed to open " DEV_SCULL2);
        close(fd_write);  // Важно: закрыть уже открытый дескриптор при ошибке
        exit(EXIT_FAILURE);
    }

    printf("P1: Started (Writing numbers to %s, Reading from %s). Press Ctrl+C to stop.\n", 
           DEV_SCULL0, DEV_SCULL2);

    // Основной рабочий цикл
    while (keep_running) {
        // Фаза записи: запись 2 чисел в scull0
        for (int i = 0; i < 2 && keep_running; i++) {
            // Измерение времени операции записи
            start_time = get_current_time_us();
            
            // Форматирование числа в строку и запись в буфер
            snprintf(write_buf, BUFFER_SIZE, "%d", number_counter);
            
            // Запись в устройство (включая нуль-терминатор)
            n = write(fd_write, write_buf, strlen(write_buf) + 1);
            
            end_time = get_current_time_us();
            
            // Обработка результата записи
            if (n < 0) {
                perror("P1: Write failed");
            } else {
                // Успешная запись - логируем информацию
                print_timing_info("P1-WRITE", "wrote to scull0", number_counter, end_time - start_time);
            }
            number_counter++;  // Увеличиваем счетчик для следующего числа
        }

        // Фаза чтения: чтение 1 числа из scull2
        start_time = get_current_time_us();
        
        // Чтение из устройства (максимум BUFFER_SIZE-1 байт чтобы осталось место для '\0')
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        
        end_time = get_current_time_us();
        
        // Обработка результата чтения
        if (n < 0) {
            perror("P1: Read from scull2 failed");
        } else if (n > 0) {
            // Успешное чтение - преобразуем данные в число и логируем
            read_buf[n] = '\0';  // Гарантируем нуль-терминацию строки
            int received_number = atoi(read_buf);
            print_timing_info("P1-READ", "read from scull2", received_number, end_time - start_time);
        }

        // Пауза между итерациями для регулировки скорости
        usleep(2000000); // 2 секунды между итерациями
    }

    // Грациозное завершение
    printf("P1: Shutting down...\n");
    close(fd_write);
    close(fd_read);
    return 0;
}
