#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define DEV_SCULL0 "/dev/scull_ring0"
#define DEV_SCULL2 "/dev/scull_ring2"
#define BUFFER_SIZE 512  // Увеличили размер буфера

volatile sig_atomic_t keep_running = 1;

void signal_handler(int sig) {
    keep_running = 0;
}

int main() {
    int fd_write, fd_read;
    char write_buf[BUFFER_SIZE];
    char read_buf[BUFFER_SIZE];
    int counter = 1;
    ssize_t n;

    signal(SIGINT, signal_handler);

    fd_write = open(DEV_SCULL0, O_WRONLY);
    if (fd_write < 0) {
        perror("P1: Failed to open " DEV_SCULL0);
        exit(EXIT_FAILURE);
    }

    fd_read = open(DEV_SCULL2, O_RDONLY);
    if (fd_read < 0) {
        perror("P1: Failed to open " DEV_SCULL2);
        close(fd_write);
        exit(EXIT_FAILURE);
    }

    printf("P1: Started (Writing to %s, Reading from %s). Press Ctrl+C to stop.\n", 
           DEV_SCULL0, DEV_SCULL2);

    while (keep_running) {
        // Генерация и запись данных в scull0
        snprintf(write_buf, BUFFER_SIZE, "P1_DATA_%d", counter);
        n = write(fd_write, write_buf, strlen(write_buf) + 1);
        if (n < 0) {
            perror("P1: Write failed");
        } else {
            printf("P1: Wrote to scull0: %s\n", write_buf);
        }

        // Чтение данных из scull2
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        if (n < 0) {
            perror("P1: Read from scull2 failed");
        } else if (n > 0) {
            read_buf[n] = '\0';
            printf("P1: Read from scull2: %s\n", read_buf);
        }

        counter++;
        usleep(3000000);
    }

    printf("P1: Shutting down...\n");
    close(fd_write);
    close(fd_read);
    return 0;
}