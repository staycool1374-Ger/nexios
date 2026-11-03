    .section .text.boot.ap_entry, "ax"
    .globl rv64_ap_entry
    .type rv64_ap_entry, @function

// #351 slice-3: AP entry stub (MMU-off safe, fully position-independent).
// HSM hart_start target: a0 = hartid, a1 = opaque (PA of this AP's
// ApParam handoff block, placed by the BSP in a PMM page).  HSM starts
// harts bare in S-mode (satp=0), so every address below is physical
// until the kernel root is installed.  The stub is symbol-free (local
// labels + SBI immediates only): the BSP passes everything the stub
// needs in the block (satp value, stack PA/HH, per_cpu HH, entry HH,
// logical id, hartid), so no PC-relative reach or absolute-address
// materialization is ever needed here.  Installs the provided root,
// switches to the AP's stack + tp, then calls ap_main(logical, hartid).
// Uses t0-t6 (no stack yet); s0/s1 carry hartid/block-PA across MMU-on.
rv64_ap_entry:
    // Save entry state (a0 = hartid, a1 = param-block PA).
    mv      s0, a0
    mv      s1, a1

    // Sanity: opaque must be a low identity-mapped address.  A wild
    // opaque (firmware bug) would install garbage satp — halt instead.
    li      t6, 0x80000000
    bltu    s1, t6, 9f
    li      t6, 0x90000000
    bgeu    s1, t6, 9f

    // Load the handoff block (PA: identity-accessible with MMU off).
    ld      t0, 0(s1)           // t0 = satp value (MODE|PPN, BSP-built)
    ld      t1, 8(s1)           // t1 = stack-top PA (16-byte aligned)
    ld      t2, 16(s1)          // t2 = stack-top HH (same stack, HH alias)
    ld      t3, 24(s1)          // t3 = per_cpu HH base
    ld      t4, 32(s1)          // t4 = ap_main HH entry
    ld      t5, 40(s1)          // t5 = logical id (echo-checked below)
    // t6 = hartid echo (s0 already holds it; kept for symmetry).

    // Validate: Sv39 mode + in-range logical id.  Anything else means a
    // corrupt block or a wrong-firmware handoff — halt, never boot blind.
    srli    t6, t0, 60
    li      t0, 8
    // NOTE: t0 reused below; satp value reloaded after the check.
    bne     t6, t0, 9f
    li      t6, 8
    bgeu    t5, t6, 9f

    // Install the kernel root (mirrors boot.S ordering).
    ld      t0, 0(s1)
    csrw    satp, t0
    sfence.vma zero, zero
    fence.i

    // Higher half: stack + tp, then C.  SP is set exactly once, to the
    // HH alias (the PA form is only valid pre-MMU and only inside the
    // 2 MiB identity window, which a PMM stack is not guaranteed to be
    // in — so never run with SP=PA after satp is on).
    mv      sp, t2
    slli    t6, t5, 12          // t6 = logical * 4096 (page index)
    add     t3, t3, t6          // t3 = &per_cpu[logical] (HH VA)
    mv      tp, t3
    fence.i

    // ap_main(logical, hartid).
    mv      a0, t5                // logical id (block echo, range-checked)
    mv      a1, s0                // hartid (HSM a0, preserved)
    jr      t4

    // Unreachable: fall into the halt below if ap_main ever returns.
9:
    // Unexpected entry state: SBI console diagnostic ("SMP?") + halt.
    // SBI calls are MMU-independent (ecall), safe from any state.
    li      a0, 83               // 'S'
    li      a7, 1
    ecall
    li      a0, 77               // 'M'
    li      a7, 1
    ecall
    li      a0, 80               // 'P'
    li      a7, 1
    ecall
    li      a0, 63               // '?'
    li      a7, 1
    ecall
6:
    wfi
    j       6b
    .size rv64_ap_entry, .-rv64_ap_entry
