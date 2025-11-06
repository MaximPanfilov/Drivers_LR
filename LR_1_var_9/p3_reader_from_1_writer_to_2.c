#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define DEV_SCULL1 "/dev/scull_ring1"
#define DEV_SCULL2 "/dev/scull_ring2"
#define BUFFER_SIZE 512  // Увеличили размер буфера

volatile sig_atomic_t keep_running = 1;

void signal_handler(int sig) {
    keep_running = 0;
}

int main() {
    int fd_read, fd_write;
    char read_buf[BUFFER_SIZE];
    char write_buf[BUFFER_SIZE];
    int counter = 1;
    ssize_t n;

    signal(SIGINT, signal_handler);

    fd_read = open(DEV_SCULL1, O_RDONLY);
    if (fd_read < 0) {
        perror("P3: Failed to open " DEV_SCULL1);
        exit(EXIT_FAILURE);
    }

    fd_write = open(DEV_SCULL2, O_WRONLY);
    if (fd_write < 0) {
        perror("P3: Failed to open " DEV_SCULL2);
        close(fd_read);
        exit(EXIT_FAILURE);
    }

    printf("P3: Started (Reading from %s, Writing to %s). Press Ctrl+C to stop.\n", 
           DEV_SCULL1, DEV_SCULL2);

    while (keep_running) {
        // Чтение данных из scull1
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        if (n < 0) {
            perror("P3: Read from scull1 failed");
        } else if (n > 0) {
            read_buf[n] = '\0';
            printf("P3: Read from scull1: %s\n", read_buf);

            // Обработка и запись в scull2 с проверкой длины
            int written = snprintf(write_buf, BUFFER_SIZE, "P3_FINAL_%d_%s", counter, read_buf);
            if (written >= BUFFER_SIZE) {
                printf("P3: Warning: message truncated\n");
            }
            
            n = write(fd_write, write_buf, strlen(write_buf) + 1);
            if (n < 0) {
                perror("P3: Write to scull2 failed");
            } else {
                printf("P3: Wrote to scull2: %s\n", write_buf);
            }
            counter++;
        }

        usleep(500000);
    }

    printf("P3: Shutting down...\n");
    close(fd_read);
    close(fd_write);
    return 0;
}