# AUDIT REPORT 2026-09-01T163155Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/arch/hal/iopb.hpp, src/kernel/arch/x86_64/hal/iopb.cpp, src/kernel/cap/mmio.cpp, src/kernel/cap/mmio.hpp, src/kernel/elf/elf.cpp, src/kernel/memory/vmm.cpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_mmio.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_mmio.cpp, src/kernel/test/test_cap_mmio_user.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## FINDINGS

iter-5 S2 double-release fix verified CORRECT (no S1/S2). Full re-check of all CRITICAL CHECKS:

1. **Dynamic Allocations (RT paths):** none. `MmioUserMap::s_slots_` is a static bounded array; the IOPB ledger is a static bounded array; `snapshot_reset` uses a fixed `cap::MmioCap *to_release[kMaxMaps]` stack array; VMM map/unmap allocate page-table pages via PMM only outside the map lock (map syscall path, not ISR). PASS.

2. **Concurrency boundaries:** `s_lock_` and `g_iopb_lock` are RAII `SpinLockGuard`-wrapped. VMM map/unmap and every `release()` that may dispose run OUTSIDE the lock scopes (map() unmap() invalidate_cap() drain_task() snapshot_reset()). dispose-triggered reentrancy (dispose -> invalidate_cap -> s_lock_) is safe because pins are released outside the lock. Lock order scheduler_lock_ -> g_iopb_lock -> mmio-map-lock is respected; no lock is held across reschedule. PASS.

3. **iter-5 re-validation (double-release):** both `unmap` and the map-failure rollback re-validate the slot at the clear step (`occupied && mmio==cap && va==va [&& owner/gen for unmap]`) under `s_lock_`, and release the slot pin ONLY on the observed occupied true->false transition. Because every clear+release is serialized under `s_lock_` with a check-and-set on `occupied`, exactly one path observes the true->false transition; a concurrent clearer already released the pin. No double-release, no leak (acquire/release balanced in every interleaving). PASS.

4. **Memory safety / UAF:** 
   - unmap/map-failure deref `mmio->size` in `unmap_mmio_from_cap`/`map_mmio_from_cap` is protected by the slot pin; in the concurrent-revoke race the cap stays alive because `cap::revoke` holds the CNode slot reference across `target->revoke()` (-> invalidate_cap) until after it returns (cap.cpp:218-223), and dispose-driven invalidate_cap cannot race an in-flight unmap (dispose fires only at refcount 0, which implies no live slot pin). PASS.
   - cleanup drain runs before `free_user_pages`/`free_page` and before `release_all_objects` (task.cpp:1847 vs 1855-56, 1886) — no write into freed page-table memory and the cap is alive for the unmap deref. PASS.
   - exec drain runs on the OLD PML4 before swap (elf.cpp) — stored slot pml4 never dangles. PASS.
   - fork deep-copy skips non-RAM frames via `!PMM::is_user_page(src_data)` at the x86_64/aarch64 PT-leaf and both riscv64 leaf sites (l2,l3) — child never inherits a device mapping and never memcpys HHDM+device_phys. PASS.
   - IOPB ledger stores `cap_ptr` equality-only, never dereferenced; `iopb_ledger_clear_cap` guards `owner->iopb_slot_ != IOPB_SLOT_NONE` before touching the pool and is serialized with drop_task under g_iopb_lock. PASS.

5. **Critical section interference:** ENSURE relaxation in map_page/map_page_in_pml4 scoped by `phys_addr < PMM::total_memory()` — kernel pages (< total RAM) still enforce `is_user_page`; only device frames (>= total RAM) skip it, which is the correct semantics for legitimate user MMIO mappings, not a Heisenbug mask. VA-window static_asserts hold (base 0x61000000, end 0x62000000 < STACK_VADDR 0x70000000; base >= HEAP_VADDR+HEAP_SIZE 0x60100000). TSS reload on retroactive revoke mirrors the existing grant path. PASS.

6. **Preprocessor / conditional semantics:** x86_64 IOPB ledger (with arch-neutral no-op stubs for non-x86_64) and the arch-neutral MmioUserMap are both consistent; ledger tests guarded by CONFIG_ARCH_X86_64; riscv64 fork deep-copy sites both guarded; debug/release divergence none (no CONFIG_DEBUG-gated code in the patch). PASS.

- [S3] src/kernel/syscall/syscall_handlers_mmio.cpp:79-88 — `iopb_ledger_add` runs BEFORE `iopb_grant_range`; if grant_range ever failed after a successful claim (currently unreachable for the running task, which cannot be concurrently cleaned up), the ledger entry would leak (a phantom record consuming a ledger slot). Hardening: drop the ledger entry when grant_range fails.
  WHY: defensive only — the current-task slot cannot become unclaimed between claim and grant, so the leak path is not reachable today, but the ordering leaves no rollback.

- [S3] src/kernel/cap/mmio.cpp:390-435 — `unmap` returns `true` even when the re-validation finds the slot already cleared by a concurrent invalidate_cap (i.e. it reports success without having performed the clear). Not a safety defect (the mapping is gone either way), but the return value no longer reflects "this call cleared it".
  WHY: semantic only; a concurrent cross-task revoke already removed the mapping, so returning true is harmless.

- [S3] src/kernel/memory/vmm.cpp:256/350/534/601 — the ENSURE relaxation uses `phys_addr < PMM::total_memory()`; a device BAR placed below total RAM but not user-allocated would still trip the ENSURE on the user-map path. Unrealistic for x86_64 (PCI MMIO hole is above RAM) and kernel pages remain guarded.
  WHY: hardening note; no safety regression in practice.

## PATCH
(APPROVED — no rejected_patch.diff written.)

DECISION: APPROVED
