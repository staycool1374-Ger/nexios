# AUDIT REPORT 20260915-165738
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/x86_64/hal/cpuid_impl.hpp, src/kernel/arch/x86_64/hal/io_impl.hpp, src/kernel/arch/x86_64/hal/pcid.cpp, src/kernel/arch/x86_64/hal/pcid.hpp, src/kernel/arch/x86_64/hal/smp.cpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/elf/elf.cpp, src/kernel/kernel.cpp, src/kernel/nexios_config.h, src/kernel/task/scheduler.cpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_pcid.cpp, test-history.txt

## FINDINGS
- [S1] src/kernel/arch/x86_64/hal/pcid.cpp:101 — pcid_free() recycles a PCID with no TLB invalidation
  WHY: Freed address-space entries tagged with the ID survive (tagged switches never flush and VMM::free_user_pages explicitly leaves flushing to the caller), so the next task reissued the ID can take stale-TLB hits to freed frames — an address-space isolation breach; verified the sibling paths are safe (exec_into_current does an immediate untagged write_cr3 = full local flush; clone uses a fresh ID; INVLPG covers single-page unmaps, but bulk teardown has no flush).
- [S3] src/kernel/nexios_config.h:389 — CONFIG_PCID_MAX (4096) defined but never referenced; allocator uses arch::PCID_MAX (4095)
  WHY: Dead macro whose name/value suggests an invalid 12-bit max (4096) invites an off-by-one at a future call site — delete it or wire the allocator to it.
- [S3] src/kernel/arch/x86_64/hal/pcid.cpp:88 — g_next/g_epoch/g_supported are plain globals mutated on birth/cleanup/rollover paths with no proven common lock
  WHY: Concurrent creates on two CPUs race the cursor/epoch (C++ data race); benign in practice because the CAS bitmap alone enforces never-reissue and epoch is diagnostic-only — make them atomic or document the serialization.
- [S3] src/kernel/arch/x86_64/hal/pcid.cpp:119 — pcid_snapshot_save/restore covers g_next/g_epoch but not the 64-word g_live bitmap
  WHY: A test aborting mid-force leaks live bits across tests (isolate resets only the support flag); pass-paths free everything so no gate impact — snapshot the bitmap or assert it empty on restore.

## PATCH
audits/rejected_patch.diff was written: it adds a one-line same-root CR3 reload (global local flush) in pcid_free() after the bitmap clear, restoring the pre-PCID flush-on-teardown guarantee for recycled IDs.

DECISION: REJECTED
