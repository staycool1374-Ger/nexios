.section .text
.globl syscall_entry
.type syscall_entry, @function
.extern scheduler_save_rsp_to
.extern irq_context_switch_common
.extern debug_write
.extern debug_write_hex

// AArch64 syscall entry via SVC #0 from EL0
// On entry:
//   x0 = syscall number
//   x1-x4 = arguments
//   SP_EL1 = kernel stack, SP_EL0 = user stack
//   ELR_EL1 = return address (user PC)
//   SPSR_EL1 = saved processor state
//
// Save area layout (36 qwords = 288 bytes):
//   sp+0:    x0
//   sp+8:    x1
//   ...
//   sp+240:  x30
//   sp+248:  SP_EL0
//   sp+256:  ELR_EL1
//   sp+264:  SPSR_EL1
//   sp+272:  (temp for return value)
syscall_entry:
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

    // syscall_handler(number, arg0, arg1, arg2, arg3, regs)
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldr     x4, [sp, #32]
    mov     x5, sp
    bl      syscall_handler

    str     x0, [sp, #272]
    // The deferred-switch resume path (vectors.S restore_all) loads x0
    // from the frame's slot 0 — the aarch64 analogue of the x86 iret-frame
    // rax slot filled by handle_interrupt_c (regs[0] = syscall_handler).
    // Publish the return value there before applying the switch, or the
    // resumed task erets with the syscall number in x0 instead of the
    // result.  The no-switch epilogue overwrites x0 from +272 unchanged.
    str     x0, [sp, #0]

    // A blocking syscall (IPC receive/send_sync, IRQ wait, exit, ...) arms
    // the deferred switch (scheduler_save_rsp_to) before returning.  If the
    // arm is set, do NOT return into the (now BLOCKED) task — apply the
    // switch instead: the new task's context is restored and this task's
    // frame (including the syscall return value at +272) is saved into its
    // own context for later resumption.  Mirrors the x86_64 isr_stubs
    // syscall epilogue (isr_stubs.asm).
    adrp    x0, scheduler_save_rsp_to
    ldr     x0, [x0, #:lo12:scheduler_save_rsp_to]
    cbz     x0, .sys_no_switch
    b       irq_context_switch_common

.sys_no_switch:
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

    ldr     x0, [sp, #272]

    add     sp, sp, #288
    eret
