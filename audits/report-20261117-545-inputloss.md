# AUDIT REPORT 20261117T131142Z
PATCH: git diff 493b74d66 -- src/services/shell.cpp src/kernel/input/serial_input.cpp src/kernel/input/serial_input.hpp docs/specs/drivers.md prompts/AGENTS-KERNEL-BRIEFING.md (uncommitted working tree on main; no audits/pending_patch.diff materialized)
FILES: src/services/shell.cpp, src/kernel/input/serial_input.cpp, src/kernel/input/serial_input.hpp, docs/specs/drivers.md, prompts/AGENTS-KERNEL-BRIEFING.md

## FINDINGS
- [S3] src/kernel/input/serial_input.hpp:85-89 — readable() doc cites the reverted "TEMP-DIAG silent-stall hunt Step 0" probe as its identity; no TEMP-DIAG code remains (verified zero hits for PROMPT-RESET/TEMP_NAPDIAG/[NAP]/[HB] in src/), so a future reader may hunt for a nonexistent probe. Provenance note is harmless; reword to plain "prompt-reset discriminator diagnostic" when next touched.
  WHY: Doc references dead hunt scaffolding by name with no in-tree referent.

## CHECKS (11 critical + 8 scoped verifications)
- C1 dynamic allocation: none — one static size_t (s_discarded_on_reset); no new/delete/malloc. PASS.
- C2 concurrency: no locks added/removed/moved; SPSC contract preserved — reset() addition is a pure peek_slice before the existing clear, valid under the unchanged QUIESCENT-ONLY contract; readable() is a pure peek (no state change). PASS.
- C3 assertion masking: no test or assertion lines touched. PASS.
- C4 memory safety: no PMM/BufferPool/object-lifecycle code touched. PASS.
- C5 critical-section interference: shell nap (Notify+wheel), pump/pop/inject, ISR paths all untouched. PASS.
- C6 preprocessor semantics: x86 boot-time serial::reset() mirrors the removed x86 prompt-time reset exactly; non-x86 coverage identical via Keyboard::flush()->serial::reset() facade at the same boot point. No asymmetric arch behavior. PASS.
- C7 retrieval artifacts: on-thread pre-call — graphify query (shell prompt/Keyboard flush/serial reset/readline chain, USED) + obsidian search (0 hits, no vault precedent, disposition recorded). In-tree docs/specs/drivers.md §10.4 used instead. PASS.
- C8 multi-arch parity: serial core + shell code shared on all three arches (compiled everywhere since #407); arch-gated lines are the pre-existing x86-only blocks moved verbatim; no page-size/MMIO/fence assumptions. PASS.
- C9 fail-closed: no hardware-discovery/DMA/allocation/IPC paths touched; push_decoded drop counting unchanged; reset() cannot fail. PASS.
- C10 capabilities: no cross-domain channels; devfs tty raw-inb path and iocd keyboard path untouched. PASS.
- C11 evidence: test-history.txt tail rows match the claimed gates (x86 hal 130/ui 87/drivers 111/core_sync 103/ipc 96/servers 66/deadline 148/selftest 157; aarch64 hal 83/ui 87/core_sync 103/ipc 96; riscv64 hal 85/ui 87/core_sync 103), plus discriminator soak 3/4 with one PROMPT-RESET discarded=1 hit and the aarch64 16/16 pre-fix signature cited. PASS.
- V1 no lock-order/BLOCKED change: nap holds no lock; producer (pump/inject) / consumer (pop/peek/consume) roles unchanged; x86 prompt loses serial reset together with the facade flush. PASS.
- V2 reset() QUIESCENT-ONLY preserved: remaining production callers are boot-only (shell pre-loop, devfs_init, Keyboard::init) + tests; dropped/overrun zeroing untouched (tests still assert it). PASS.
- V3 s_discarded_on_reset monotonic, never zeroed (single write site serial_input.cpp:99, `+=` only); overflow non-credible (≤256 bytes per rare reset on 64-bit size_t). PASS.
- V4 readable() pure peek with valid-view caveats documented in hpp:60-64. PASS.
- V5 boot-only flush genuinely quiescent (post-banner, pre-while(true), before first readline; UART pump is shell-driven so no concurrent producer); GDB-session branch untouched and still first in loop. PASS.
- V6 S1 type-ahead executes only CR/LF-terminated lines in the bounded 256 ring: readline returns true solely on '\n' (CR mapped), pos clamped to max_len-1 (Shell::BUF_SIZE=256), fast path consumes over-capacity tails. PASS.
- V7 no second UART consumer: sole production poll_getchar is pump() (serial_input.cpp:125); shell order ring->iocd->hw-ring documented and unchanged; devfs raw-inb path pre-existing and untouched. PASS.
- V8 no test files touched; no allocation; ResourceTracker delta 0 (no new resource type); shared-only cross-arch. test-history.txt append-only rows are mandatory evidence, not code. Untracked issue_512.txt / issue_547_comments.txt are harness scratch outside the patch. PASS.

DECISION: APPROVED
