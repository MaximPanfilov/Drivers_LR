#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#define DEV2 "/dev/scull1"
#define DEV3 "/dev/scull2"

void generate_data(char *buffer, int size) {
    static int counter = 0;
    time_t now = time(NULL);
    snprintf(buffer, size, "Process3 -> Dev3: Data %d at %ld", counter++, now);
}

int main() {
    int fd2, fd3;
    char write_buffer[256];
    char read_buffer[256];
    ssize_t ret;
    
    printf("Process 3 started (read from dev2, write to dev3)\n");
    
    fd2 = open(DEV2, O_RDONLY);
    if (fd2 < 0) {
        perror("Failed to open dev2");
        exit(1);
    }
    
    fd3 = open(DEV3, O_WRONLY);
    if (fd3 < 0) {
        perror("Failed to open dev3");
        close(fd2);
        exit(1);
    }
    
    while (1) {
        // Чтение из dev2
        ret = read(fd2, read_buffer, sizeof(read_buffer) - 1);
        if (ret > 0) {
            read_buffer[ret] = '\0';
            printf("Process3 read from dev2: %s\n", read_buffer);
        } else if (ret < 0) {
            perror("Read from dev2 failed");
        }
        
        // Запись в dev3
        generate_data(write_buffer, sizeof(write_buffer));
        ret = write(fd3, write_buffer, strlen(write_buffer));
        if (ret > 0) {
            printf("Process3 wrote to dev3: %s\n", write_buffer);
        } else if (ret < 0) {
            perror("Write to dev3 failed");
        }
        
        sleep(2);
    }
    
    close(fd2);
    close(fd3);
    return 0;
}