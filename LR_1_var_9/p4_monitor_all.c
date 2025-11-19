
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

// IOCTL команды для взаимодействия с драйвером scull_ring
#define SCULL_RING_IOCTL_GET_STATUS _IOR('s', 1, int[4])      // Получить статус буфера
#define SCULL_RING_IOCTL_GET_COUNTERS _IOR('s', 2, long[2])   // Получить счетчики операций
#define SCULL_RING_IOCTL_PEEK_BUFFER _IOWR('s', 10, char[512]) // Просмотреть содержимое буфера

// Пути к устройствам драйвера
#define DEV_SCULL0 "/dev/scull_ring0"
#define DEV_SCULL1 "/dev/scull_ring1"
#define DEV_SCULL2 "/dev/scull_ring2"

// Глобальная переменная для graceful shutdown по сигналу
volatile sig_atomic_t keep_running = 1;

/**
 * Обработчик сигнала для graceful shutdown
 * Устанавливает флаг keep_running в 0 при получении SIGINT (Ctrl+C)
 */
void signal_handler(int sig) {
    keep_running = 0;
}

/**
 * Печать временной метки в формате [HH:MM:SS]
 * Используется для логгирования времени событий
 */
void print_timestamp() {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    printf("[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
}

/**
 * Получение и отображение детальной информации о состоянии устройства
 * @fd: файловый дескриптор устройства
 * @dev_name: имя устройства для отображения
 * @last_reads: массив счетчиков чтения с предыдущей итерации
 * @last_writes: массив счетчиков записи с предыдущей итерации
 * @dev_index: индекс устройства (0, 1, 2) для отслеживания истории
 * 
 * Функция собирает через IOCTL:
 * - Статус буфера (заполненность, размер)
 * - Счетчики операций (чтение/запись)
 * - Содержимое буфера (последние сообщения)
 * - Разницу счетчиков с предыдущим измерением
 */
void print_detailed_status(int fd, const char* dev_name, long *last_reads, long *last_writes, int dev_index) {
    int status[4];              // Статус буфера: [data_len, size, 0, 0]
    long counters[2];           // Счетчики: [read_count, write_count]
    char buffer_content[512];   // Буфер для содержимого
    static int first_run[3] = {1, 1, 1};  // Флаг первого запуска для каждого устройства
    int ret;
    
    // Получение статуса буфера через IOCTL
    ret = ioctl(fd, SCULL_RING_IOCTL_GET_STATUS, status);
    if (ret != 0) {
        print_timestamp();
        printf("%s: Error reading status\n", dev_name);
        return;
    }
    
    // Получение счетчиков операций через IOCTL
    ret = ioctl(fd, SCULL_RING_IOCTL_GET_COUNTERS, counters);
    if (ret != 0) {
        print_timestamp();
        printf("%s: Error reading counters\n", dev_name);
        return;
    }
    
    // Расчет разницы счетчиков с предыдущей итерацией
    long read_diff = 0, write_diff = 0;
    
    if (!first_run[dev_index]) {
        // Не первая итерация - вычисляем разницу
        read_diff = counters[0] - last_reads[dev_index];
        write_diff = counters[1] - last_writes[dev_index];
    } else {
        // Первая итерация - инициализируем флаг
        first_run[dev_index] = 0;
    }
    
    // Попытка получить содержимое буфера для отладки
    memset(buffer_content, 0, sizeof(buffer_content));
    ret = ioctl(fd, SCULL_RING_IOCTL_PEEK_BUFFER, buffer_content);
    
    if (ret != 0) {
        // Ошибка при получении содержимого буфера - выводим только базовую информацию
        print_timestamp();
        printf("%s: Data=%d/%d (%.1f%%)", 
               dev_name, status[0], status[1], 
               (float)status[0] / status[1] * 100.0);
        
        // Показываем разницу операций только если были изменения
        if (read_diff > 0 || write_diff > 0) {
            printf(" [R:+%ld W:+%ld]", read_diff, write_diff);
        }
        printf(" [Total:R%ld W%ld]\n", counters[0], counters[1]);
    } else {
        // Успешно получили содержимое буфера - выводим полную информацию
        print_timestamp();
        printf("%s: Data=%d/%d (%.1f%%)", 
               dev_name, status[0], status[1], 
               (float)status[0] / status[1] * 100.0);
        
        // Показываем разницу операций только если были изменения
        if (read_diff > 0 || write_diff > 0) {
            printf(" [R:+%ld W:+%ld]", read_diff, write_diff);
        }
        printf(" [Total:R%ld W%ld]\n", counters[0], counters[1]);
        printf("    Numbers: %s\n", buffer_content);  // Отображаем содержимое буфера
    }
    
    // Сохраняем текущие значения счетчиков для следующей итерации
    last_reads[dev_index] = counters[0];
    last_writes[dev_index] = counters[1];
}

/**
 * Основная функция монитора
 * 
 * Программа открывает все три устройства и в бесконечном цикле:
 * 1. Очищает экран
 * 2. Собирает статус всех устройств через IOCTL
 * 3. Отображает информацию в формате:
 *    [HH:MM:SS] scull0: Data=current/size (fill%) [R:+reads W:+writes] [Total:Rtotal Wtotal]
 *    Numbers: [содержимое буфера]
 * 4. Ждет 2 секунды и повторяет
 * 
 * Грациозно завершается по Ctrl+C
 */
int main() {
    int fd0, fd1, fd2;          // Файловые дескрипторы устройств
    int iteration = 0;          // Счетчик итераций мониторинга
    long last_reads[3] = {0};   // История счетчиков чтения для каждого устройства
    long last_writes[3] = {0};  // История счетчиков записи для каждого устройства

    // Регистрация обработчика сигнала для graceful shutdown
    signal(SIGINT, signal_handler);

    // Открытие всех трех устройств драйвера в режиме только чтения
    // Режим O_RDONLY достаточен для IOCTL операций мониторинга
    fd0 = open(DEV_SCULL0, O_RDONLY);
    fd1 = open(DEV_SCULL1, O_RDONLY);
    fd2 = open(DEV_SCULL2, O_RDONLY);

    // Проверка успешности открытия всех устройств
    if (fd0 < 0 || fd1 < 0 || fd2 < 0) {
        perror("P4: Failed to open one or more devices");
        exit(EXIT_FAILURE);
    }

    printf("P4: Number Monitor Started. Press Ctrl+C to stop.\n\n");

    // Основной цикл мониторинга
    while (keep_running) {
        // Очистка экрана для обновляемого дисплея
        system("clear");
        
        // Заголовок с номером итерации
        printf("=== Number Flow Monitor (Iteration: %d) ===\n\n", iteration++);
        
        // Получение и отображение статуса для каждого устройства
        print_detailed_status(fd0, "scull0", last_reads, last_writes, 0);
        printf("\n");
        print_detailed_status(fd1, "scull1", last_reads, last_writes, 1);
        printf("\n");
        print_detailed_status(fd2, "scull2", last_reads, last_writes, 2);
        
        // Легенда для понимания формата вывода
        printf("\nLegend: Data=current/size (fill%%), [R:+reads W:+writes] [Total:Rtotal Wtotal]\n");
        printf("Refreshing every 2 seconds...\n");
        
        // Пауза между обновлениями
        sleep(2);
    }

    // Грациозное завершение
    printf("P4: Shutting down...\n");
    close(fd0);
    close(fd1);
    close(fd2);
    return 0;
}
