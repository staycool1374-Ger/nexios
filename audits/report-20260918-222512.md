# SIL 3 Audit Report — NexIOS issue #184 bring-up patch

PATCH: audits/pending_patch.diff (+144/-0, 7 files)
FILES:
- linker/linker_riscv64.ld (marker KEEP placements, +8/-0 context)
- src/kernel/arch/aarch64/hal/page_table_impl.hpp (tlb_purge_context fallback)
- src/kernel/arch/aarch64/interrupt_controller.cpp (aarch64_el0_fault_handler diagnostic park)
- src/kernel/arch/aarch64/test_stubs.cpp (register_apic_tpr_tests, register_pt_merge_tests stubs)
- src/kernel/arch/riscv64/hal/page_table_impl.hpp (tlb_purge_context fallback)
- src/kernel/arch/riscv64/test_stubs.cpp (register_apic_tpr_tests, register_pt_merge_tests stubs)
- src/lib/compiler_rt.cpp (new, riscv64-gated __clzdi2/__ctzdi2 restore)

SCOPE: diff-only audit per tasking. Read audits/pending_patch.diff in full first.
Ambiguity resolution only (no builds/tests): `git show HEAD:<path>` + targeted
`rg` reference checks for #ifdef symmetry / namespace / call-site resolution.

## FINDINGS

No S1 findings.
No S2 findings.
No S3 findings.

WHY (check-by-check, with file:line):

- Allocations (none): no new/delete/malloc/free in patch. New state is
  three zero-initialized volatile uint64_t latches
  (src/kernel/arch/aarch64/interrupt_controller.cpp:96-98) + pure-compute
  libcalls (src/lib/compiler_rt.cpp:228-250) + inline TLB wrappers
  (aarch64/hal/page_table_impl.hpp:78-80, riscv64/hal/page_table_impl.hpp:160-162)
  + linker KEEP directives + Logger::info stubs. No heap path.
- Concurrency / IRQ-contract: aarch64_el0_fault_handler
  (src/kernel/arch/aarch64/interrupt_controller.cpp:108-121) runs with
  interrupts masked (vector entry), touches only its own latched ESR/FAR/ELR
  statics, then parks via arch::pause() (defined: src/kernel/arch/aarch64/hal/io_impl.hpp:112).
  No IRQ re-enable, no scheduler touch, no return (infinite park) — matches
  vectors.S contract (src/kernel/arch/aarch64/vectors.S:106 `bl aarch64_el0_fault_handler`
  followed by unreachable `eret`; handler documents no-resume). Single-writer
  latch, GDB-observable, no lock needed. tlb_purge_context fallbacks are
  lock-free single privileged invalidate + existing barriers
  (arch_page_table_tlb_flush_all), local-CPU only and documented superset of
  single-context purge, mirroring x86_64 CR3-reload fallback — same class as
  x86 (src/kernel/arch/x86_64/hal/page_table_impl.hpp:91). Safe under the
  tlb_shootdown/elf/test_invpcid call sites.
- Assertion masking (none — no tests touched): patch adds only stubs/fallbacks/
  handler/linker/compiler-rt; no assert/expect/check modifications, no
  test-threshold weakening.
- Double-free / memory safety: no ownership transfer, no free path, no pointer
  arithmetic on the new code except linker symbol placement. compiler_rt loops
  are bounded shifts/masks on by-value ulongs with early zero return; no OOB.
  Linker KEEP(*(.marker.*)) matches empty-unless-emitted sections — benign when
  absent (standard orphan/KEEP idiom), no VMA overlap change.
- Critical-section interference: no lock acquire/release, no IRQ mask
  save/restore alteration, no ready-queue/scheduler interaction, no PMM/MemPool
  accounting change. GIC paths untouched (only appended code after `ack:` block).
- #ifdef symmetry (verified): `__clzdi2/__ctzdi2` fully inside
  `#if defined(CONFIG_ARCH_RISCV64)` (src/lib/compiler_rt.cpp:224-254); rg
  shows zero C++ TU references outside that file (only its own defs/comments),
  so x86_64/aarch64 TUs lose nothing — consistent with libcall-only use and
  the stated inline-lowering on x86/AArch64. New tlb_purge_context defs sit
  inside `namespace arch` (aarch64 between flush_all:55 and class:62;
  riscv64 between :64 and :70; cf. x86:91 inside namespace arch:30-172),
  resolving existing callers arch::tlb_purge_context (elf.cpp:780,
  tlb_shootdown.cpp:81, test_invpcid.cpp:124). Handler symbol resolves
  vectors.S:106/180 (verified HEAD has call site awaiting this def).
  New stubs sit inside `#if !defined(CONFIG_ARCH_X86_64)` before the
  existing `#endif` (aarch64/test_stubs.cpp:145, riscv64/test_stubs.cpp:188),
  complementing declarations in test_registry.cpp:87,96 with no x86 duplicate
  (x86 reals in test_apic_tpr.cpp:273, test_pt_merge.cpp:305).
- Retrieval artifacts (as tasked): GitHub #184 thread holds graphify query +
  vault zero-match + hypothesis checkpoints; nm baseline evidence cited for
  the riscv64 libcall gap and zero-undefined-refs claim on x86/aarch64.
  Taken as provided context under diff-only constraint (no builds/tests run).

## VERIFICATION (read-only)

- `git show HEAD:src/kernel/arch/aarch64/interrupt_controller.cpp` — confirms
  `namespace arch` open and `} // namespace arch` close; patch appends inside it.
- `git show HEAD:src/kernel/arch/{aarch64,riscv64}/hal/page_table_impl.hpp` —
  confirms insertion point inside `namespace arch`.
- `git show HEAD:src/kernel/arch/x86_64/hal/page_table_impl.hpp` — confirms
  x86 precedent inside same namespace.
- `rg tlb_purge_context / aarch64_el0_fault_handler / __clzdi2|__ctzdi2 /
  register_apic_tpr_tests|register_pt_merge_tests` — reference/symmetry checks above.
- `git show HEAD:src/kernel/arch/aarch64/vectors.S` — confirms EL0 non-SVC
  branch targets the new handler.
- `rg "pause" src/kernel/arch/aarch64` — confirms arch::pause availability.

DECISION: APPROVED
