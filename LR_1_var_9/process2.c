#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#define DEV1 "/dev/scull0"
#define DEV2 "/dev/scull1"

void generate_data(char *buffer, int size) {
    static int counter = 0;
    time_t now = time(NULL);
    snprintf(buffer, size, "Process2 -> Dev2: Data %d at %ld", counter++, now);
}

int main() {
    int fd1, fd2;
    char write_buffer[256];
    char read_buffer[256];
    ssize_t ret;
    
    printf("Process 2 started (read from dev1, write to dev2)\n");
    
    fd1 = open(DEV1, O_RDONLY);
    if (fd1 < 0) {
        perror("Failed to open dev1");
        exit(1);
    }
    
    fd2 = open(DEV2, O_WRONLY);
    if (fd2 < 0) {
        perror("Failed to open dev2");
        close(fd1);
        exit(1);
    }
    
    while (1) {
        // Чтение из dev1
        ret = read(fd1, read_buffer, sizeof(read_buffer) - 1);
        if (ret > 0) {
            read_buffer[ret] = '\0';
            printf("Process2 read from dev1: %s\n", read_buffer);
        } else if (ret < 0) {
            perror("Read from dev1 failed");
        }
        
        // Запись в dev2
        generate_data(write_buffer, sizeof(write_buffer));
        ret = write(fd2, write_buffer, strlen(write_buffer));
        if (ret > 0) {
            printf("Process2 wrote to dev2: %s\n", write_buffer);
        } else if (ret < 0) {
            perror("Write to dev2 failed");
        }
        
        sleep(2);
    }
    
    close(fd1);
    close(fd2);
    return 0;
}