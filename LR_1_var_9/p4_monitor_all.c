#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>

#define SCULL_RING_IOCTL_GET_STATUS _IOR('s', 1, int[4])
#define SCULL_RING_IOCTL_GET_COUNTERS _IOR('s', 2, long[2])

#define DEV_SCULL0 "/dev/scull_ring0"
#define DEV_SCULL1 "/dev/scull_ring1"
#define DEV_SCULL2 "/dev/scull_ring2"

volatile sig_atomic_t keep_running = 1;

void signal_handler(int sig) {
    keep_running = 0;
}

void print_status(int fd, const char* dev_name) {
    int status[4];
    long counters[2];
    
    if (ioctl(fd, SCULL_RING_IOCTL_GET_STATUS, status) == 0) {
        printf("%s: Data: %d/%d, Free: %d/%d", 
               dev_name, status[0], status[1], status[1] - status[0], status[1]);
        
        // Получаем счетчики операций
        if (ioctl(fd, SCULL_RING_IOCTL_GET_COUNTERS, counters) == 0) {
            printf(" [R:%ld W:%ld]", counters[0], counters[1]);
        }
        printf("\n");
    } else {
        printf("%s: Error reading status\n", dev_name);
    }
}

int main() {
    int fd0, fd1, fd2;

    signal(SIGINT, signal_handler);

    fd0 = open(DEV_SCULL0, O_RDONLY);
    fd1 = open(DEV_SCULL1, O_RDONLY);
    fd2 = open(DEV_SCULL2, O_RDONLY);

    if (fd0 < 0 || fd1 < 0 || fd2 < 0) {
        perror("P4: Failed to open one or more devices");
        exit(EXIT_FAILURE);
    }

    printf("P4: Monitor Started. Press Ctrl+C to stop.\n");

    while (keep_running) {
        system("clear");
        printf("=== Scull Ring Buffers Status ===\n");
        
        print_status(fd0, "scull0");
        print_status(fd1, "scull1"); 
        print_status(fd2, "scull2");
        
        printf("\nRefreshing every 2 seconds...\n");
        sleep(2);
    }

    printf("P4: Shutting down...\n");
    close(fd0);
    close(fd1);
    close(fd2);
    return 0;
}