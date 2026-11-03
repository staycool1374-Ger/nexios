#include <unistd.h>

/* #389 cross-CPU TLB shootdown E2E sweep probe.
 *
 * Harness contract (kernel test test_tlb_shootdown.cpp):
 *   - maps TEST_VA (read/write user page) and FLAG_VA (shared page,
 *     FLAG[0]=stage 0/1/2, FLAG[1]=observed value, FLAG[2]=mode) into
 *     this task's page table, pins this task to CPU1, dispatches it.
 *   - mode 0 (shootdown lands / negative control): the probe writes
 *     TEST_VA (fills the local TLB on the CPU it runs on), publishes
 *     FLAG[0]=1 (touched), spins until FLAG[0]==2 (go: the harness has
 *     unmapped TEST_VA with or without a cross-CPU shootdown), then
 *     reads TEST_VA into FLAG[1] and exits 7.
 *     Invalidation landed  -> the read faults (EL0 abort -> task
 *     TERMINATED by signal, never exits). Stale TLB (no shootdown) ->
 *     the read succeeds -> exit 7.
 *   - mode 1 (teardown refault): the probe skips the TEST_VA write (no
 *     fill - a stale entry from a torn-down address space is what the
 *     test hunts), publishes FLAG[0]=1, spins for go, reads TEST_VA
 *     into FLAG[1], exits 7. A fresh fill reads the new mapping; a
 *     surviving stale entry reads the old content.
 *   - a compiler + CPU fence sits between the TEST_VA write and the
 *     FLAG[0]=1 publish so the TLB fill is globally performed before
 *     the harness observes readiness (x86 TSO orders it anyway; the
 *     fence pins it on aa64/rv64).
 */

#define TEST_VA_BYTES ((volatile unsigned char *)0x11000000UL)
#define FLAG_WORDS ((volatile unsigned long *)0x11001000UL)

static void publish_ready(void) {
#if defined(__x86_64__)
    __asm__ volatile("mfence" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#elif defined(__riscv)
    __asm__ volatile("fence rw,rw" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

int main(void) {
    volatile unsigned long mode = FLAG_WORDS[2];
    if (mode == 0UL) {
        *TEST_VA_BYTES = 0xA5;
    }
    publish_ready();
    FLAG_WORDS[0] = 1UL;
    while (FLAG_WORDS[0] != 2UL) {
        __asm__ volatile("" ::: "memory");
    }
    FLAG_WORDS[1] = (unsigned long)*TEST_VA_BYTES;
    _exit(7);
    return 7;
}
