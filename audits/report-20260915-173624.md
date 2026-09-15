# AUDIT REPORT 20260915-173624
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/x86_64/hal/io_impl.hpp, src/kernel/arch/x86_64/hal/page_table_impl.hpp, src/kernel/arch/x86_64/hal/pcid.cpp, src/kernel/elf/elf.cpp, src/kernel/ipc/buffer_pool.cpp, src/kernel/memory/vmm.cpp, src/kernel/test/test_invpcid.cpp, test-history.txt

## FINDINGS
- [S3] src/kernel/test/test_invpcid.cpp:337 — duplicate `#include <kernel/arch/x86_64/hal/pcid.hpp>` (lines 336-337)
  WHY: Harmless under `#pragma once`; style-only, no semantic effect.
- [S3] src/kernel/arch/x86_64/hal/page_table_impl.hpp:93 — `cr3 & ~0xFFFULL` clears PWT/PCD as well as PCID on CR3-reload flush paths
  WHY: Matches the established pre-existing in-tree idiom (pcid.cpp rollover) and PWT/PCD are 0 in practice, so no functional impact.
- [S3] src/kernel/test/test_invpcid.cpp:399 — sibling re-touch writes via the HHDM alias, not the tested k_va_b mapping
  WHY: Proves page liveness, not TLB-entry intactness; mapping intactness is separately covered by the walk assertion, so the comment slightly overclaims with no safety impact.

DECISION: APPROVED
