#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>

#define DEV1 "/dev/scull0"
#define DEV2 "/dev/scull1"
#define DEV3 "/dev/scull2"

struct buffer_info {
    int count;
    int size;
    int read_pos;
    int write_pos;
};

#define GET_BUFFER_INFO 0x1001

void print_buffer_info(const char *dev_name, int fd) {
    struct buffer_info info;
    
    if (ioctl(fd, GET_BUFFER_INFO, &info) == 0) {
        printf("%s: Size=%d, Used=%d, Free=%d, ReadPos=%d, WritePos=%d\n", 
               dev_name, info.size, info.count, info.size - info.count, 
               info.read_pos, info.write_pos);
    } else {
        perror("ioctl failed");
    }
}

int main() {
    int fd1, fd2, fd3;
    
    printf("Monitor process started\n");
    
    while (1) {
        fd1 = open(DEV1, O_RDONLY);
        fd2 = open(DEV2, O_RDONLY);
        fd3 = open(DEV3, O_RDONLY);
        
        if (fd1 < 0 || fd2 < 0 || fd3 < 0) {
            perror("Failed to open devices");
            sleep(5);
            continue;
        }
        
        printf("\n=== Buffer Status ===\n");
        print_buffer_info("Device 1", fd1);
        print_buffer_info("Device 2", fd2);
        print_buffer_info("Device 3", fd3);
        printf("====================\n\n");
        
        close(fd1);
        close(fd2);
        close(fd3);
        
        sleep(3);
    }
    
    return 0;
}