
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

#define DEV_SCULL1 "/dev/scull_ring1"
#define DEV_SCULL2 "/dev/scull_ring2"
#define BUFFER_SIZE 512

volatile sig_atomic_t keep_running = 1;
int final_counter = 0;

void signal_handler(int sig) {
    keep_running = 0;
}

long get_current_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

void print_timing_info(const char* process, const char* operation, int number, long elapsed_us) {
    printf("[%ld] %s: %s number %d (took %ld us)\n", 
           get_current_time_us(), process, operation, number, elapsed_us);
}

int main() {
    int fd_read, fd_write;
    char read_buf[BUFFER_SIZE];
    char write_buf[BUFFER_SIZE];
    ssize_t n;
    long start_time, end_time;

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
        // Read 1 number from scull1
        start_time = get_current_time_us();
        
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        
        end_time = get_current_time_us();
        
        if (n < 0) {
            perror("P3: Read from scull1 failed");
        } else if (n > 0) {
            read_buf[n] = '\0';
            int received_number = atoi(read_buf);
            print_timing_info("P3-READ", "read from scull1", received_number, end_time - start_time);

            // Process and write 2 numbers to scull2
            for (int i = 0; i < 2 && keep_running; i++) {
                start_time = get_current_time_us();
                
                snprintf(write_buf, BUFFER_SIZE, "%d", final_counter);
                n = write(fd_write, write_buf, strlen(write_buf) + 1);
                
                end_time = get_current_time_us();
                
                if (n < 0) {
                    perror("P3: Write to scull2 failed");
                } else {
                    print_timing_info("P3-WRITE", "wrote to scull2", final_counter, end_time - start_time);
                }
                final_counter++;
            }
        }

        usleep(1000000); // 1 second between iterations
    }

    printf("P3: Shutting down...\n");
    close(fd_read);
    close(fd_write);
    return 0;
}
