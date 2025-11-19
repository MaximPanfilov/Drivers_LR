
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

#define DEV_SCULL0 "/dev/scull_ring0"
#define DEV_SCULL2 "/dev/scull_ring2"
#define BUFFER_SIZE 512

volatile sig_atomic_t keep_running = 1;
int number_counter = 0;

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
    int fd_write, fd_read;
    char write_buf[BUFFER_SIZE];
    char read_buf[BUFFER_SIZE];
    ssize_t n;
    long start_time, end_time;

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

    printf("P1: Started (Writing numbers to %s, Reading from %s). Press Ctrl+C to stop.\n", 
           DEV_SCULL0, DEV_SCULL2);

    while (keep_running) {
        // Write 2 numbers to scull0
        for (int i = 0; i < 2 && keep_running; i++) {
            start_time = get_current_time_us();
            
            snprintf(write_buf, BUFFER_SIZE, "%d", number_counter);
            n = write(fd_write, write_buf, strlen(write_buf) + 1);
            
            end_time = get_current_time_us();
            
            if (n < 0) {
                perror("P1: Write failed");
            } else {
                print_timing_info("P1-WRITE", "wrote to scull0", number_counter, end_time - start_time);
            }
            number_counter++;
        }

        // Read 1 number from scull2
        start_time = get_current_time_us();
        
        n = read(fd_read, read_buf, BUFFER_SIZE - 1);
        
        end_time = get_current_time_us();
        
        if (n < 0) {
            perror("P1: Read from scull2 failed");
        } else if (n > 0) {
            read_buf[n] = '\0';
            int received_number = atoi(read_buf);
            print_timing_info("P1-READ", "read from scull2", received_number, end_time - start_time);
        }

        usleep(2000000); // 1 second between iterations
    }

    printf("P1: Shutting down...\n");
    close(fd_write);
    close(fd_read);
    return 0;
}
