# AUDIT REPORT 20260906T135246Z
PATCH: audits/pending_patch.diff
FILES: Makefile, src/kernel/gcov/gcov_handler.cpp, src/kernel/gcov/gcov_handler.hpp,
src/kernel/nexios_config.h, src/kernel/task/task.cpp, src/kernel/test/test_isolate.cpp,
src/lib/test.cpp, tools/coverage_report.py
ITERATION: 2 (previous: audits/report-20260906T133007Z.md — DECISION: REJECTED)

## FINDINGS

- [S3] src/lib/test.cpp:499 — patch does not contain the declaration of the new API it calls: `kernel::test::profiling_dump_samples()` is declared only in the *worktree* copy of `src/lib/test.hpp:170-176` (`#ifdef CONFIG_PROFILING`), which is not part of `audits/pending_patch.diff`.
  WHY: iter-1 S2 is refuted only *conditionally* — I reproduced the compile claim (`x86_64-elf-g++ -Werror` on `src/lib/test.cpp` for `-DCONFIG_PROFILING`, `-DCONFIG_COVERAGE` and neither: all rc=0), but that pass requires the worktree `test.hpp`; the hunk MUST land in the same commit or `make profiling` / `make profile-class` fail to compile.

- [S3] src/kernel/profiling/sampler.cpp (worktree, not in patch) + Makefile:680-682 — the new `profile-class` capture (`-debugcon file:build/profiling/$(CLASS)/samples.raw`) depends on the sampler writing `SMPL` to debugcon 0xE9, but the producer change (`arch::outb(arch::COM1,…)` → `arch::outb(0xE9,…)`) is not in the patch.
  WHY: without that file in the same commit, `samples.raw` contains only `[DUMP][DONE]` and `tools/extract_samples.py` aborts with "no SMPL marker found" — loud rather than silent, but the patch is incomplete as submitted (it also drops two unbounded `while ((arch::inb(COM1+5) & 0x20) == 0);` polls that only the worktree version removes).

- [S3] tools/coverage_report.py:141-142 — `while norm.startswith("./") or norm.startswith("../"): norm = norm[3:]` still strips three characters for the two-character `./` prefix.
  WHY: verified `area_of("./src/lib/test.cpp")` → `"other"` while `area_of("src/lib/test.cpp")` → `"lib"`, i.e. a `./`-prefixed addr2line path is silently dropped from the kernel area totals (iter-1 S3 #11 is only half fixed — the `../` case now works); use `norm[2:]` for `./`, `norm[3:]` for `../`.

- [S3] src/kernel/nexios_config.h:767-768 with src/kernel/task/task.cpp:528,643,667-668 — `CONFIG_KSTACK_WINDOW_SIZE` is doubled to 32 MiB, but the window's page-table pool is hard-wired to 8 PT pages (`s_kstack_pt_pages[8]`, `for (unsigned i = 0; i < 8; ++i)`) = 16 MiB and `map_kstack_page()` silently returns for `pt_idx >= 8`.
  WHY: the old 16 MiB window was fail-closed (`alloc_kslot()` returned 0 past the end); the new bound lets `alloc_kslot()` hand out VAs above 16 MiB whose pages are never mapped. Not reachable today (`KSLOT_POOL_SIZE = 64` × 135168 B ≈ 8.25 MiB high-water), so it is hardening only — either keep 16 MiB (the pool size is the real limit) or extend the PT pool to 16 pages.

- [S3] src/lib/test.cpp:499 and src/lib/test.cpp:551-586 — a `CONFIG_PROFILING` build dumps twice (`run_filtered()` → `profiling_dump_samples()`, then `shutdown_kernel()`'s block), and the first dump runs with interrupts enabled while `Timer::handle_irq()` (src/kernel/arch/x86_64/hal/timer.cpp:100) keeps writing `sample_head` / `sample_count` / `sample_buffer`.
  WHY: `Sampler::dump_to_serial()` neither masks IRQs nor clears `sampler_enabled`, so the new call site reads sampler state concurrently with its ISR producer (CODING_STYLE §11.6) and emits a second `SMPL`/`@@COVBEGIN@@` frame into the same capture; the `run_filtered()` call site is redundant with the post-`cli()` dump and should be dropped (or the sampler disabled around it).

- [S3] src/lib/test.cpp:561-586 — raw magic port literals `0x3F8` / `0x3FD`, two `for (int i = 0; i < 50000; ++i) arch::io_wait();` busy delays, `[DUMP]`/`[DONE]` debug markers and >80-char one-line loops, although `COM1`/`COM1_LSR` constants are defined 80 lines below in the same function.
  WHY: CODING_STYLE §10.5 (named constants), §8 (80 columns); iter-1 S3 #4 was only partially addressed (the block was moved ahead of the ACPI write, which is correct, but not cleaned up).

- [S3] src/lib/test.cpp:657-658, 687-688, 707-708, 735-736 — `arch::outb(COM1, '\\'); arch::outb(COM1, 'n');` emits a backslash followed by `n` (leftover escaping, `"\n"` was intended), and the ~80 marker bytes are written without a TX-ready wait although a bounded drain loop precedes them.
  WHY: cosmetic corruption of the parsed serial stream plus an inconsistency with the drain contract used everywhere else in the same function.

- [S3] Makefile:741 (`coverage-class` relink) — `@$(MAKE) --no-print-directory $(INITRD_OBJ) $(KERNEL)` does not inherit the target-specific `CXX`/`CXXFLAGS` (verified: GNU make does not export target-specific variables to sub-makes), and the target has no dependency on `coverage-build`.
  WHY: any object rebuilt during a per-class relink is compiled WITHOUT `-finstrument-functions` (silently under-reported coverage), and a standalone `make coverage-class` after a plain `make debug` boots an uninstrumented kernel and records a valid but empty (0-function) frame; add `export`-ed coverage flags or a stamp guard that fails when `build/.build-type` != `coverage`.

## VERIFIED — CORRECTIONS ACCEPTED

- **S2 #1 (unbounded COM1 polls) FIXED**: both `profiling_dump_samples()` polls are bounded by `SERIAL_DRAIN_POLL_LIMIT = 100000` (src/lib/test.cpp:645,650,702); the whole dump is additionally short-circuited by `ok`, so a stalled UART emits `@@COVABORT@@` after ~2 poll bursts instead of hanging per byte (`serial_put()` bound 2 000 000, every subsequent call guarded — verified by reading all call sites).
- **S2 #2 (missing declaration) REFUTED (conditional)**: see S3 above — declaration present in the worktree, three-variant `-Werror` compile reproduced by the auditor.
- **S3 #3 atomicity FIXED**: `func_count` / `dropped_funcs` are now read with `__atomic_load_n(..., __ATOMIC_RELAXED)` at gcov_handler.cpp:203,224,228; the only remaining non-atomic accesses are the `(void)` casts in the `!CONFIG_ARCH_X86_64` stub (245-246).
- **S3 probe bound / production BSS / dead API FIXED**: `MAX_PROBE = 32`; production object verified with `x86_64-elf-nm` as `func_addrs` 0x800 + `hash_slots` 0x800 = 4 KiB BSS (was 32 KiB), no producer exists without `-finstrument-functions`; `coverage_table_stats()`/`CoverageStats` are gone from the tree.
- **S3 kstack window**: see S3 above (implemented, but above the 16 MiB PT-pool cap).
- **S3 Makefile guards**: `coverage-class` now hard-fails on missing `gtimeout`, on a `$(QEMU_FLAGS)` without `-serial chardev:dbg` (present, Makefile:246-248), and on a missing/empty `serial.raw`; `check-build-stamp` still forces a clean when the stamp says `coverage`, so a later `make execute-test x86_64 debug <class>` cannot boot the instrumented ISO.
- **S3 host tool**: the "largest absolute gaps" section and repo-relative `shorten()` rendering are present; `area_of()` still mis-handles `./` (see S3).
- **Wire contract re-verified independently**: a synthetic 1500-function frame built with the kernel's own fold order (count + 9×count + registered + dropped, checksum emitted last) parses `valid=True/ok`; a single flipped byte → "checksum mismatch"; truncation → "no @@COVEND@@ sentinel". `python3 -m py_compile` clean.
- **Build neutrality (production)**: `gcov_handler.cpp` compiles `-Werror` clean for production, `CONFIG_PROFILING`, `CONFIG_COVERAGE` and the aarch64 `#else` branch; `saved_size > CONFIG_STACK_SIZE` is numerically identical to the old `> 65536` outside coverage builds and `KSTACK_ENTRY_SIZE = 16 + TCB::STACK_SIZE` scales with it (no snapshot-buffer overflow); `_task_trampoline`'s attribute is a no-op without `-finstrument-functions`; `nexios_config.h` overrides are `#ifdef CONFIG_COVERAGE`-only; the `test.cpp` blocks are `CONFIG_PROFILING`/`CONFIG_COVERAGE`-only, so the debug and release gates are bit-identical.
- **Gates**: `make check-style` → 288 files, Errors 0, Passed (no new error; the only new line is the false-positive `headers — Missing '#pragma once'` warning for `gcov_handler.hpp:1`, whose `#pragma once` is at line 23); `make check-config` → PASSED (1 pre-existing `CONFIG_TICK_HZ` warning).
- **Out of scope (pre-existing, not introduced by this patch)**: src/lib/test.cpp:514-520 and 548-552 still contain unbounded `while ((arch::inb(COM1_LSR) & 0x20) == 0) { }` polls on the same shutdown path — recommend the same bounded-drain treatment in a follow-up.

DECISION: APPROVED
