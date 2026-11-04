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
 * not a script concern.
 *
 * C++ / constructor support (issue #455 item 3): .init_array (+ .fini
 * for symmetry, unwalked) with PROVIDE start/end symbols. crt0 walks
 * .init_array bounded and NULL-safe between TLS_SET and main; the weak
 * references in crt0.S keep toolchain-default links (no .init_array
 * inputs) working with a zero-length skip. */

SECTIONS
{
    .nexios_sdk :
    {
        KEEP(*(.nexios_sdk))
    }

    .init_array :
    {
        PROVIDE(__init_array_start = .);
        KEEP(*(.init_array .init_array.*))
        PROVIDE(__init_array_end = .);
    }

    .fini_array :
    {
        PROVIDE(__fini_array_start = .);
        KEEP(*(.fini_array .fini_array.*))
        PROVIDE(__fini_array_end = .);
    }
} INSERT AFTER .rodata;
