// Tiny hello for #314 loadelf-from-ramdisk proof (keep small; the ELF
// is uploaded over slow XMODEM, so every kilobyte costs minutes).
#include <stdio.h>

int main(void) {
    printf("hi-ramdisk\n");
    return 0;
}
