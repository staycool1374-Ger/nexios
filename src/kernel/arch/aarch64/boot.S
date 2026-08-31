.section .boot.text, "ax"
.globl _start
.type _start, @function

_start:
    // Save DTB address (x0 from bootloader) in x20
    mov     x20, x0

    // Drop to EL1 from whichever EL we boot at
    mrs     x0, CurrentEl
    and     x0, x0, #0xC
    cmp     x0, #0xC
    b.ne    2f
    // At EL3: configure EL2 state, then go directly to EL1
    mov     x0, #1              // SCR_EL3: NS=1 (non-secure)
    msr     scr_el3, x0
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #0x9            // ICC_SRE_EL2: SRE=1, Enable=1
    msr     ICC_SRE_EL2, x0
    isb
    mov     x0, #0x3C5          // SPSR_EL3: EL1h, DAIF masked
    msr     spsr_el3, x0
    adr     x0, 1f
    msr     elr_el3, x0
    eret

2:
    // Drop from EL2 to EL1 if needed
    mrs     x0, CurrentEl
    and     x0, x0, #0xC
    cmp     x0, #0x8
    b.ne    1f
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    // Enable GICv3 system register access from EL1
    mov     x0, #0x9          // ICC_SRE_EL2: SRE=1, Enable=1
    msr     ICC_SRE_EL2, x0
    isb
    mov     x0, #0x3C5
    msr     spsr_el2, x0
    adr     x0, 1f
    msr     elr_el2, x0
    eret

1:
    ldr     x0, =_boot_stack_end
    mov     sp, x0

    // === Init PL011 UART ===
    ldr     x0, =0x9000030
    mov     w1, #0
    str     w1, [x0]
    ldr     x0, =0x9000024
    mov     w1, #27
    str     w1, [x0]
    ldr     x0, =0x9000028
    mov     w1, #8
    str     w1, [x0]
    ldr     x0, =0x900002C
    mov     w1, #0x70
    str     w1, [x0]
    ldr     x0, =0x9000030
    mov     w1, #0x301
    str     w1, [x0]

    // === Identity page tables at 0x40020000 ===
    // L0 at 0x40020000, L1 at 0x40021000, L2 (x4) at 0x40022000-0x40025000
    mov     x9, #0x40020000         // x9 = L0 base

    // Clear L0[0..3]
    str     xzr, [x9]
    str     xzr, [x9, #8]
    str     xzr, [x9, #0x10]
    str     xzr, [x9, #0x18]

    // L1 at 0x40021000
    ldr     x10, =0x40021000
    str     xzr, [x10]
    str     xzr, [x10, #8]
    str     xzr, [x10, #0x10]
    str     xzr, [x10, #0x18]

    // L0[0] = L1
    ldr     x12, =0x40021000
    orr     x12, x12, #3
    str     x12, [x9]

    // L1[0..3] = L2 tables
    ldr     x12, =0x40022000
    orr     x12, x12, #3
    str     x12, [x10]

    ldr     x12, =0x40023000
    orr     x12, x12, #3
    str     x12, [x10, #8]

    ldr     x12, =0x40024000
    orr     x12, x12, #3
    str     x12, [x10, #0x10]

    ldr     x12, =0x40025000
    orr     x12, x12, #3
    str     x12, [x10, #0x18]

    // Fill all 2048 L2 entries with 2MB block descriptors
    // Entries 0-511 (0-1GB): Device (AttrIdx=0) = 0x421
    // Entries 512-639 (1GB-1.25GB): Normal (AttrIdx=1) = 0x725 (DRAM)
    // Entries 640-2047 (1.25GB-4GB): Device (AttrIdx=0) = 0x421
    ldr     x12, =0x40022000
    mov     x11, #2048
    ldr     x14, =0x421     // Device: Valid|Block|NS=1|AF=1 = 0x421
    ldr     x15, =0x725     // Normal: Valid|Block|NS=1|SH=Inner|AF=1|AttrIdx=1 = 0x725

200:
    sub     x11, x11, #1
    cmp     x11, #512
    csel    x13, x14, x15, lo
    cmp     x11, #640
    csel    x13, x14, x13, hs
    lsl     x0, x11, #21
    orr     x0, x0, x13
    str     x0, [x12, x11, lsl #3]
    cbnz    x11, 200b

    // === Higher half mapping via TTBR1 ===
    // VA for kernel: 0xFFFF800040080000
    //   bits 47:39 = 256 → L0[256]
    //   bits 38:30 = 1   → L1[1]
    //   bits 29:21 = 0   → L2[0] (2MB block, 0x40080000)
    //   bits 29:21 = 1   → L2[1] (2MB block, 0x40280000)
    // L0[256] → L1 at 0x40026000 → L2 at 0x40027000

    // L1 table at 0x40026000 - clear 4 entries
    ldr     x12, =0x40026000
    stp     xzr, xzr, [x12]
    stp     xzr, xzr, [x12, #0x10]

    // L2 table at 0x40027000 - clear all 512 entries
    ldr     x13, =0x40027000
    mov     x14, #512
201:
    sub     x14, x14, #1
    str     xzr, [x13, x14, lsl #3]
    cbnz    x14, 201b

    // L0[256] = L1 table at 0x40026000
    ldr     x12, =0x40026000
    orr     x12, x12, #3      // table descriptor
    str     x12, [x9, #256*8]

    // L1[0] = L2 table at 0x40028000 (VA 0xFFFF800000000000-0xFFFF80003FFFFFFF)
    // Clear all 512 entries, then populate needed MMIO mappings
    ldr     x14, =0x40028000
    mov     x15, #512
205:
    sub     x15, x15, #1
    str     xzr, [x14, x15, lsl #3]
    cbnz    x15, 205b
    // Map UART at VA 0xFFFF800009000000 (PA 0x9000000), L2 index 72
    ldr     x14, =0x40028000
    ldr     x12, =0x9000421       // 0x9000000 | Device: Valid|Block|NS|AF
    str     x12, [x14, #72*8]
    // Map GICv2 MMIO at VA 0xFFFF800008000000 (PA 0x08000000), L2 index 64.
    // One 2MiB Device block covers GICD (0x08000000), GICC (0x08010000) and
    // GICH/GICV (0x08020000/0x08030000).  The GIC must be reachable through
    // the TTBR1 kernel window: the TTBR0 identity map does not exist in user
    // page tables, so raw-physical GIC access faults once a user table is
    // loaded (silent sync-exception loop in the IRQ path).
    ldr     x12, =0x8000421       // 0x8000000 | Device: Valid|Block|NS|AF
    str     x12, [x14, #64*8]
    ldr     x12, =0x40028000
    orr     x12, x12, #3
    ldr     x13, =0x40026000  // L1 base
    str     x12, [x13]        // L1[0]

    // L1[1] = L2 table at 0x40027000 (kernel at VA 0xFFFF800040000000)
    ldr     x12, =0x40027000
    orr     x12, x12, #3      // table descriptor
    ldr     x13, =0x40026008  // L1[1]
    str     x12, [x13]

    // Fill L2 entries 0-127 (256MB of RAM at PA 0x40000000-0x4FFFFFFF)
    // Each entry is a 2MB block at 0x725 (Normal WB WA, NS|AF|SH=Inner)
    ldr     x12, =0x40027000  // L2 base
    ldr     x14, =0x725       // NS|AF|SH=11|Attr=1|Table/Block
    ldr     x15, =0x40000000  // current PA (2MB-aligned)
    mov     x16, #128         // counter
210:
    orr     x0, x15, x14      // x0 = PA | flags
    str     x0, [x12], #8     // store at L2[i], advance pointer
    add     x15, x15, #0x200000 // next 2MB block
    subs    x16, x16, #1
    b.ne    210b

    // === Configure MMU ===
    ldr     x8, =0xFF00          // MAIR: Attr0=Device, Attr1=Normal WB WA
    msr     mair_el1, x8

    ldr     x8, =0x5B1103510     // TCR: T0SZ=16, T1SZ=16, TG0=TG1=4KB, EPD1=0, IPS=48b
    msr     tcr_el1, x8

    msr     ttbr0_el1, x9       // TTBR0 = L0 (identity map)
    msr     ttbr1_el1, x9       // TTBR1 = L0 (higher half via L0[256])

    dsb     sy
    isb

    // Clean page table pages (0x40020000-0x40028FFF) and invalidate TLBI
    mov     x11, #0x40020000
    mov     x15, #9
202:
    dc      cvac, x11
    add     x11, x11, #0x1000
    subs    x15, x15, #1
    b.ne    202b
    dsb     sy

    tlbi    vmalle1
    dsb     sy
    isb

    // === Enable MMU (M=1, I=1) ===
    mrs     x0, sctlr_el1
    mov     x1, #0x1001
    orr     x0, x0, x1
    dsb     sy
    msr     sctlr_el1, x0
    isb
    dsb     sy
    isb

    // === Transition to higher-half ===
    // Convert SP from identity-mapped (PA) to higher-half VA.
    // _boot_stack_end is linked at low VMA (identity PA); after MMU enable
    // we must use the TTBR1_EL1 alias (HHDM_OFFSET + PA) so the kernel
    // stack is accessible after TTBR0_EL1 is later switched to user tables.
    ldr     x1, =0xFFFF800000000000
    add     sp, sp, x1

    // Jump via absolute address — this takes us to higherhalf_entry at its
    // higher-half VMA.  From this point forward the kernel runs entirely
    // via TTBR1_EL1 (higher-half) and TTBR0_EL1 can be freely switched.
    mov     x0, x20             // DTB pointer saved at _start
    mov     x1, xzr             // mb_info = 0
    ldr     x2, =higherhalf_entry
    br      x2

.section .boot.data, "aw"
.balign 4096
_boot_stack:
    .space  16384
_boot_stack_end:
