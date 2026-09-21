# AUDIT REPORT: pending_patch.diff (14 files, iteration 3) — issue #100 aarch64 bring-up

Patch: `audits/pending_patch.diff` — 14 files (boot.S, early_init.cpp, page_table_impl.hpp,
pci_impl.hpp, rtc.cpp, syscall_entry.S, test_aarch64.cpp, hal/context.hpp, hal/pci.hpp,
kernel.cpp, scheduler.cpp, task.cpp, test_cross_arch.cpp, libc/syscall.h).

## FINDINGS

**Iteration-2 S2 verification (init_stack by-ref):** RESOLVED. aarch64 `ArchContextManager::init_stack`
decrements the pointer 36 slots; entry at `stack_top[32]`, user SP at `[31]`, matching the
`cross_context_init_stack` scan `stack_top[0..35]` (in-bounds of the 1024-slot buffer). All aarch64
callers pass lvalues; x86/riscv signatures remain by-value.

**Iteration-2 S3 verification (stale boot.S addresses):** RESOLVED. No stale 0x4000[1-8]000 table-address
comments remain; table addresses/comments are the 0x4002xxxx family, internally consistent.

**ECAM consistency:** CONFIRMED — pci.hpp:50 comment, pci.hpp:149 aarch64 default, pci_impl.hpp:34 all
agree on 0x3f000000; riscv64 keeps 0x100000000; `#ifndef`-guarded with identical aarch64 values.

- [S3] page_table_impl.hpp — empty-table reclamation: guarded with `DESC_TABLE` so a 2 MiB block
  descriptor (bit[1]=0) can never be misinterpreted as an L3 table and freed (latent — no non-test
  callers on aarch64). Addressed post-report.
- [S3] pci_impl.hpp:39 — `PCI_ECAM_SIZE` comment now reflects the 0x3f000000 window. Addressed post-report.

**Verified-clean:** syscall ABI (x8=number @ sp+64, x0-x3 args), SPSR 0x345 (EL1h, I=0), arch guards
coherent across x86/aarch64/riscv64, aarch64_el0_fault_handler behavior unchanged, PAN clear-on-absent
targeted, PL031 at 0x09010000 inside the mapped UART 2 MiB block, kernel.cpp fallback add_region matches
signature, test doc-blocks match behavior. No spinlock-across-reschedule or TCB-lifecycle violations.

## DECISION: APPROVED