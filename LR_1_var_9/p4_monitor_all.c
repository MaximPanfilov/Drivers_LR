
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

// MUST MATCH THE DRIVER - changed to 10
#define SCULL_RING_IOCTL_GET_STATUS _IOR('s', 1, int[4])
#define SCULL_RING_IOCTL_GET_COUNTERS _IOR('s', 2, long[2])
#define SCULL_RING_IOCTL_PEEK_BUFFER _IOWR('s', 10, char[256])  // Changed to 10

#define DEV_SCULL0 "/dev/scull_ring0"
#define DEV_SCULL1 "/dev/scull_ring1"
#define DEV_SCULL2 "/dev/scull_ring2"

volatile sig_atomic_t keep_running = 1;

void signal_handler(int sig) {
    keep_running = 0;
}

void print_timestamp() {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    printf("[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
}

void print_detailed_status(int fd, const char* dev_name, long *last_reads, long *last_writes, int dev_index) {
    int status[4];
    long counters[2];
    char buffer_content[256];
    static int first_run[3] = {1, 1, 1};
    int ret;
    
    // Get status
    ret = ioctl(fd, SCULL_RING_IOCTL_GET_STATUS, status);
    if (ret != 0) {
        print_timestamp();
        printf("%s: Error reading status (errno=%d: %s)\n", dev_name, errno, strerror(errno));
        return;
    }
    
    // Get counters
    ret = ioctl(fd, SCULL_RING_IOCTL_GET_COUNTERS, counters);
    if (ret != 0) {
        print_timestamp();
        printf("%s: Error reading counters (errno=%d: %s)\n", dev_name, errno, strerror(errno));
        return;
    }
    
    long read_diff = 0, write_diff = 0;
    
    if (!first_run[dev_index]) {
        read_diff = counters[0] - last_reads[dev_index];
        write_diff = counters[1] - last_writes[dev_index];
    } else {
        first_run[dev_index] = 0;
    }
    
    // Get buffer content
    memset(buffer_content, 0, sizeof(buffer_content));
    ret = ioctl(fd, SCULL_RING_IOCTL_PEEK_BUFFER, buffer_content);
    if (ret != 0) {
        print_timestamp();
        printf("%s: Data=%d/%d (%.1f%%)", 
               dev_name, status[0], status[1], 
               (float)status[0] / status[1] * 100.0);
        
        if (read_diff > 0 || write_diff > 0) {
            printf(" [R:+%ld W:+%ld]", read_diff, write_diff);
        }
        printf(" [Total:R%ld W%ld]\n", counters[0], counters[1]);
        printf("    PEEK ERROR: errno=%d (%s)\n", errno, strerror(errno));
    } else {
        print_timestamp();
        printf("%s: Data=%d/%d (%.1f%%)", 
               dev_name, status[0], status[1], 
               (float)status[0] / status[1] * 100.0);
        
        if (read_diff > 0 || write_diff > 0) {
            printf(" [R:+%ld W:+%ld]", read_diff, write_diff);
        }
        printf(" [Total:R%ld W%ld]\n", counters[0], counters[1]);
        printf("    Content: %s\n", buffer_content);
    }
    
    last_reads[dev_index] = counters[0];
    last_writes[dev_index] = counters[1];
}

int main() {
    int fd0, fd1, fd2;
    int iteration = 0;
    long last_reads[3] = {0}, last_writes[3] = {0};

    signal(SIGINT, signal_handler);

    fd0 = open(DEV_SCULL0, O_RDONLY);
    fd1 = open(DEV_SCULL1, O_RDONLY);
    fd2 = open(DEV_SCULL2, O_RDONLY);

    if (fd0 < 0 || fd1 < 0 || fd2 < 0) {
        perror("P4: Failed to open one or more devices");
        exit(EXIT_FAILURE);
    }

    printf("P4: Enhanced Monitor Started. Using PEEK IOCTL command 10\n");
    printf("Press Ctrl+C to stop.\n\n");

    while (keep_running) {
        system("clear");
        printf("=== Scull Ring Buffers - Enhanced Monitor (Iteration: %d) ===\n\n", iteration++);
        
        print_detailed_status(fd0, "scull0", last_reads, last_writes, 0);
        printf("\n");
        print_detailed_status(fd1, "scull1", last_reads, last_writes, 1);
        printf("\n");
        print_detailed_status(fd2, "scull2", last_reads, last_writes, 2);
        
        printf("\nLegend: Data=current/size (fill%%), [R:+reads W:+writes] [Total:Rtotal Wtotal]\n");
        printf("Refreshing every 2 seconds...\n");
        sleep(2);
    }

    printf("P4: Shutting down...\n");
    close(fd0);
    close(fd1);
    close(fd2);
    return 0;
}
