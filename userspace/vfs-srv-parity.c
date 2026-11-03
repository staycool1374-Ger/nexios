#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    char buf[64];
    int fd = open("/hey.c.elf", 0);
    printf("S VFS seq=%d op=open_initrd rc=%d\n", 0, fd < 0 ? -1 : 0);
    if (fd >= 0) {
        long n = read(fd, buf, sizeof(buf));
        printf("S VFS seq=%d op=read_initrd rc=%d\n", 1, n < 0 ? -1 : 0);
        close(fd);
    }
    fd = open("/proc/meminfo", 0);
    printf("S VFS seq=%d op=open_procfs rc=%d\n", 2, fd < 0 ? -1 : 0);
    if (fd >= 0)
        close(fd);
    int wfd = open("/tmp/parity_probe.tmp", O_CREAT | O_WRONLY);
    printf("S VFS seq=%d op=create_tmpfs rc=%d\n", 3, wfd < 0 ? -1 : 0);
    if (wfd >= 0) {
        long w = write(wfd, "probe", 5);
        printf("S VFS seq=%d op=write_tmpfs rc=%d\n", 4, w != 5 ? -1 : 0);
        close(wfd);
    }
    printf("S VFS seq=%d op=done rc=%d\n", 5, 0);
    return 0;
}
