.section .text.boot
.global _start
_start:
    /* OpenSBI starts us in S-mode with:
     *   a0 = hart ID
     *   a1 = DTB pointer
     *   satp = 0 (Bare)
     */

    /* Save boot info */
    mv s0, a0
    mv s1, a1

    /* Only hart 0 proceeds; park others */
    li t0, 0
    beq a0, t0, 1f
0:  wfi
    j 0b
1:

    /* Issue #29: the fixed 64KB .boot_stack_rv at 0x80E00000 must clear
     * the image + PMM metadata (this ld rejects ASSERT, so fail-stop
     * here instead — before the stack is used, no stack frames,
     * s0/s1 preserved).  The 2MB slack covers PMM metadata for RAM up
     * to ~990MB (8.25B/page); larger RAM configs must move the stack. */
    la t0, kernel_phys_end
    li t1, 0x80C00000         /* stack base - 2MB metadata slack */
    /* RV64 LUI sign-extends bit 31, so the li above yields
     * 0xFFFFFFFF80C00000, not 0x0000000080C00000; the unsigned bltu
     * below would then be always-taken and the fail-stop dead.
     * Zero-extend into the true physical domain before comparing. */
    slli t1, t1, 32
    srli t1, t1, 32
    bltu t0, t1, 10f
    li a0, 33                 /* '!' */
    li a7, 1
    ecall
    li a7, 8                  /* SBI shutdown */
    ecall
9:  wfi
    j 9b
10:

    /* Temporary stack (physical address, identity-mapped) */
    la sp, _boot_stack_rv_top

    /* ---- Sv39 Page Table Setup ----
     * Non-leaf PTEs: flags=0x1 (V only, R=W=X=0)
     * Leaf PTEs:     flags=0xEF (V|R|W|X|G|A|D, U=0)
     * PPN in PTE bits 53:10: address >> 2 for 4K-aligned addresses
     */

    li t4, 0xEF               /* leaf flags */
    li t5, 0x1                /* non-leaf flags */

    /* ---- L0 entries ---- */
    la t0, _boot_pt_l0

    /* L0[258] -> L1 (higher-half kernel mapping, offset 258*8=2064) */
    la t1, _boot_pt_l1
    srli t1, t1, 2
    or t1, t1, t5
    li t2, 2064
    add t2, t0, t2
    sd t1, 0(t2)

    /* L0[2] -> L1_id (identity mapping for boot 2MB page, offset 2*8=16) */
    la t1, _boot_pt_l1_id
    srli t1, t1, 2
    or t1, t1, t5
    sd t1, 16(t0)

    /* L0[0] -> L2_page (lower-half mapping for MMIO range, offset 0*8=0) */
    la t1, _boot_pt_l2
    srli t1, t1, 2
    or t1, t1, t5
    sd t1, 0(t0)

    /* ---- Higher-half 2MB pages (256MB physical RAM at 0x80000000) ---- */
    /* Map all 128 × 2MB pages of QEMU RAM (PA 0x80000000-0x8FFFFFFF)
     * into the HHDM higher-half range (VA 0xFFFFFFC080000000-0xFFFFFFC08FFFFFFF). */
    la t2, _boot_pt_l1
    li t3, 0x80000            /* base PPN = 0x80000000 >> 12 */
    li t6, 128                /* 256MB / 2MB */
1:  slli t1, t3, 10           /* PPN << 10 */
    or t1, t1, t4             /* | leaf flags */
    sd t1, 0(t2)              /* store PTE */
    addi t2, t2, 8            /* next PTE */
    addi t3, t3, 0x200        /* next PPN (+512 = 2MB >> 12) */
    addi t6, t6, -1           /* decrement count */
    bnez t6, 1b

    /* L1_id[1]: 2MB leaf PA 0x80200000 (identity mapping) */
    li t1, 0x80200
    slli t1, t1, 10
    or t1, t1, t4
    la t2, _boot_pt_l1_id
    sd t1, 8(t2)                /* 1 * 8 = 8 */

    /* L1_id[i]: 2MB identity leaf for the 64KB boot stack (issue #29).
     * The stack lives at _boot_stack_rv (linker-placed, clear of the image
     * and PMM metadata); derive the leaf index and PPN from the symbol so
     * relocation never desyncs the table. */
    la t0, _boot_stack_rv_top
    srli t0, t0, 21
    andi t0, t0, 0x1FF
    slli t0, t0, 3              /* byte offset into L1 */
    la t1, _boot_pt_l1_id
    add t1, t1, t0              /* &L1_id[i] */
    la t0, _boot_stack_rv
    li t2, -0x200000
    and t0, t0, t2              /* 2MB-aligned base */
    srli t0, t0, 2              /* PPN field = base >> 2 */
    or t0, t0, t4               /* | leaf flags */
    sd t0, 0(t1)

    /* ---- Lower-half 2MB pages (MMIO) ---- */

    /* L2_page[96]: 2MB leaf PA 0x0C000000 (PLIC priority/pending/enable) */
    li t1, 0x0C000
    slli t1, t1, 10
    or t1, t1, t4
    la t2, _boot_pt_l2
    sd t1, 768(t2)            /* 96 * 8 = 768 */

    /* L2_page[97]: 2MB leaf PA 0x0C200000 (PLIC threshold/claim) */
    li t1, 0x0C200
    slli t1, t1, 10
    or t1, t1, t4
    sd t1, 776(t2)            /* 97 * 8 = 776 */

    /* L2_page[128]: 2MB leaf PA 0x10000000 (UART) */
    li t1, 0x10000
    slli t1, t1, 10
    or t1, t1, t4
    sd t1, 1024(t2)           /* 128 * 8 = 1024 */

    /* L2_page[384..511]: 128x2MB identity leaves PA 0x30000000-0x3FFFFFFF
     * (PCIe ECAM, issue #29; QEMU virt `info mtree` proof: pcie-ecam @
     * 0x30000000, 256MB).  Identity (not HHDM-aliased): follows the
     * in-tree PLIC/UART precedent above — L0[0] already covers VA
     * 0x0-0x40000000, so raw-phys ECAM access is valid in S-mode. */
    la t2, _boot_pt_l2
    li t1, 3072              /* 384 * 8: first ECAM leaf slot */
    add t2, t2, t1
    li t3, 0x30000           /* base PPN = 0x30000000 >> 12 */
    li t6, 128               /* 256MB / 2MB */
2:  slli t1, t3, 10          /* PPN << 10 */
    or t1, t1, t4            /* | leaf flags */
    sd t1, 0(t2)             /* store PTE */
    addi t2, t2, 8           /* next PTE */
    addi t3, t3, 0x200       /* next PPN (+512 = 2MB >> 12) */
    addi t6, t6, -1          /* decrement count */
    bnez t6, 2b

    /* ---- Enable MMU ---- */
    la t0, _boot_pt_l0
    srli t0, t0, 12
    li t1, 8
    slli t1, t1, 60           /* Sv39 << 60 */
    or t0, t1, t0
    csrw satp, t0
    sfence.vma

    /* ---- Higher-half transition ---- */
    la sp, _boot_stack_rv_top
    li t0, 0xFFFFFFC0
    slli t0, t0, 32           /* 0xFFFFFFC000000000 */
    add sp, sp, t0

    /* Set trap vector (stvec) */
    la t0, .L_trapvec_ptr
    ld t0, 0(t0)
    csrw stvec, t0

    /* Jump to higherhalf_entry.
     * .text VMA now matches the HHDM page-table mapping (shifted by +0x7000),
     * so no manual adjustment needed.
     */
    la t0, .L_higherhalf_ptr
    ld t0, 0(t0)
    mv a0, s0                 /* hart ID */
    mv a1, s1                 /* DTB pointer */
    jalr ra, t0

    /* Should not return. SBI shutdown if we do. */
    li a7, 8
    ecall

.align 3
.L_higherhalf_ptr:
    .quad higherhalf_entry
.L_trapvec_ptr:
    .quad trap_vector

.section .boot.bss, "aw", @nobits
.align 12
_boot_pt_l0:
    .space 4096
_boot_pt_l1:
    .space 4096
_boot_pt_l2:
    .space 4096
_boot_pt_l1_id:
    .space 4096
_boot_pt_l2_id:
    .space 4096
/* LOAD-BEARING PADDING: .boot must still end at 0x80207000 (= boot_vma_end
 * = .text LMA).  The fixed .text VMA (0xFFFFFFC080207000) requires LMA
 * 0x80207000 (VMA = HHDM_base + (LMA & 0x1FFFFF) within the shared 2MB
 * page).  The 64KB stack moved to .boot_stack_rv; this 4KB reservation
 * keeps every downstream LMA byte-stable after its removal. */
_boot_reserved:
    .space 4096

/* Issue #29: dedicated 64KB boot stack, clear of the page tables above.
 * The old 4KB _boot_stack sat directly above the tables in .boot.bss;
 * boot depth (~24KB by admission time incl. ~8KB stack TCBs) overflowed
 * it and scribbled PTEs.  64KB covers observed depth + TCBs + margin.
 * VMA==LMA (low identity) for pre-transition use; the transition adds
 * HHDM (all-RAM cover) like before.  Lifetime: image-resident
 * (.boot_stack_rv output section) + PMM::reserve_range, so the allocator
 * never hands it out while the BSP is parked on it pre-scheduler. */
.section .boot_stack_rv, "aw", @nobits
.align 12
_boot_stack_rv:
    .space 65536
_boot_stack_rv_top:
