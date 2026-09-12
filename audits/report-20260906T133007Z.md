# AUDIT REPORT 20260906T133007Z
PATCH: audits/pending_patch.diff
FILES: Makefile, src/kernel/gcov/gcov_handler.cpp, src/kernel/gcov/gcov_handler.hpp, src/kernel/nexios_config.h, src/kernel/task/task.cpp, src/kernel/test/test_isolate.cpp, src/lib/test.cpp, tools/coverage_report.py

## FINDINGS

- [S2] src/lib/test.cpp:497 (call) and src/lib/test.cpp:638 (definition) — CONFIG_PROFILING code path enabled without its declaration: `kernel::test::profiling_dump_samples()` is called at line 497 but is only declared in `src/lib/test.hpp:171`, which is **not part of this patch** (it exists only as an uncommitted worktree change).
  WHY: the patch makes `profile-class` define `-DCONFIG_PROFILING` (Makefile:662) and the pre-existing `profiling` target already does, so both targets now compile the new `#ifdef CONFIG_PROFILING` block and fail with "profiling_dump_samples was not declared in this scope" — an existing build target is broken by this change set.

- [S2] src/lib/test.cpp:647 and src/lib/test.cpp:694 — new unbounded runtime blocking wait `while ((arch::inb(COM1_LSR) & 0x20) == 0) { }` in `profiling_dump_samples()`.
  WHY: CODING_STYLE §6 requires fully bounded loops with a deterministic max iteration count and explicitly excludes runtime blocking waits from the exemption; this is in the shutdown path, so a UART that never reports THR-empty hangs the kernel forever with interrupts disabled (`shutdown_kernel` ran `arch::cli()`), and only the host `gtimeout` terminates the run — the same patch correctly bounds the equivalent poll in `gcov_flush_to_serial()` with `SERIAL_POLL_LIMIT`, so the new function is inconsistent with its own contract.

- [S3] src/lib/test.cpp:568-582 — the CONFIG_PROFILING dump (`gcov_flush_to_serial()` + `Sampler::dump_to_serial()`, markers, drains, delays) still runs *after* `arch::outw(arch::QEMU_ACPI_PORT, 0x2000)` / `arch::outw(arch::QEMU_SHUTDOWN_PORT, 0x2000)`.
  WHY: the patch's own justification for moving the coverage dump ahead of those port writes ("the ACPI sleep write tears the VM down immediately") applies verbatim to this block, so the newly added dump/marker work is unreachable in practice.

- [S3] src/lib/test.cpp:558-581 and src/lib/test.cpp:650-693 — raw magic port literals `0x3F8`/`0x3FD`, two `for (int i = 0; i < 50000; ++i) arch::io_wait();` busy delays, and leftover `[DUMP]`/`[DONE]` debug markers plus a literal `'\\'`/`'n'` pair where `\n` was intended.
  WHY: the surrounding code already defines `static constexpr uint16_t COM1 = 0x3F8;` (src/lib/test.cpp:511-512) and the file is compiled into the kernel, so unnamed constants and debug scaffolding pollute the serial stream that the harness parses.

- [S3] src/kernel/gcov/gcov_handler.cpp:74 — `static uint32_t hash_slots[HASH_SLOTS]` (8192 x 4 B = 32 KiB) is allocated in **every** build.
  WHY: `src/kernel/gcov/gcov_handler.cpp` is auto-discovered by `mk/rules.mk:20` and compiled into debug, release, aarch64 and riscv64 kernels, but the table is only reachable when `-finstrument-functions` is passed (only the `profiling` / `coverage` targets), so production BSS grows by 32 KiB with no consumer; consider gating the table on `CONFIG_COVERAGE || CONFIG_PROFILING` or shrinking the non-coverage `MAX_FUNCS`.

- [S3] src/kernel/gcov/gcov_handler.cpp:197, 218, 246 — `func_count` and `dropped_funcs` are written with `__atomic_fetch_add(..., __ATOMIC_RELAXED)` but read with plain loads.
  WHY: CODING_STYLE §11.6 requires an object accessed concurrently to be accessed atomically from *every* context; use `__atomic_load_n(&func_count, __ATOMIC_RELAXED)` in `gcov_flush_to_serial()` and `coverage_table_stats()`.

- [S3] src/kernel/gcov/gcov_handler.cpp:64 — `MAX_PROBE = 8` with `HASH_SLOTS = 2 * MAX_FUNCS` (load factor 0.5).
  WHY: linear probing at 50 % load produces clusters of ~log2(n) (~14 for 16384 entries), so probe exhaustion is statistically likely; each exhaust increments `dropped_funcs`, and `tools/coverage_report.py:892-895` aborts the whole report when `dropped > 0`, i.e. a healthy run can be rejected. Raise `MAX_PROBE` (e.g. 32) or lower the load factor.

- [S3] src/kernel/gcov/gcov_handler.cpp:105 — `func_count` is incremented past `MAX_FUNCS` on every rejected registration and is never clamped outside `gcov_flush_to_serial()`.
  WHY: on a long run `func_count` can wrap (u32) and `coverage_table_stats()` reports an unclamped, meaningless `registered` value; the dump clamp at line 198 hides the discrepancy.

- [S3] src/kernel/gcov/gcov_handler.cpp:244 — `coverage_table_stats()` has no caller anywhere in the tree.
  WHY: a new exported API with zero users is dead code (CODING_STYLE §5.3 spirit) and gives no diagnostic value unless the dump fails.

- [S3] src/kernel/nexios_config.h:762-767 — stack tiers are doubled under `CONFIG_COVERAGE` while `CONFIG_KSTACK_WINDOW_SIZE` stays at 16 MiB.
  WHY: `alloc_kslot()` (src/kernel/task/task.cpp:588-596) bump-allocates `CONFIG_STACK_SIZE + guard` from that fixed window and returns 0 when exhausted, so the instrumented build silently halves the number of concurrently allocatable kernel stacks; scale the window with the same `#ifdef`.

- [S3] Makefile (coverage-class, lines 91-93) — `gtimeout $(COVERAGE_TIMEOUT) ... || true` with no `command -v gtimeout` guard (unlike `debug-test`, Makefile:880) and a positional `sed 's|-serial chardev:dbg|...|'` on `$(QEMU_FLAGS)`.
  WHY: if `gtimeout` is missing the QEMU boot never happens and `|| true` hides it, and if the sed pattern ever stops matching, serial output goes to stdio and `serial.raw` is never created — both produce a silently invalid capture instead of an error.

- [S3] tools/coverage_report.py:835-837 — `norm = norm[3:] if norm.startswith("./") else norm[3:]`.
  WHY: both branches strip three characters, so a leading `../` is mis-normalised; harmless for current paths but a latent defect in the host tool.

## VERIFIED NEUTRAL / ACCEPTED

- Production neutrality confirmed for: the `nexios_config.h` stack override (guarded by `#ifdef CONFIG_COVERAGE`, never defined by `debug`/`release`), the `test_isolate.cpp` bound (`saved_size > CONFIG_STACK_SIZE` is numerically identical to the previous `> 65536` when `CONFIG_STACK_SIZE == 65536`), and `_task_trampoline`'s `no_instrument_function` (a no-op attribute unless `-finstrument-functions` is passed).
- The trampoline reasoning checks out against src/kernel/task/task.cpp:1058-1066: the synthetic frame ends with the 5-word `iret` frame, so the task enters `_task_trampoline` with `RSP == kernel_stack_top` and no return address above it; with `-fno-omit-frame-pointer` the hook reads `[rbp+8] == [kernel_stack_top+8]`, which is outside the stack — matching the reported `CR2 == stack top`. No other C++ entry point in the tree is reached with a synthetic frame (all others are entered by `call` from `isr_stubs.asm`), so the single attribute is sufficient.
- Instrumented-build safety: the hook is `no_instrument_function` and calls only `slot_for`/`find_or_add_func` (also `no_instrument_function`), does no allocation, takes no lock, and every loop is bounded (`MAX_PROBE`, `SERIAL_POLL_LIMIT`, `count <= MAX_FUNCS`). The claim is atomic (`__atomic_fetch_add`); a nested IRQ can only produce a duplicate slot/index, never a lost or corrupted entry, because the dump iterates `func_addrs[0..count)` rather than the slot map and the host unions addresses. Overflow/probe-exhaustion is fail-closed (`dropped_funcs`, `STAT` trailer, `@@COVABORT@@`) and never mis-attributes to index 0.
- Wire contract verified byte-for-byte against `tools/coverage_report.py:parse_frame()`: the kernel checksum covers count + 9 x count + registered + dropped, and the parser folds exactly the same bytes (the emitted checksum value is not folded into itself by either side).

## PATCH

`audits/rejected_patch.diff` was written: it bounds the two unbounded COM1 TX-ready polls in `profiling_dump_samples()` with a `SERIAL_DRAIN_POLL_LIMIT` guarded loop. The `src/lib/test.hpp` declaration for `profiling_dump_samples()` is **already present in the worktree** and therefore not repeated in the patch — it must simply be included in the same commit (dropping it breaks `make profiling` and `make profile-class`).

DECISION: REJECTED
