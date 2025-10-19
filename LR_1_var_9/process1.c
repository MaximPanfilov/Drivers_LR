#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#define DEV1 "/dev/scull0"
#define DEV3 "/dev/scull2"

void generate_data(char *buffer, int size) {
    static int counter = 0;
    time_t now = time(NULL);
    snprintf(buffer, size, "Process1 -> Dev1: Data %d at %ld", counter++, now);
}

int main() {
    int fd1, fd3;
    char write_buffer[256];
    char read_buffer[256];
    ssize_t ret;
    
    printf("Process 1 started (write to dev1, read from dev3)\n");
    
    fd1 = open(DEV1, O_WRONLY);
    if (fd1 < 0) {
        perror("Failed to open dev1");
        exit(1);
    }
    
    fd3 = open(DEV3, O_RDONLY);
    if (fd3 < 0) {
        perror("Failed to open dev3");
        close(fd1);
        exit(1);
    }
    
    while (1) {
        // Чтение из dev3
        ret = read(fd3, read_buffer, sizeof(read_buffer) - 1);
        if (ret > 0) {
            read_buffer[ret] = '\0';
            printf("Process1 read from dev3: %s\n", read_buffer);
        } else if (ret < 0) {
            perror("Read from dev3 failed");
        }
        
        // Запись в dev1
        generate_data(write_buffer, sizeof(write_buffer));
        ret = write(fd1, write_buffer, strlen(write_buffer));
        if (ret > 0) {
            printf("Process1 wrote to dev1: %s\n", write_buffer);
        } else if (ret < 0) {
            perror("Write to dev1 failed");
        }
        
        sleep(2);
    }
    
    close(fd1);
    close(fd3);
    return 0;
}