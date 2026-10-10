// Probe for issue #316: deterministic %lu/%llu/%lx vectors with
// correctly-typed arguments (in-tree libc link). If every line prints
// exactly, the observed misrender came from the caller side (wrong arg
// types desyncing va_arg stepping), not from vsnprintf itself.
// Single- and multi-arg forms, including the exact failing shape
// `bytes=%lu blocks=%lu`. Lines stay < 200 B (printf uses a 256 B
// stack buffer). Built by the generic userspace rule; run via
// loadelf /bin/printf-probe.c.elf + runelf over a scripted handoff.
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int tty = open("/dev/tty", O_RAWTTY);
    if (tty < 0)
        return 1;
    (void)dup2(tty, 1);
    {
        unsigned long z = 0;
        unsigned long one = 1;
        unsigned long v300 = 300;
        unsigned long v384 = 384;
        unsigned long u32max = 4294967295UL;
        unsigned long u32p1 = 4294967296UL;
        unsigned long u64max = 18446744073709551615UL;
        unsigned long long ll300 = 300ULL;
        unsigned long long llmax = 18446744073709551615ULL;
        printf("P01=%lu\n", z);
        printf("P02=%lu\n", one);
        printf("P03=%lu\n", v300);
        printf("P04=%lu\n", v384);
        printf("P05=%lu\n", u32max);
        printf("P06=%lu\n", u32p1);
        printf("P07=%lu\n", u64max);
        printf("P08=%llu\n", ll300);
        printf("P09=%llu\n", llmax);
        printf("P10=%lx\n", (unsigned long)0x0);
        printf("P11=%lx\n", (unsigned long)0x300);
        printf("P12=%lx\n", (unsigned long)0xDEADBEEFCAFEBABEUL);
        printf("P13 bytes=%lu blocks=%lu\n", v300, one);
        printf("P14 bytes=%lu blocks=%lu\n", v384, one);
        printf("P15 mix %lu %u %lu\n", v300, 7U, v384);
        printf("P16 mix %d %lu %x\n", -3, v300, 255U);
        printf("PROBE DONE\n");
    }
    return 0;
}
