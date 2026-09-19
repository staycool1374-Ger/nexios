.section .text
.globl trap_vector
.globl syscall_entry
.type trap_vector, @function
.type syscall_entry, @function

# Must be 4-byte aligned for stvec Direct mode (low 2 bits = 0)
.balign 4
.equ SAVE_SIZE, 296

trap_vector:
syscall_entry:
    # Save area: subtract SAVE_SIZE from SP
    addi sp, sp, -SAVE_SIZE

# Register save area offsets (from sp):
#   0-248: x1-x31 (GPRs)
#   256:   sepc
#   264:   sstatus
#   272:   scause
#   280:   stval
#   288:   return value (for syscalls)
.equ OFF_RA,   0
.equ OFF_SP,   8
.equ OFF_GP,   16
.equ OFF_TP,   24
.equ OFF_T0,   32
.equ OFF_T1,   40
.equ OFF_T2,   48
.equ OFF_S0,   56
.equ OFF_S1,   64
.equ OFF_A0,   72
.equ OFF_A1,   80
.equ OFF_A2,   88
.equ OFF_A3,   96
.equ OFF_A4,   104
.equ OFF_A5,   112
.equ OFF_A6,   120
.equ OFF_A7,   128
.equ OFF_S2,   136
.equ OFF_S3,   144
.equ OFF_S4,   152
.equ OFF_S5,   160
.equ OFF_S6,   168
.equ OFF_S7,   176
.equ OFF_S8,   184
.equ OFF_S9,   192
.equ OFF_S10,  200
.equ OFF_S11,  208
.equ OFF_T3,   216
.equ OFF_T4,   224
.equ OFF_T5,   232
.equ OFF_T6,   240
.equ OFF_SEPC, 248
.equ OFF_SSTAT, 256
.equ OFF_SCAUSE, 264
.equ OFF_STVAL, 272
.equ OFF_RET,  280

    # TRACE: print 'A' after save area alloc
    li a0, 65
    li a7, 1
    ecall

    # Save all GPRs (x1-x31)
    sd x1, OFF_RA(sp)
    sd x2, OFF_SP(sp)
    sd x3, OFF_GP(sp)
    sd x4, OFF_TP(sp)
    sd x5, OFF_T0(sp)
    sd x6, OFF_T1(sp)
    sd x7, OFF_T2(sp)
    sd x8, OFF_S0(sp)
    sd x9, OFF_S1(sp)
    sd x10, OFF_A0(sp)
    sd x11, OFF_A1(sp)
    sd x12, OFF_A2(sp)
    sd x13, OFF_A3(sp)
    sd x14, OFF_A4(sp)
    sd x15, OFF_A5(sp)
    sd x16, OFF_A6(sp)
    sd x17, OFF_A7(sp)
    sd x18, OFF_S2(sp)
    sd x19, OFF_S3(sp)
    sd x20, OFF_S4(sp)
    sd x21, OFF_S5(sp)
    sd x22, OFF_S6(sp)
    sd x23, OFF_S7(sp)
    sd x24, OFF_S8(sp)
    sd x25, OFF_S9(sp)
    sd x26, OFF_S10(sp)
    sd x27, OFF_S11(sp)
    sd x28, OFF_T3(sp)
    sd x29, OFF_T4(sp)
    sd x30, OFF_T5(sp)
    sd x31, OFF_T6(sp)

    # Save CSRs
    csrr t0, sepc
    sd t0, OFF_SEPC(sp)
    csrr t0, sstatus
    sd t0, OFF_SSTAT(sp)
    csrr t0, scause
    sd t0, OFF_SCAUSE(sp)
    csrr t0, stval
    sd t0, OFF_STVAL(sp)

    # TRACE: print 'B' after CSR save
    li a0, 66
    li a7, 1
    ecall

    # Increment isr_nesting_depth (matches x86_64: wrap entire handler)
    la t0, isr_nesting_depth
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)

    # TRACE: print 'C' after nesting depth inc
    li a0, 67
    li a7, 1
    ecall

    # Load scause
    ld t0, OFF_SCAUSE(sp)

    # Check if interrupt (bit 63 set — negative when signed)
    bge t0, zero, 1f

    # === Interrupt ===
    # Call handle_plic_trap(scause, sepc, sp)
    ld a0, OFF_SCAUSE(sp)
    ld a1, OFF_SEPC(sp)
    mv a2, sp
    call handle_plic_trap
    j 5f

1:  # === Exception ===
    li t1, 8                   # ECALL from U-mode
    beq t0, t1, 6f
    li t1, 9                   # ECALL from S-mode
    beq t0, t1, 6f
    # Other exceptions: output 'E' via SBI then call C handler
    li a0, 69
    li a7, 1
    ecall
    li a0, 10
    li a7, 1
    ecall
    ld a0, OFF_SEPC(sp)
    ld a1, OFF_SCAUSE(sp)
    ld a2, OFF_STVAL(sp)
    call handle_kernel_exception

6:  # Syscall (ECALL): syscall_handler(num, a0, a1, a2, a3, regs)
    # §2 CONTRACT GAP (issues #67/#68 verification, ABI v1): this reads
    # the number from OFF_A0 (user a0) and args from a1-a3, but
    # src/libc/syscall.h specifies a7=num, a0-a3=args. riscv64 has no
    # production boot path (issue #29), so this stub is unexecuted
    # scaffolding — DO NOT "fix" the mapping without a runtime check;
    # tracked separately. The TRACE ecalls above also clobber a0/a7
    # before the GPR save for the same reason.
    ld a0, OFF_A0(sp)          # syscall number from a0
    ld a1, OFF_A1(sp)          # arg1
    ld a2, OFF_A2(sp)          # arg2
    ld a3, OFF_A3(sp)          # arg3
    mv a4, sp                  # regs pointer
    call syscall_handler
    sd a0, OFF_A0(sp)         # store return value into saved a0 slot

5:  # === Return path / context switch check ===
    # Only context switch at outermost nesting (depth == 1)
    la t0, isr_nesting_depth
    ld t1, 0(t0)
    li t2, 1
    bne t1, t2, 7f

    # Check scheduler_save_rsp_to
    la t0, scheduler_save_rsp_to
    ld t0, 0(t0)
    beqz t0, 7f

    # Save current SP (pointer to save area — matches x86_64 IRET frame pointer)
    la t1, scheduler_save_rsp_to
    ld t2, 0(t1)
    sd sp, 0(t2)

    # Load new SP (points to save area of next task)
    la t2, scheduler_load_rsp_from
    ld sp, 0(t2)

    # Clear scheduler_save_rsp_to
    la t2, scheduler_save_rsp_to
    sd zero, 0(t2)

    # Call scheduler_on_context_switch()
    addi sp, sp, -16
    sd ra, 0(sp)
    call scheduler_on_context_switch
    ld ra, 0(sp)
    addi sp, sp, 16

    # Check SATP switch
    la t0, scheduler_load_cr3_from
    ld t0, 0(t0)
    beqz t0, 7f
    csrw satp, t0
    sfence.vma
    la t0, scheduler_load_cr3_from
    sd zero, 0(t0)

7:  # Issue #74: consume the TLS slot into the trap-frame TP save area.
    # Both SATP paths converge here, so the slot is consumed
    # unconditionally. A direct tp write would die at the OFF_TP restore
    # below, and no sfence is needed for tp.
    la t0, scheduler_load_tls_from
    ld t1, 0(t0)
    sd zero, 0(t0)
    beqz t1, 8f
    sd t1, OFF_TP(sp)
8:  # Decrement nesting depth
    la t0, isr_nesting_depth
    ld t1, 0(t0)
    addi t1, t1, -1
    sd t1, 0(t0)

    # Restore CSRs
    ld t0, OFF_SSTAT(sp)
    csrw sstatus, t0
    ld t0, OFF_SEPC(sp)
    csrw sepc, t0

    # Restore all GPRs (x1-x31)
    ld x1, OFF_RA(sp)
    ld x3, OFF_GP(sp)
    ld x4, OFF_TP(sp)
    ld x5, OFF_T0(sp)
    ld x6, OFF_T1(sp)
    ld x7, OFF_T2(sp)
    ld x8, OFF_S0(sp)
    ld x9, OFF_S1(sp)
    ld x10, OFF_A0(sp)
    ld x11, OFF_A1(sp)
    ld x12, OFF_A2(sp)
    ld x13, OFF_A3(sp)
    ld x14, OFF_A4(sp)
    ld x15, OFF_A5(sp)
    ld x16, OFF_A6(sp)
    ld x17, OFF_A7(sp)
    ld x18, OFF_S2(sp)
    ld x19, OFF_S3(sp)
    ld x20, OFF_S4(sp)
    ld x21, OFF_S5(sp)
    ld x22, OFF_S6(sp)
    ld x23, OFF_S7(sp)
    ld x24, OFF_S8(sp)
    ld x25, OFF_S9(sp)
    ld x26, OFF_S10(sp)
    ld x27, OFF_S11(sp)
    ld x28, OFF_T3(sp)
    ld x29, OFF_T4(sp)
    ld x30, OFF_T5(sp)
    ld x31, OFF_T6(sp)

    # Restore sp last
    addi sp, sp, SAVE_SIZE

    sret
