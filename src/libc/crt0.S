/* NexIOS RTOS — Userspace entry (crt0).
 *
 * 6-step entry contract v0.1 (issue #171, spec §13): (1) raw ABI_VERSION
 * query (needs no TLS), (2) contract check vs the ABI major this SDK
 * targets (mismatch ⇒ fail-closed SYS_EXIT + serial diagnostic), (3)
 * TLS_SET to the picolibc thread block (picolibc owns the TLS-block layout;
 * crt0 only points the thread register at it — TLS_SET itself needs no
 * live TLS), (4) launch parse (argc/argv/envp from the loader-built stack;
 * the versioned launch struct is reserved-absent in v0.1, P4), (5) main,
 * (6) clean SYS_EXIT on return.
 *
 * Contract notes (binding): tls_base_ == 0 means unset — the kernel loads
 * nothing; TLS comes only from step 3. crt0.o never maps kernel memory
 * (audit gate — only its own .rodata is read). The fail-closed exit code
 * is 0xC0DE (CRT0 contract mismatch).
 *
 * Mismatch design: v0.1 compares the kernel ABI major from the step-1
 * query against NEXIOS_ABI_MAJOR (syscall.h) — any differ (newer or older
 * kernel) fails closed. SDK-major/contract equality is structural in v0.1
 * (single SDK, nothing to mix at link); runtime comparison against
 * launch-provided values activates with the launch struct (P4) without
 * reshaping this branch.
 *
 * Diagnostic design: the mismatch message goes out via best-effort
 * SYS_WRITE to stderr (fd 2, wired to /dev/tty by the loader). Direct
 * UART/port I/O from Ring 3 would fault (x86 needs an IOPB grant, issue
 * #8; aarch64/riscv UART MMIO is not user-mapped), so the kernel writes it
 * to serial for us. The write result is ignored — SYS_EXIT below is the
 * guarantee, the message is best-effort.
 */
#include "sdk_version.h"

.section .text
.global _start

#if defined(__x86_64__)
_start:
    /* Step 1: raw ABI_VERSION query (84, syscall.h). No TLS needed.
       Unused arg regs are zeroed, never left as garbage: §2 passes args
       by register, and a garbage 5th-arg register faulted the aarch64
       exit trap (issue #171) — the same luck must not be relied on here. */
    mov $84, %rax
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    int $0x80
    /* Step 2: contract check — major(query) must equal NEXIOS_ABI_MAJOR
       (syscall.h:304, currently 1). Full-matrix note (spec §14): 0.x
       requires exact major match; additive minors keep running. */
    shr $16, %rax
    cmp $1, %rax
    jne .Lfail
    /* Step 3: TLS_SET (85) to the picolibc thread block. Issue #338:
       publish the thread-local block (errno at TP+0) before main. */
    mov $85, %rax
    lea nexios_tls_block(%rip), %rbx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    int $0x80
    /* Step 4: launch parse — argc/argv/envp from the loader-built stack
       (argc at (%rsp), argv at 8(%rsp), envp after the argv NULL). */
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    lea 16(%rsp,%rdi,8), %rdx
    andq $-16, %rsp
    /* Step 5: driver main. */
    call main
    /* Step 6: clean SYS_EXIT (6) with main's return code. */
    mov %rax, %rbx
    mov $6, %rax
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    int $0x80
    hlt
.Lfail:
    /* Best-effort diagnostic + fail-closed exit. See header comment. */
    mov $13, %rax             /* SYS_WRITE (syscall.h) */
    mov $2, %rbx              /* stderr */
    lea Lcrt0_nomsg(%rip), %rcx
    mov Lcrt0_nomsgl(%rip), %rdx
    xor %esi, %esi
    xor %edi, %edi
    int $0x80
    mov $6, %rax              /* SYS_EXIT */
    mov $0xC0DE, %rbx         /* CRT0 contract-mismatch exit code */
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    int $0x80
    hlt
#elif defined(__aarch64__)
_start:
    /* Step 1: raw ABI_VERSION query (84). No TLS needed. Unused arg
       regs are zeroed, never left as garbage: §2 passes args by
       register (x8=num, x0-x3=args, x4 5th), and a garbage 5th-arg
       register faulted this file's exit trap (issue #171). */
    mov x8, #84
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    svc #0
    /* Step 2: contract check — major(query) must equal NEXIOS_ABI_MAJOR
       (syscall.h:304, currently 1). */
    lsr x0, x0, #16
    cmp x0, #1
    b.ne .Lfail
    /* Step 3: TLS_SET (85) to the picolibc thread block (errno at TP+0). */
    mov x8, #85
    adrp x0, nexios_tls_block
    add x0, x0, :lo12:nexios_tls_block
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    svc #0
    /* Step 4: launch parse — argc/argv/envp from the loader-built stack.
       (Pre-v0.1 passed sp alone in x0; v0.1 parses for launch parity.) */
    ldr x0, [sp]
    add x1, sp, #8
    lsl x2, x0, #3
    add x2, x1, x2
    add x2, x2, #8
    /* Step 5: driver main. */
    bl main
    /* Step 6: clean SYS_EXIT (6) with main's return code. */
    mov x8, #6
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    svc #0
1:  wfe
    b 1b
.Lfail:
    mov x8, #13               /* SYS_WRITE */
    mov x0, #2                /* stderr */
    adrp x1, Lcrt0_nomsg
    add x1, x1, :lo12:Lcrt0_nomsg
    adrp x2, Lcrt0_nomsgl
    ldr x2, [x2, :lo12:Lcrt0_nomsgl]
    mov x3, #0
    mov x4, #0
    svc #0
    mov x8, #6                /* SYS_EXIT */
    mov x0, #0xC0DE           /* CRT0 contract-mismatch exit code */
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    svc #0
2:  wfe
    b 2b
#elif defined(__riscv)
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    /* Step 1: raw ABI_VERSION query (84). No TLS needed. Unused arg
       regs are zeroed, never left as garbage: §2 passes args by
       register (a7=num, a0-a3=args, a4 5th); garbage arg regs faulted
       the aarch64 exit trap (issue #171). */
    li a7, 84
    li a0, 0
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    ecall
    /* Step 2: contract check — major(query) must equal NEXIOS_ABI_MAJOR
       (syscall.h:304, currently 1). */
    srli a0, a0, 16
    li t0, 1
    bne a0, t0, .Lfail
    /* Step 3: TLS_SET (85) to the picolibc thread block (errno at TP+0). */
    li a7, 85
    la a0, nexios_tls_block
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    ecall
    /* Step 4: launch parse — argc/argv/envp from the loader-built stack.
       (Pre-v0.1 passed sp alone in a0; v0.1 parses for launch parity.) */
    ld a0, 0(sp)
    addi a1, sp, 8
    slli t0, a0, 3
    add a2, a1, t0
    addi a2, a2, 8
    /* Step 5: driver main. */
    call main
    /* Step 6: clean SYS_EXIT (6) with main's return code (already in a0). */
    li a7, 6
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    ecall
1:  wfi
    j 1b
.Lfail:
    li a7, 13                /* SYS_WRITE */
    li a0, 2                 /* stderr */
    la a1, Lcrt0_nomsg
    la t0, Lcrt0_nomsgl
    ld a2, 0(t0)
    li a3, 0
    li a4, 0
    ecall
    li a7, 6                 /* SYS_EXIT */
    li a0, 0xC0DE            /* CRT0 contract-mismatch exit code */
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    ecall
2:  wfi
    j 2b
#endif

    /* Shared fail-closed diagnostic (user-half .rodata, always mapped). */
    .section .rodata
Lcrt0_nomsg:
    .ascii "crt0: ABI contract mismatch, exiting\n"
Lcrt0_nomsg_end:
    /* Length quad needs 8-alignment for aarch64 ldr/riscv ld; the end
       label above keeps padding out of the length. */
    .balign 8
Lcrt0_nomsgl:
    .quad Lcrt0_nomsg_end - Lcrt0_nomsg

    /* Issue #338: per-task thread-local block (struct nexios_tcb layout
       in errno.h — errno int at TP+0). Zero-init .bss: fresh tasks
       observe errno == 0; fork copies it with the address space. */
    .section .bss
    .align 16
    .global nexios_tls_block
nexios_tls_block:
    .zero 16

    /* Spec §14: SDK identity embedded in every driver image (readelf/
       objdump visible; the loader stays dumb). Values are the build-time
       SDK constants from sdk_version.h. Non-alloc: never loaded. */
    .section .nexios_sdk,"",@progbits
    .balign 8
    .quad NEXIOS_SDK_MAJOR
    .quad NEXIOS_SDK_MINOR
    .quad NEXIOS_CAP_API_MAJOR
    .quad NEXIOS_CAP_API_MINOR
    .quad NEXIOS_CRT0_CONTRACT
