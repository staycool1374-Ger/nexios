# AUDIT REPORT 2026-09-14T18-56-16Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/elf/elf_shared.hpp, src/kernel/elf/elf_shared.cpp, src/kernel/elf/elf.hpp, src/kernel/elf/elf_loader.hpp, src/kernel/elf/elf_loader.cpp, src/kernel/memory/vmm.hpp, src/kernel/memory/vmm.cpp, src/kernel/task/task.hpp, src/kernel/task/task.cpp, src/kernel/test/test_elf_shared.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_expected_counts.hpp, docs/specs/elf-shared-libs.md, test-history.txt

## FINDINGS
- [S1] elf_shared.cpp:map_hit — temp image buffer freed, then reused for parsing/reloc AND recorded into ctx for a second free (UAF + double-free on every cache-hit path; silent only because freed pages went untouched in-window)
  WHY: free_image_pages ran unconditionally before the reloc block while image_views/image_phys kept pointing at the buffer.
  STATUS: fixed pre-report — free runs only on error exits; success retains via the recorded entry.
- [S1] elf_shared.cpp:map_hit reloc-fail — acquired/images entries left registered while the refcount was dropped, so the loader cleanup released twice (second release steals another owner's ref when the slot survives)
  WHY: Registration must be atomic with success; a bare error return leaves half-registered state the caller cannot distinguish.
  STATUS: fixed pre-report — entries popped (they are provably last), buffer freed, then release.
- [S2] shared-RO double-free via blind teardown — per-pml4 free_user_pages cannot distinguish cache-owned shared phys from task-owned pages; two tasks sharing lib TEXT (or teardown ordering) frees twice
  WHY: Lifetime split across two owners (cache refcount vs pml4 walk) with no protocol between them.
  STATUS: fixed pre-report — hard rule implemented everywhere: unmap shared RO first (new VMM::unmap_page_in_pml4, 3-arch), then release (cache frees RO phys at refcount 1→0), then free_user_pages; TCB::cleanup + destroy_completed_tcb + loader cleanup_and_idle + tests all follow it; release() hardened with bounds clamps.
- [S3] SharedLibCache::instance() Meyers singleton under -fno-threadsafe-statics — first-call race on SMP
  WHY: Same exposure as the tree's DmesgService/KlogService singletons; first use is loader/test task-context.
  STATUS: accepted (tree-consistent), no change.
- [S3] Shell rm of /lib mid-load races file-block lifetime (tmpfs ignores vnode refcounts, so fd-pinning would be theater; the existing Stage-A fd path shares the exposure exactly)
  WHY: Fixing only Stage B would claim safety the VFS layer does not provide; a real fix is fd-pinned reads for the whole loader (future work).
  STATUS: documented limitation (spec §7 notes initrd/test scope), no change.
- [S3] Reloc loop has no reschedule points (large libs bind slowly but boundedly; ticks preempt the prio-15 loader so no starvation, only load latency)
  WHY: Eager binding is a load-time cost by design (spec non-goal: no lazy binding for WCET).
  STATUS: accepted, no change.
- [S3] Planner STEP-15 (idt.cpp reservations) confirmed not applicable — no IDT change exists or is needed (dynamic vectors live in the existing claim window)
  WHY: Verified, no vector table work in this issue.
  STATUS: accepted, no change.

Positive checks: zero heap allocation (PMM pages + fixed arrays only, every buffer with exactly one free site per path — verified path-by-path); no locks across reschedule (cache lock_ held only for slot scans); no test assertion weakened (9 real tests, all behavioral); snapshot layout untouched; preprocessor symmetry (new tests x86-gated, counts {9,0,0}; unmap implemented per-arch — x86_64/aarch64/riscv64 builds green); check-7 retrieval artifacts on #95 (graphify 222-node query + vault 0-match in the plan-verdict comment); EOI/lock ordering untouched; TCB field init covered by all 4 memset sites + ctor list (reorder-checked).

DECISION: APPROVED
