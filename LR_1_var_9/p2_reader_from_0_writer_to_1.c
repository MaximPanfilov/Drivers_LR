
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

#define DEV_SCULL0 "/dev/scull_ring0"
#define DEV_SCULL1 "/dev/scull_ring1"
#define BUFFER_SIZE 512

volatile sig_atomic_t keep_running = 1;

void signal_handler(int sig) {
    keep_running = 0;
}

long get_current_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

void print_timing_info(const char* process, const char* operation, int message_num, long elapsed_us) {
    printf("[%ld] %s: %s message %d (took %ld us)\n", 
           get_current_time_us(), process, operation, message_num, elapsed_us);
}

int main() {
    int fd_read, fd_write;
    char read_buf[BUFFER_SIZE];
    char write_buf[BUFFER_SIZE];
    int counter = 1;
    ssize_t n;
    long start_time, end_time;

    signal(SIGINT, signal_handler);

    fd_read = open(DEV_SCULL0, O_RDONLY);
    if (fd_read < 0) {
        perror("P2: Failed to open " DEV_SCULL0);
        exit(EXIT_FAILURE);
    }

    fd_write = open(DEV_SCULL1, O_WRONLY);
    if (fd_write < 0) {
        perror("P2: Failed to open " DEV_SCULL1);
        close(fd_read);
        exit(EXIT_FAILURE);
    }

    printf("P2: Started (Reading from %s, Writing to %s). Press Ctrl+C to stop.\n", 
           DEV_SCULL0, DEV_SCULL1);

    while (keep_running) {
        // Read ONE message from scull0 - 1x speed
        start_time = get_current_time_us();
        
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        
        end_time = get_current_time_us();
        
        if (n < 0) {
            perror("P2: Read from scull0 failed");
        } else if (n > 0) {
            read_buf[n] = '\0'; // Ensure null termination
            print_timing_info("P2-READ", "read from scull0", counter, end_time - start_time);
            printf("    Content: %s\n", read_buf);

            // Process and write to scull1 - 2x faster than read
            for (int i = 0; i < 2 && keep_running; i++) {
                start_time = get_current_time_us();
                
                int written = snprintf(write_buf, BUFFER_SIZE, "P2_PROCESSED_%d_%d_%s", counter, i, read_buf);
                if (written >= BUFFER_SIZE) {
                    printf("P2: Warning: message truncated\n");
                }
                
                n = write(fd_write, write_buf, strlen(write_buf) + 1);
                
                end_time = get_current_time_us();
                
                if (n < 0) {
                    perror("P2: Write to scull1 failed");
                } else {
                    print_timing_info("P2-WRITE", "wrote to scull1", counter * 100 + i, end_time - start_time);
                }
            }
            counter++;
        }

        usleep(1000000); // 1 second between iterations
    }

    printf("P2: Shutting down...\n");
    close(fd_read);
    close(fd_write);
    return 0;
}
