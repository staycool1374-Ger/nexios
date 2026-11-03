    .section .boot.text, "ax"
    .globl aa64_ap_entry
    .type aa64_ap_entry, @function

// #351 slice-2: AP entry stub (MMU-off safe, position-independent).
// PSCI_CPU_ON / spin-table release target.  The stub self-identifies via
// MPIDR_EL1 Aff0 against the BSP-published table (one mechanism for both
// wake paths; x0 is ignored because spin-table provides no context id).
// All BSP-published cells are extern "C" (unmangled): s_ap_mpidr[],
// s_ap_count, s_ap_boot[] ([ttbr, mair, tcr, hhdm, percpu_hh]),
// s_ap_stack[] (per-logical stack-top PA, ABI-biased).  Installs the
// published kernel TTBRs, enables the MMU, switches to the AP's stack +
// TPIDR, then calls ap_main(logical, mpidr).  Uses x9-x19 (the C call
// below takes x0/x1 only).
aa64_ap_entry:
    // Identify: Aff0 -> logical via the published table (bounded scan).
    mrs     x9, mpidr_el1
    and     x9, x9, #0xFF              // x9 = Aff0
    ldr     x10, =s_ap_mpidr          // literal HH VA (pool is PA-readable)
    ldr     x11, =0xFFFF800000000000
    sub     x10, x10, x11             // x10 = table PA (MMU off)
    ldr     x11, =s_ap_count
    ldr     x12, =0xFFFF800000000000
    sub     x11, x11, x12
    ldr     x11, [x11]                // x11 = published count
    mov     x12, #0                   // x12 = logical cursor
1:
    cmp     x12, x11
    b.hs    9f                        // overrun: unknown topology -> halt
    cmp     x12, #8
    b.hs    9f                        // hard cap (CONFIG_MAX_CPUS == 8)
    lsl     x13, x12, #3
    ldr     x14, [x10, x13]           // x14 = table[logical]
    cmp     x14, x9
    b.eq    2f                        // Aff0 match
    add     x12, x12, #1
    b       1b
2:
    // Load the published translation regime (shared cells).
    ldr     x10, =s_ap_boot
    ldr     x13, =0xFFFF800000000000
    sub     x10, x10, x13             // x10 = cells PA
    ldr     x15, [x10]                // x15 = kernel TTBR phys
    ldr     x16, [x10, #8]            // x16 = MAIR
    ldr     x17, [x10, #16]           // x17 = TCR
    ldr     x19, [x10, #24]           // x19 = HHDM slide
    ldr     x14, [x10, #32]           // x14 = per_cpu HH base
    // Load this AP's stack top (per-logical cell).
    ldr     x10, =s_ap_stack
    sub     x10, x10, x13             // x10 = stack table PA
    lsl     x13, x12, #3
    ldr     x18, [x10, x13]           // x18 = stack-top PA (ABI-biased)

    // Install regime (mirrors boot.S ordering).
    msr     mair_el1, x16
    msr     tcr_el1, x17
    msr     ttbr0_el1, x15
    msr     ttbr1_el1, x15
    dsb     sy
    isb
    tlbi    vmalle1
    dsb     sy
    isb
    ic      ialluis
    dsb     sy
    isb

    // Enable MMU (M=1, I=1 — mirrors boot.S; C stays off like the BSP).
    mrs     x13, sctlr_el1
    mov     x10, #0x1001
    orr     x13, x13, x10
    dsb     sy
    msr     sctlr_el1, x13
    isb
    dsb     sy
    isb

    // Higher half: stack, TPIDR, FPEN, then C.
    mov     sp, x18
    add     sp, sp, x19               // SP = stack PA + HHDM
    mov     x13, #4096
    mul     x13, x12, x13
    add     x14, x14, x13             // x14 = &per_cpu[logical] (HH VA)
    msr     tpidr_el1, x14
    isb
    mov     x14, #(3 << 20)           // CPACR_EL1: FPEN=0b11 (mirrors boot.S:
    msr     cpacr_el1, x14            // release C++ may emit NEON immediately)
    isb
    mov     x0, x12                   // logical id
    mrs     x1, mpidr_el1             // full MPIDR
    ldr     x16, =ap_main
    br      x16

    // Unreachable: fall into the halt below if ap_main ever returns.
9:
    // Unknown MPIDR: raw-PL011 diagnostic ("SMP?") + halt.  Mirrors the
    // slice-0 secondary halt style (raw UART, polled, no init dependency).
    ldr     x1, =0x9000000
5:
    ldr     w2, [x1, #0x18]
    tst     w2, #(1 << 5)
    b.ne    5b
    mov     w2, #0x53                 // 'S'
    str     w2, [x1]
4:
    ldr     w2, [x1, #0x18]
    tst     w2, #(1 << 5)
    b.ne    4b
    mov     w2, #0x4D                 // 'M'
    str     w2, [x1]
3:
    ldr     w2, [x1, #0x18]
    tst     w2, #(1 << 5)
    b.ne    3b
    mov     w2, #0x50                 // 'P'
    str     w2, [x1]
7:
    ldr     w2, [x1, #0x18]
    tst     w2, #(1 << 5)
    b.ne    7b
    mov     w2, #0x3F                 // '?'
    str     w2, [x1]
6:
    wfi
    b       6b
    .size aa64_ap_entry, .-aa64_ap_entry
