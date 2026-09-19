# AUDIT REPORT 2026-09-19T09-43-44Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/aarch64/syscall_entry.S, src/kernel/arch/hal/idt.hpp, src/kernel/arch/pci.cpp, src/kernel/arch/riscv64/syscall_entry.S, src/kernel/arch/x86_64/hal/apic.hpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/irq_delivery.cpp, src/kernel/kernel.cpp, src/kernel/syscall/syscall.hpp, src/libc/syscall.h

## FINDINGS
- [S3] src/kernel/arch/pci.cpp: init_vector_alloc — allocator can hand out 0xEC/0x73 IPI vectors (comment-reserved only, not marked used), pre-existing latent overlap the new comment accurately documents but does not fix
  WHY: pci_alloc_vector skips only 0x80 + g_vector_used[] (0-47, 0x80, 0xE0, 0xFF), so SCHED_VECTOR/SHOOTDOWN_BATCH_VECTOR are allocatable to MSI devices; out of scope for this comment-only plan, hardening note only.

## CHECKS
1. Dynamic allocations: none added (comment-only lines verified: every `+` line is `//`/`///`/`#`/`;` comment; removed lines are superseded comments only).
2. Concurrency boundaries: untouched (no lock/guard/IRQ code changed).
3. Assertion masking: no test or assert touched.
4. Memory safety: no allocator/PMM/BufferPool code touched.
5. Critical section interference: zero instruction/encoding change; kernel.cpp/isr_stubs/apic changes are comments adjacent to unchanged logic.
6. Preprocessor/conditional semantics: no #if/#else/#endif or branch touched; riscv64 CONTRACT GAP comment documents the OFF_A0-vs-a7 mismatch WITHOUT altering the stub (tracked as issue #185); aarch64 x8/x0-x3, x86_64 rax/rbx/rcx/rdx/rsi regs[0..4], vector table (32/33/0x80/0xE0/0xFF), 0x71/0x72 test-only, -4096/-4095/-4097 error convention, and spec §10 versioning cites all verified true against HEAD sources, spec syscall-abi-picolibc.md, and test files.
7. Retrieval artifacts: present on issue threads #67/#68 as comments per task input (accepted, not re-fetched by read-only auditor).

DECISION: APPROVED
