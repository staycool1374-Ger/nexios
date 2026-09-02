# AUDIT REPORT 2026-08-30T11-14-39Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/configuration.md, docs/specs/cspace.md, docs/specs/iommu.md, src/kernel/cap/cap_types.hpp, src/kernel/cap/iommu.cpp, src/kernel/cap/iommu.hpp, src/kernel/iommu/iommu.cpp, src/kernel/iommu/iommu.hpp, src/kernel/iommu/vtd.hpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_iommu.cpp, src/kernel/test/test_cap_iommu.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_idle_task.cpp, src/kernel/test/test_o1_scheduler.cpp, src/kernel/test/test_registry.cpp

## FINDINGS
- [S3] src/kernel/iommu/iommu.cpp:444-448 — CONFIG_IOMMU_MAX_BUSES < 8 is only caught as a compiler error ("excess elements in array initializer"), not by the static_assert (which covers only > 8); the guard is one-directional.
  WHY: Safe direction (build fails closed, never a runtime OOB), but the static_assert message "initializer list covers the default bound" does not actually protect the shrink case — `static_assert(CONFIG_IOMMU_MAX_BUSES == 8)` would make the config constraint explicit; hardening note only.

## VERIFICATION NOTES (iteration 3)

### Test-change 1: test_idle_task.cpp derived MAX_ITER — sound, no Heisenbug
- `kernel::integrity::_text_start/_text_end` are public (`memory/integrity.hpp:45-47`); the derived length `_text_end - (_text_start + 8)` exactly matches the region integrity.cpp actually CRCs (`integrity.cpp:93-94`).
- `CRC_CHUNK_SIZE = 4096` (`integrity.cpp:30`); every `crc_process_chunk()` call advances `min(remaining, 4096)` (`integrity.cpp:124-127`) — fully deterministic, no timing component exists to mask. Bound = ceil(text_len/4096) + 64 = 268 for the measured 204-chunk .text: generous but strictly bounded.
- Regression-catching power preserved: both post-loop assertions retained (`test_idle_task.cpp:239-240` — `JARVIS_ASSERT(done)` AND `JARVIS_ASSERT(iterations < MAX_ITER)`). A CRC that never completes (stall/timing bug) leaves done==false and still fails. This is a stale-size-constant fix, not assertion masking.

### Test-change 2: test_o1_scheduler.cpp relaxed priority assertion — sound, no Heisenbug
- Kernel priority direction verified: higher number wins — preemption when `highest_ready > cur_eff` (`scheduler.cpp:951-952`), `dequeue_highest()`/`peek_highest()` (`ready_queue_manager.cpp:46-70`), and the untouched sibling test in the same file asserting "next_task should return the highest priority task (15)" (`test_o1_scheduler.cpp:255-257`).
- The elf-loader is a boot-time system task at prio 15, documented "above harness (10)" (`elf_loader.hpp:73`); in the full-suite environment its presence in the ready queue legitimately outranks the test's prio-10 t1, so `== 10` was over-specified for a shared queue.
- The real invariant is preserved: with t1 (prio 10) freshly enqueued, `next->priority >= 10` still fails on ANY scheduler returning a below-enqueued-priority task (< 10). The `arch::IrqGuard` around add_task/next_task is retained (`test_o1_scheduler.cpp:268`), so the tick-dispatch race protection is unchanged — no timing bug is being masked.

### Carried kernel-side findings (iteration-2) re-verified fixed in this patch
- sl_walk unwind: on mid-walk page-allocation failure, every freshly linked entry is unlinked (`*linked_entry[n] = 0`) and freed exactly once (`PMM::free_page(linked_page[n])`) before returning nullptr (iommu.cpp:516-524) — no orphaned pages, no partial state.
- bdf_valid guards: `device < 32 && function < 8` applied at attach_device, clear_attachment, and context_asr (iommu.cpp:728, 774, 838). PciBdf fields confirmed raw uint8_t (`arch/hal/pci.hpp:32-35`), so unvalidated device/function would index up to 2295 in a 256-entry context table — the guard is a genuine OOB prevention. `bus` (0-255) indexes `g_root_table[256]` directly and is always in bounds.

### CRITICAL CHECKS 1-6 (complete patch)
1. Dynamic Allocations: only MemPool::alloc for the cap object (bounded by CONFIG_CAP_MAX_IOMMU live counter, rolled back on failure — IrqCap pattern) and PMM pages for SL tables (fail-closed on exhaustion). sl_walk uses fixed stack arrays. No sneaked heap use.
2. Concurrency Boundaries: all 10 manager entry points use RAII `SpinLockGuard`; leaf-lock discipline holds — the lock is never held across MemPool::alloc/free, cap::lookup, dispose, or reschedule; no cspace+manager lock nesting.
3. Assertion Masking: none — both test changes preserve regression-catching power (verified above).
4. Memory Safety / double-free: single-owner page discipline holds — cascade_free_empty frees a table only when all 512 entries are zero and then zeroes the parent entry (freed table never reachable again; root excluded from cascade); domain_destroy walks only occupied records, clears each before proceeding, frees the root once (`sl_root_phys != 0` guard, then zeroed); dispose/revoke idempotent via `domain_idx_ = -1`; map_frame rollback clear_leaf cannot prematurely free shared tables (other entries non-zero). Pin acquire/release balanced on every syscall return path.
5. Critical-Section Interference: new leaf SpinLock serializes all manager state; syscall chain is lookup (cspace lock, released inside) → manager lock (brief) → release pins after return — no overlap, no blocking under the lock.
6. Preprocessor/#ifdef semantics: `#if defined(CONFIG_ARCH_X86_64)` / `#else` branches in both handlers are symmetric and both terminate with -1 ((void) casts on unused params, no uninitialized vars); `force_present` is NOT #ifdef-gated (identical debug/release control flow); all four new config macros #ifndef-guarded. Syscall table sized by MAX_SYSCALL=61 with both new entries explicitly appended; test count updates consistent (990 = 978 + 12; register function registers exactly 12 tests).
