# AUDIT REPORT 20260915-191200
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/x86_64/hal/cpuid_impl.hpp, src/kernel/arch/x86_64/hal/io_impl.hpp, src/kernel/arch/x86_64/hal/pcid.cpp, src/kernel/arch/x86_64/hal/pcid.hpp, src/kernel/arch/x86_64/hal/smp.cpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/elf/elf.cpp, src/kernel/kernel.cpp, src/kernel/nexios_config.h, src/kernel/task/scheduler.cpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_pcid.cpp, test-history.txt

## FINDINGS
- [S3] src/kernel/arch/x86_64/hal/pcid.cpp:183 — pcid_free() local-only flush is correct only under the CPU0-pin invariant (documented in-code); any future off-CPU0 user-teardown path must route through the #158 lazy-shootdown protocol
  WHY: Verified sound for the current tree (user execution CPU0-only via scheduler.cpp:359-370 enqueue clamp + dispatch-only ap_tick + hlt-only AP idle; user cleanup CPU0-only via BSP on_tick reap, BSP idle cleanup_step, user-parent waitpid, BSP reboot; kernel children carry PCID 0 so AP waitpid on them is a no-op), so the residual is a recorded assumption, not a defect.

DECISION: APPROVED
