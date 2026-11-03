#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>

struct dirent {
    char d_name[64];
    unsigned long d_ino;
};

int main(void) {
    int fd = open("/mnt", 0);
    printf("S BLK seq=%d op=open_mnt rc=%d\n", 0, fd < 0 ? -1 : 0);
    unsigned long entries = 0;
    if (fd >= 0) {
        unsigned long pos = 0;
        struct dirent dent;
        for (int i = 0; i < 64; ++i) {
            long r = __syscall5(16, (long)fd, (long)&pos, (long)&dent, 0);
            if (r < 0)
                break;
            if (dent.d_name[0] == '\0')
                continue;
            ++entries;
        }
        printf("S BLK seq=%d op=readdir_mnt rc=%d\n", 1, 0);
        close(fd);
    }
    printf("S BLK seq=%d op=entries rc=%lu\n", 2, entries);
    printf("S BLK seq=%d op=done rc=%d\n", 3, 0);
    return 0;
}
