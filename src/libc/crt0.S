.section .text
.global _start

#if defined(__x86_64__)
_start:
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    lea 16(%rsp,%rdi,8), %rdx
    andq $-16, %rsp
    call main
    mov %rax, %rdi
    call exit
    hlt
#elif defined(__aarch64__)
_start:
    mov x0, sp
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
    call main
    call exit
1:  wfi
    j 1b
#endif
