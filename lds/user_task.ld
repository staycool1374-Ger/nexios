ENTRY(_start)

/* NexIOS user-task link contract (issue #171 P5).
 *
 * Augments — never replaces — the toolchain default script: per-arch
 * load origins stay toolchain defaults (x86_64/aarch64 0x400000,
 * riscv64 0x10000), so one file serves all three arches. Consumed via
 * `-T user_task.ld` by driver builds (the staged-tree reference build
 * in P6 consumes it; link-consumption E2E is deferred to P6).
 *
 * Homes .nexios_sdk explicitly (KEEP, non-ALLOC PROGBITS): the SDK
 * identity section must survive --gc-sections and must never land in
 * a PT_LOAD (C4 audit gate: no kernel-half addresses in driver
 * images). Stack alignment stays a link-flag concern
 * (-Wl,-z,max-page-size=0x1000, as in the in-tree userspace rule),
 * not a script concern. */

SECTIONS
{
    .nexios_sdk :
    {
        KEEP(*(.nexios_sdk))
    }
} INSERT AFTER .rodata;
