.section .vectors, "ax"
.globl vectors
.globl exception_entry
.align 12

// AArch64 exception vector table (16 entries, each 0x80 apart)
// Entries 0-3:  Current EL with SP0
// Entries 4-7:  Current EL with SPx
// Entries 8-11: Lower EL using AArch64
// Entries 12-15: Lower EL using AArch32

vectors:
exception_entry:
    // Current EL with SP0
    .align 7
    b       default_exception  // Sync
    .align 7
    b       default_exception  // IRQ
    .align 7
    b       default_exception  // FIQ
    .align 7
    b       default_exception  // SError

    // Current EL with SPx (kernel runs in EL1h, so this is the active set)
    .align 7
    b       default_exception  // Sync (EL1t sync — unused, kernel uses EL1h)
    .align 7
    b       el1_irq            // IRQ
    .align 7
    b       default_exception  // FIQ
    .align 7
    b       default_exception  // SError

    // Lower EL using AArch64
    .align 7
    b       el0_sync           // Sync (SVC from EL0 → syscall)
    .align 7
    b       el0_irq            // IRQ while at EL0
    .align 7
    b       default_exception  // FIQ
    .align 7
    b       default_exception  // SError

    // Lower EL using AArch32
    .align 7
    b       default_exception  // Sync
    .align 7
    b       default_exception  // IRQ
    .align 7
    b       default_exception  // FIQ
    .align 7
    b       default_exception  // SError

.data
.align 3
.globl g_debug_esr
g_debug_esr: .quad 0
.globl g_debug_far
g_debug_far: .quad 0

.section .text
.type default_exception, @function
default_exception:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, esr_el1
    adrp    x1, g_debug_esr
    str     x0, [x1, #:lo12:g_debug_esr]
    mrs     x0, far_el1
    adrp    x1, g_debug_far
    str     x0, [x1, #:lo12:g_debug_far]
    mrs     x0, elr_el1
    add     x0, x0, #4
    msr     elr_el1, x0
    ldp     x0, x1, [sp], #16
    eret

// EL0 sync handler — SVC from userspace → syscall_entry
// NOTE: any lower-EL sync exception lands here, not just SVC.  Check ESR_EL1
// EC: 0x15 = SVC from AArch64 → syscall path; anything else is a user-task
// fault (data/page abort etc.) and must NOT be dispatched as a syscall
// (ereting to the faulting PC would loop forever).
//
// EL0 entry also MUST switch SP_EL1 to the current task's own kernel stack
// (the aarch64 analogue of x86 TSS.RSP0): otherwise every user task's
// exception frames land on the dispatcher's leftover stack and corrupt it
// across tasks.  scheduler_load_kstack_top is published by switch_to_task
// on every dispatch.
.type el0_sync, @function
el0_sync:
    adrp    x0, scheduler_load_kstack_top
    ldr     x0, [x0, #:lo12:scheduler_load_kstack_top]
    cbz     x0, 2f
    mov     sp, x0
2:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, esr_el1
    lsr     x0, x0, #26        // EC = ESR bits [31:26]
    cmp     x0, #0x15          // SVC from AArch64
    b.eq    1f
    ldp     x0, x1, [sp], #16
    bl      aarch64_el0_fault_handler
    eret                       // handler adjusts ELR or parks the task
1:
    ldp     x0, x1, [sp], #16  // restore syscall nr + arg0
    b       syscall_entry

// ---------------------------------------------------------------------------
// IRQ handlers — full register save/restore with preemptive context switch
// ---------------------------------------------------------------------------

// Save area layout (36 qwords = 288 bytes):
//   sp+0:    x0
//   sp+8:    x1
//   ...
//   sp+240:  x30
//   sp+248:  SP_EL0
//   sp+256:  ELR_EL1
//   sp+264:  SPSR_EL1

.macro save_all
    sub     sp, sp, #288
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x5, sp_el0
    mrs     x6, elr_el1
    mrs     x7, spsr_el1
    stp     x5, x6, [sp, #248]
    str     x7, [sp, #264]
.endm

.macro restore_all
    ldp     x5, x6, [sp, #248]
    msr     sp_el0, x5
    msr     elr_el1, x6
    ldr     x7, [sp, #264]
    msr     spsr_el1, x7
    ldp     x1, x2, [sp, #8]
    ldp     x3, x4, [sp, #24]
    ldp     x5, x6, [sp, #40]
    ldp     x7, x8, [sp, #56]
    ldp     x9, x10, [sp, #72]
    ldp     x11, x12, [sp, #88]
    ldp     x13, x14, [sp, #104]
    ldp     x15, x16, [sp, #120]
    ldp     x17, x18, [sp, #136]
    ldp     x19, x20, [sp, #152]
    ldp     x21, x22, [sp, #168]
    ldp     x23, x24, [sp, #184]
    ldp     x25, x26, [sp, #200]
    ldp     x27, x28, [sp, #216]
    ldr     x29, [sp, #232]
    ldr     x30, [sp, #240]
    ldr     x0, [sp, #0]
    add     sp, sp, #288
.endm

.extern scheduler_save_rsp_to
.extern scheduler_load_rsp_from
.extern scheduler_load_cr3_from
.extern scheduler_on_context_switch
.extern aarch64_el0_fault_handler

.type el1_irq, @function
el1_irq:
    save_all
    bl      handle_gic_irq
    b       irq_context_switch_common

.type el0_irq, @function
el0_irq:
    // Land on the current task's own kernel stack (see el0_sync note).
    adrp    x0, scheduler_load_kstack_top
    ldr     x0, [x0, #:lo12:scheduler_load_kstack_top]
    cbz     x0, 3f
    mov     sp, x0
3:
    save_all
    bl      handle_gic_irq
    // fall through to irq_context_switch_common

// After handle_gic_irq, check for scheduler context switch
.globl irq_context_switch_common
.type irq_context_switch_common, @function
irq_context_switch_common:
    adrp    x0, scheduler_save_rsp_to
    ldr     x0, [x0, #:lo12:scheduler_save_rsp_to]
    cbz     x0, .restore

    // Save current SP to *scheduler_save_rsp_to
    mov     x1, sp
    str     x1, [x0]

    // Load new SP from scheduler_load_rsp_from
    adrp    x1, scheduler_load_rsp_from
    ldr     x1, [x1, #:lo12:scheduler_load_rsp_from]
    mov     sp, x1

    // Reset scheduler_save_rsp_to to 0
    adrp    x2, scheduler_save_rsp_to
    str     xzr, [x2, #:lo12:scheduler_save_rsp_to]

    // Update current task (no caller-saved regs to protect — 
    // all user registers are in the save area on the stack)
    bl      scheduler_on_context_switch

    // Handle CR3 (page table) switch
    adrp    x3, scheduler_load_cr3_from
    ldr     x3, [x3, #:lo12:scheduler_load_cr3_from]
    cbz     x3, .restore
    msr     ttbr0_el1, x3
    // A TTBR0 change with ASID=0 requires an invalidation of the stale
    // boot-context translations, otherwise stale identity-map entries are
    // reused for user VAs.
    dsb     sy
    tlbi    vmalle1
    dsb     sy
    adrp    x4, scheduler_load_cr3_from
    str     xzr, [x4, #:lo12:scheduler_load_cr3_from]
    isb

.restore:
    restore_all
    eret
