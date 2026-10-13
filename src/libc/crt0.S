.section .text
.global _start

#if defined(__x86_64__)
_start:
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    lea 16(%rsp,%rdi,8), %rdx
    andq $-16, %rsp
    // Issue #338: publish the thread-local block (errno at TP+0) before
    // main. TLS_SET = 85, address in rbx. main args (rdi/rsi/rdx) are
    // preserved across the trap via push/pop (no call between).
    push %rdi
    push %rsi
    push %rdx
    mov $85, %rax
    lea nexios_tls_block(%rip), %rbx
    int $0x80
    pop %rdx
    pop %rsi
    pop %rdi
    call main
    mov %rax, %rdi
    call exit
    hlt
#elif defined(__aarch64__)
_start:
    mov x0, sp
    // Issue #338: publish the thread-local block before main. x0 (sp)
    // is stashed on the aligned stack across the svc.
    str x0, [sp, #-16]!
    mov x8, #85
    adrp x0, nexios_tls_block
    add x0, x0, :lo12:nexios_tls_block
    svc #0
    ldr x0, [sp], #16
    bl main
    bl exit
1:  wfe
    b 1b
#elif defined(__riscv)
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    mv a0, sp
    // Issue #338: publish the thread-local block before main. a0 (sp)
    // is stashed on the aligned stack across the ecall.
    addi sp, sp, -16
    sd a0, 0(sp)
    li a7, 85
    la a0, nexios_tls_block
    ecall
    ld a0, 0(sp)
    addi sp, sp, 16
    call main
    call exit
1:  wfi
    j 1b
#endif

    // Issue #338: per-task thread-local block (struct nexios_tcb layout
    // in errno.h — errno int at offset 0). Zero-init .bss: fresh tasks
    // observe errno == 0; fork copies it with the address space.
    .section .bss
    .align 16
    .global nexios_tls_block
nexios_tls_block:
    .zero 16
