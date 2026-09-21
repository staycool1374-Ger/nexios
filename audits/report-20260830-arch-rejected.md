# AUDIT REPORT 2026-08-30T20-03-11Z
PATCH: audits/pending_patch.diff
FILES: prompts/STATE.md, src/kernel/arch/aarch64/boot.S, src/kernel/arch/aarch64/hal/gic.hpp, src/kernel/arch/aarch64/interrupt_controller.cpp, src/kernel/arch/aarch64/syscall_entry.S, src/kernel/arch/aarch64/timer.cpp, src/kernel/arch/aarch64/vectors.S, src/kernel/arch/hal/timer.hpp, src/kernel/elf/elf.cpp, src/kernel/irq_delivery.cpp, src/kernel/kernel.cpp, src/kernel/memory/pmm.cpp, src/kernel/memory/pmm.hpp, src/kernel/memory/vmm.cpp, src/kernel/memory/vmm.hpp, src/kernel/syscall/syscall_handlers_iommu.cpp, src/kernel/task/scheduler.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_irq.cpp, src/kernel/test/test_cap_mmio.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_pmm.cpp, src/lib/constants.hpp, test-history.txt

## FINDINGS

- [S1] src/kernel/memory/vmm.cpp:158-159 (get_table) and :573-574 (map_page_in_pml4) — aarch64 2MiB→4K block-split builds L3 leaf descriptors without PAGE_TABLE (bit 1)
  WHY: `new_table[i] = (huge_base + i*0x1000) | base_flags | PAGE_ATTR_NORMAL` — base_flags is masked from a block descriptor whose bit 1 is 0, so the leaf gets bits[1:0]=01 = block-at-invalid-level, which architecturally translation-faults on first access; the in-code comment ("bits[1:0]=11 (PAGE_TABLE set)") contradicts the expression and the pre-patch code ORed `| PAGE_TABLE` — the patch regressed the encoding at both split sites, so any split of an existing 2MiB block (kernel or user table) faults.
  Severity: S1

- [S1] src/kernel/arch/aarch64/vectors.S:90-95 (el0_sync) and :186-190 (el0_irq) — user x0 clobbered before the state save on every EL0 entry
  WHY: `adrp/ldr x0, scheduler_load_kstack_top` destroys user x0 BEFORE `stp x0, x1` (el0_sync) / `save_all` (el0_irq) capture it, so the SVC path dispatches on the kstack pointer as the syscall number, the fault path erets with corrupted x0, and every EL0 timer IRQ restores corrupted x0 — the comment "restore syscall nr + arg0" documents unmet intent; a stash-before-clobber/carry-the-pointer pattern (or dropping the pre-save switch, since irq_context_switch_common already restores SP from the task's own saved rsp before eret) is required. Aggravating pre-existing context (unchanged by this patch, but decisive for its goal): the kernel entry maps x0→syscall number (syscall_entry.S:53) while the user libc passes the number in x8 and arg0 in x0 (src/libc/syscall.h:88) — even a register-clean entry would misdispatch, which is consistent with the reported remaining "daemon-ready handshake" hang.
  Severity: S1

- [S2] src/kernel/arch/aarch64/syscall_entry.S:59-71 — deferred-switch path loses the syscall return value
  WHY: the ret is stored only in the +272 temp slot; the frame is saved with slot 0 still holding the syscall number, and the resume path (vectors.S restore_all) loads x0 from slot 0 — x86 establishes the equivalent invariant by writing the ret into the frame's rax slot BEFORE the arm check (kernel.cpp:1498 `regs[0] = syscall_handler(...)`), so every arm-and-return syscall (sys_yield, syscall_handlers_misc.cpp:63-67; irq_wait rollback, syscall_handlers_irq.cpp:141-160) resumes with the syscall number in x0 instead of the result; the patch comment claims the frame "including the syscall return value at +272" is saved "for later resumption" but nothing ever reads +272 on resume.
  Severity: S2

- [S2] src/kernel/test/test_pmm.cpp:118-121 (pmm_window_live_state) — end-page expectation is not window-base-relative
  WHY: `want_end = min(HHDM_WINDOW_SIZE/PAGE_SIZE, total_pages)` ignores window_base_page_, so on aarch64 (base=262144, end=294912) the assertion demands 294912==32768 — a guaranteed false failure on the architecture this patch brings up; it passes on x86 only because base==0 (the exact coincidence the developer's "byte-identical" note rests on).
  Severity: S2

- [S3] src/kernel/arch/aarch64/vectors.S:196-232 (irq_context_switch_common) — apply-site lacks the x86 epilogue's H2 invariants
  WHY: no generation re-check, no nesting-depth ≤2 guard, no apply-side target-liveness/ownership re-validation, no abort fixup — the x86 epilogue (isr_stubs.asm) implements all four around the same deferred-switch pair and they were added as mandatory fixes (commits 71b3a088/4bf751b4/b85ba27d); the patch newly routes blocking-syscall switches through this site while the comment claims to "mirror" isr_stubs.asm — currently unreachable on single-core masked-window bring-up, but the contract makes the invariants mandatory at every apply-site.
  Severity: S3

- [S3] src/kernel/task/scheduler.cpp:3599-3646 (aarch64_el0_fault_handler) — diagnostic page-table walk has no valid-bit checks; report-once array is unguarded shared state
  WHY: L1/L2/L3 descriptors are dereferenced without testing the valid bit, so an invalid upper entry reads `HHDM_OFFSET + 0` = PA 0, unmapped on aarch64 (RAM at 0x40000000) → nested sync exception inside the fault path; `static bool fault_reported[64]` is mutated in exception context and is only safe because exception entry masks IRQs — neither fact is documented.
  Severity: S3

- [S3] src/kernel/task/scheduler.cpp:3651 — signature and first statement merged onto one line
  WHY: `extern "C" void scheduler_on_context_switch() {    uint64_t id =` is a formatting regression (CODING_STYLE readability).
  Severity: S3

## VERIFIED (no finding)
- PMM window degenerates exactly on x86_64: RAM_BASE_FALLBACK=0, compute_window_pages(0) = [0, min(32768,total)) == old hhdm_limit; scan bounds, pop guards (free_head_ < limit ⇔ >=0 && < end), and fallback scans are index-identical; pool placement math is underflow-safe (guard `window_pages > pool+16` before subtracting; pool_end ≤ window_end by construction) and byte-identical when total ≥ 32768 pages; for tiny spans the new code fail-closes instead of placing a phantom out-of-span pool (old behavior) — a fix, not a regression.
- Multi-page scan `idx + count <= end` fixes the old unsigned underflow (`limit - count` with count > limit scanned OOB); alloc_contiguous's `count == 0 || count > total_pages_` guard (pmm.cpp:353) keeps count>window → return 0 → OOM-handler path; fail-closed.
- free_page/free_page_err ignoring index < window_base_page_: on x86 base=0 so `index < 0` is impossible — byte-identical; on aarch64 no alloc path can ever return a below-window page, so the silent ignore only neutralizes buggy callers (the old code would bitmap-clear non-RAM pages and hand them out); free_page_err explicitly returns PMM_ERR_INVALID. No leak-masking path found.
- No dynamic allocations anywhere in the patch (PMM/VMM/boot/asm paths only; the fault handler's diagnostic is static).
- Concurrency boundaries: PMM::init/init_err/try_alloc/free_page/alloc_page_table all under pmm_lock_ (IrqSpinLockGuard); scheduler window statics published with RELEASE atomics under IrqGuard (scheduler.cpp:2383-2497); aarch64_el0_fault_handler dequeues under IrqGuard then sets BLOCKED (INV-2 dequeue-before-block order respected); scheduler_load_kstack_top is a single aligned 8-byte load (tear-free) with RELEASE publication.
- vectors.S el0_sync ESR check: x1 is preserved; the x0/x1 pair save/restore is symmetric on both branches (the defect is the pre-save clobber, covered above); TTBR0 switch → dsb sy → tlbi vmalle1 → dsb sy → clear flag → isb ordering is correct; syscall frame (288 bytes) and ISR save-area layouts are identical (x0-x30 @0..240, sp_el0 @248, elr @256, spsr @264), matching the scheduler.cpp aarch64 dispatch guard's f[32]/f[33] reads.
- GIC: IGROUPR=0 (all Group 0) + CTLR/GICD enable-only bits are consistent for QEMU secure-EL1 boot; intid ≥ 1020 EOI-and-return is correct for GICv2 spurious/1020-1022 special IDs (EOIR write still retires them); GIC HHDM accessors match boot.S L2[64] mapping (VA 0xFFFF800008000000 = HHDM + PA 0x08000000).
- Timer: tick_interval_ published in set_frequency before IRQ enable, re-arm with interval==0 → 1 guard; handle_irq is IRQ-context only.
- SPSR 0x10 → 0x0 (elf.cpp ×2, task.cpp): bit 4 = M[4] selects AArch32 on eret — 0x0 = EL0t is the correct AArch64 EL0 encoding; DAIF bits stay 0 (unmasked) in both cases.
- VMM leaf attributes: PAGE_AF=bit 10 matches boot.S block descriptors (0x...421) and MAIR 0xFF00 (attr1 = Normal WB cacheable) — PAGE_ATTR_NORMAL (AttrIndx=1) is valid for RAM leaves; forcing Normal on map_page would misattribute MMIO mappings, but no aarch64 MMIO mapping routes through VMM::map_page today (GIC/UART use the boot.S Device blocks) — hardening note only.
- Test gating (test_cap_irq.cpp, test_cap_mmio.cpp): by-architecture only (PIC line-mask save/restore and TSS/IOPB are x86 mechanisms); x86 semantics unchanged (assertions still compiled on x86); expected-counts table is per-architecture with aarch64 columns 0, so the gated registrations cannot skew counts.
- irq_delivery.cpp / syscall_handlers_iommu.cpp #else arms and x86-gates: warning-fix only, no orphan call sites (both validate_iommu_pair call sites are inside CONFIG_ARCH_X86_64 blocks), no control-flow change on x86; no debug/release divergence introduced.

## PATCH
audits/rejected_patch.diff written — fixes the four blocker/major findings: restores PAGE_TABLE on both vmm.cpp aarch64 split sites; rewrites el0_sync/el0_irq entry to stash user x0/x1 before any register clobber (stash-and-carry pattern); publishes the syscall return value into the frame's x0 slot before the deferred-switch jump in syscall_entry.S; makes pmm_window_live_state's end expectation window-base-relative.

DECISION: REJECTED
