# PLAN: #572 [F20] — libc dependency audit for PAUSE=36 retirement and the 34/80–83 POSIX-time move (PAUSE half actionable; MOVE half parks on #194)

## AFFECTED FILES

Phase 1 — audit deliverable (no production code):

- `audits/f20-libc-syscall-audit-572.md` — NEW. Caller inventory (PAUSE set, POSIX-time set, tracked-ELF sweep), per-symbol disposition, blocker verdicts, preconditions P1–P4 for the MOVE half, retrieval provenance.
- GitHub #572 / #194 — comments only (no file changes): audit verdict + counts on #572; precondition list P1–P4 on #194.

Phase 2 — PAUSE=36 retirement (the only code increment; executes only if Phase 1's sweep confirms 0 callers, which current evidence already shows):

- `src/kernel/syscall/syscall.hpp` — :79 keep `PAUSE = 36` as a RETIRED tombstone (number never reused, T-1 §3.1 :112-113); :232 delete `SyscallNumber::PAUSE` from `k_syscall_fast[]`; :332 delete the `sys_pause` declaration; :538 repoint slot 36 to `&Syscall::sys_unimplemented`.
- `src/kernel/syscall/syscall_handlers_misc.cpp` — :275-279 delete the `Syscall::sys_pause` definition (`arch::hlt()` body; no other user of it).
- `src/libc/syscall.h` — :69 delete `#define SYS_PAUSE 36`; :238-240 delete the `sys_pause()` inline. `SYS_MAX` (:426) stays 105.
- `src/libc/gen/nexios_syscalls.h` / `.c` — regenerated ONLY by `tools/gen_syscall_stubs.py` (never hand-edited): `nexios_sys_pause` disappears (.c :258-262, .h :322-330).
- `src/libc/unistd.h` (:61) and `src/libc/signal.h` (:58) — delete `int pause(void);` in both. It is a dead declaration (no definition anywhere in tree) and the T-1 replacement (NOTIFY_WAIT / bounded RECEIVE) must not be faked on top of a retired number.
- `docs/specs/syscall-fastpath.md` — :25 FAST membership set and :131 test-plan note lose PAUSE.
- `docs/specs/microkernel-transition.md` — :150 T-1 row 36 records "RETIRED (landed, F20 #572)"; :1003 §15 F20 row records audit report path + "PAUSE retired; 34/80–83 parked on #194". §3.4 derived counts (:218-226) need no edit (36 is already inside MOVE/RETIRE).

Phase 3 — 34 + 80–83 MOVE: NO file changes in this issue. Findings + preconditions only (see STEP 3).

Explicit non-goals (no edits in this issue): `src/kernel/time/**` (posix_time, timer_wheel, timer_srv), `src/kernel/syscall/syscall_handlers_posix_time.cpp`, `userspace/*` (incl. `timer-srv.c`, `rtc-pd.c`, `xmodem.c`, `sh.c`, `prime.c`, `*-bench.c`, `stub-roundtrip.c`, `driver-pd-parity.c`), `src/kernel/test/**` (QE-owned, read-only to this lane), `tools/gen_syscall_stubs.py` (no generator change needed — undefined numbers are already skipped, cf. 86-90), any arch file (change is arch-neutral: one shared dispatch table).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **Slot completeness is load-bearing.** `syscall_table_[number]` is invoked with no null check on both paths (`syscall.cpp:195`, `:216`) after only a bounds check against `MAX_SYSCALL = 105` (`syscall.hpp:148`, `:200`). A retired number MUST map to `Syscall::sys_unimplemented` (fail-closed ENOSYS = -38, `syscall_handlers_misc.cpp:544-547`), never to a hole, `nullptr`, or an omitted initializer.
- **Tombstone rule (T-1 §3.1 :112-113).** Number 36 is retired, not freed: it SHALL NOT be reused, and `SyscallNumber::PAUSE` stays in the enum as the tombstone so the QE-owned `test_syscall_fastpath.cpp:121` keeps compiling byte-identically. Deleting the enumerator would force an edit into a test tree this lane may not touch.
- **FAST mask is derived, never hand-written.** `SYSCALL_FAST_MASK` is computed from `k_syscall_fast[]` (`syscall.hpp:247-250`); the only mutation is removing the `PAUSE` entry. The `fast_mask_matches_config` invariant (popcount == `k_fast_count`, all members < MAX_SYSCALL, non-empty, no bit ≥ 128 — `test_syscall_fastpath.cpp:78-96`) therefore re-derives itself and must stay green with zero test edits.
- **No concurrency boundary is crossed.** `sys_pause` is a FAST member: no lock, no user-pointer dereference, no allocation, no scheduler transition; its replacement is a pure constant return. No IrqGuard window, no lock ordering, no ResourceTracker accounting, and no RT-path allocation is introduced or moved. `CONFIG_TIMER_SRV` code paths are untouched.
- **Generated-stub byte-determinism.** The `src/libc/gen/` pair may only be produced by `python3 tools/gen_syscall_stubs.py`; `--verify` must pass afterwards (a hand-edit forges the ABI).
- **Archive invariant (parked, Phase 3).** When 34/80–83 eventually retire, the same two rules apply (slot → `sys_unimplemented`, number never reused) plus one Phase-3-only constraint: timer-srv must never become a client of its own RPC (P1 below).

## STEP-BY-STEP CHANGES

Phase 1 — audit (deliverable; no code, no test edits):

1. `audits/f20-libc-syscall-audit-572.md`: open with provenance — graphify query `"libc PAUSE syscall userspace callers timer-srv POSIX time"` (BFS d=2, 247 nodes; USED: libc/syscall.h, `__syscall5`, PosixTime, posix_time.cpp, timer nodes; IGNORED: scheduler/IPC/ELF noise) + vault search (0 hits → fell back to in-repo `docs/specs/microkernel-transition.md` T-1 rows :148/:150/:183/:1003 and `docs/specs/syscall-fastpath.md` :25 — USED as normative precedent). Then record the exact command set used:
   - `rg -n 'SYS_PAUSE|sys_pause|nexios_sys_pause' src/ userspace/ tools/ docs/`
   - `rg -n '\bpause\s*\(' src/ userspace/` — every hit classified as `arch::pause`/`pause()` HAL (different symbol, majority) vs the libc POSIX name.
   - `rg -n '^\s*(int|void)\s+pause\s*\(void\)\s*\{'` → MUST be empty (proves the `pause()` declaration has no definition).
   - `rg -n '\b36\b' src/kernel/syscall/ src/libc/syscall.h docs/specs/`
   - POSIX half: `rg -n 'SYS_(GETTOD|CLOCK_GETTIME|NANOSLEEP|TIMER_CREATE|TIMERFD_CREATE)|sys_(gettod|clock_gettime|nanosleep|timer_create|timerfd_create|timer_settime|timerfd_settime)\(' src/ userspace/`
   - `git ls-files '*.elf'` — for each tracked binary name its source and confirm it is inside the audited source set (no tracked ELF may consume 36/34/80–83 from an unaudited source).
2. Same report, PAUSE verdict section: dependency set is exactly 6 symbols in 4 files — `syscall.h:69` (define), `syscall.h:238-240` (inline), `gen/nexios_syscalls.c:258-262` + `.h:322-330` (generated stub), `unistd.h:61`, `signal.h:58` (declarations) — **0 call sites, 0 definitions** across `src/libc`, `src/services`, `src/programs`, `userspace`, `src/kernel/test`. Also record the semantic finding: the kernel handler is a single `arch::hlt()` (`syscall_handlers_misc.cpp:275-279`), i.e. NOT POSIX `pause()` (no block-until-signal, no EINTR) — so retirement loses no behavioural contract and libc's declared `pause()` was never implementable on it. Verdict: **RETIRE unblocked**.
3. Same report, MOVE verdict section: libc's time surface is `time.c` only (`time` :15, `gettimeofday` :28, `clock_gettime` :41, `sleep` :49, `nanosleep` :67 → 80/81, with the 34 fallback at :20/:34) plus the generated stubs; in-tree clients are 8 files (`timer-srv.c:151`, `timer-srv-bench.c:10-33`, `rtc-pd.c:228`, `prime.c:181/197`, `driver-pd-parity.c:14-26`, `stub-roundtrip.c:29-57`, `xmodem.c:112/824`, `sh.c:129`). Record the 4 blocking preconditions and the verdict **MOVE blocked → parks as enabling evidence until #194 unblocks**:
   - **P1 (hard blocker, self-RPC):** `userspace/timer-srv.c:147-161 tmr_now_ns()` calls `clock_gettime(CLOCK_MONOTONIC)` = syscall 80. Once 80 is a timer-srv RPC, the PD is its own client (self-RPC → deadlock or recursion). Must be re-based on `SYS_GET_TICKS` (5) × `g_ns_per_tick` — the fields already exist (`:127`, `:362`, `:388`, `:433`) — or on a kernel-private MONOTONIC op. This is a #194 design decision, not an F20 edit.
   - **P2 (same-release coupling):** libc must switch to a timer-srv client library in the SAME increment that deletes the `CONFIG_TIMER_SRV=0` leg (`syscall_handlers_posix_time.cpp` seams :89/:182/:221/:329/:465/:580; `timer_srv.cpp` proxies) — #194 increment 2. A separate release breaks every libc time caller on the flip.
   - **P3 (34 coupling):** `time.c:17-35` carries a self-declared "fallback one release" GETTOD path; it must be deleted together with the 34 slot retirement, not before.
   - **P4 (QE handoff):** the numbers are pinned in QE-owned tests (`test_syscall.cpp:140 syscall_gettod`, `:713 syscall_user_gettod_uname`, plus the `posix_time`/`srv_timer` classes). Number retirement requires a QE test-tree increment in the same cycle; the dev lane must not edit tests.
4. Post the verdicts (inventory counts, PAUSE = unblocked, MOVE = blocked with P1–P4) as a #572 comment and the P1–P4 list as a #194 comment. Do not modify #194.

Phase 2 — PAUSE=36 retirement (all steps in one commit/change set; no further design decisions):

5. `src/kernel/syscall/syscall.hpp:232` — remove `SyscallNumber::PAUSE` from `k_syscall_fast[]`; leave a one-line comment there: `// 36 PAUSE retired (T-1 / F20 #572): not a FAST member.`
6. `src/kernel/syscall/syscall.hpp:79` — keep `PAUSE = 36,` and re-document in place: `///< RETIRED (T-1 :150, F20 #572): slot returns ENOSYS; number SHALL NOT be reused.` (tombstone keeps QE tests compiling).
7. `src/kernel/syscall/syscall.hpp:332` — delete `static uint64_t sys_pause(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);`; `src/kernel/syscall/syscall.hpp:538` — replace `&Syscall::sys_pause` with `&Syscall::sys_unimplemented, // 36 PAUSE RETIRED (F20 #572, ENOSYS)` (slot stays populated; array stays 105 entries).
8. `src/kernel/syscall/syscall_handlers_misc.cpp:275-279` — delete the `Syscall::sys_pause` definition.
9. `src/libc/syscall.h:69` — delete `#define SYS_PAUSE 36`; `:238-240` — delete the `sys_pause()` inline. Leave `SYS_MAX 105` (:426) and every neighbouring number untouched.
10. `python3 tools/gen_syscall_stubs.py` then `python3 tools/gen_syscall_stubs.py --verify`; commit the regenerated `src/libc/gen/nexios_syscalls.{h,c}`. Expect only the `nexios_sys_pause` stub lines to disappear (number 36 is simply undefined, the same way 86-90 are); if `--verify` reports anything else, revert the generated pair and stop.
11. `src/libc/unistd.h:61` and `src/libc/signal.h:58` — delete `int pause(void);` in both files. Do not add a replacement; the POSIX-shaped entry point returns with the sync-srv/NOTIFY_WAIT migration.
12. `docs/specs/syscall-fastpath.md:25` — landed FAST set becomes `{YIELD, GET_TICKS, PRINT, CREATE_MAILBOX, DESTROY_MAILBOX, GETPID, REBOOT, HALT, ABI_VERSION, SEND_FAST, RECV_FAST, SEND_SYNC_FAST}` (list PAUSE's removal explicitly so the reconciliation note stays honest); `:131` — test-plan note becomes "HALT/REBOOT excluded (halt the CPU); PAUSE retired at 36 (F20 #572)".
13. `docs/specs/microkernel-transition.md:150` — disposition: `RETIRED (landed, F20 #572: 0 libc callers; slot = sys_unimplemented)`; `:1003` — F20 row: audit report path + "PAUSE retired; 34/80–83 parked on #194 (P1–P4)". Touch nothing else in T-1 or §3.4.
14. Verification greps, all must be empty except the intentional tombstone: `rg -n 'SYS_PAUSE|sys_pause|nexios_sys_pause' src/ userspace/` → empty; `rg -n 'int pause\(void\)' src/libc/` → empty; `rg -n 'PAUSE' src/kernel/syscall/ src/libc/ docs/specs/` → tombstone comment(s) only; `rg -n 'PAUSE' src/kernel/test/` → `test_syscall_fastpath.cpp:121-123` only (unchanged).
15. Close criteria for #572 (recorded here, executed by the developer): (a) `audits/f20-libc-syscall-audit-572.md` committed with the inventory + P1–P4; (b) verdict comments posted on #572 and #194; (c) Phase-2 code audit APPROVED (diff-patch protocol) since `src/kernel/**` + `src/libc/**` are production paths; (d) named classes 0 failures on x86_64 debug and release, plus the arch build/run evidence in TEST STRATEGY; (e) `--verify` clean and step-14 greps clean; (f) only then close #572, with a closing comment that the MOVE half remains owned by #194 (blocked on #190) and that F20 is "audited, half landed". If Phase 1's sweep contradicts step 2 (any caller found), drop Phase 2 entirely, close #572 as audit-only, and file one follow-up issue per blocking caller — no syscall removal.

## TEST STRATEGY

- **New tests: none, and this is deliberate.** (a) The dev lane may not write into `src/kernel/test/` (QE-owned); (b) the retirement's exact invariant is already asserted by `fast_mask_matches_config` and re-derived at compile time from `k_syscall_fast[]`; (c) the retired slot's behaviour is the pre-existing `sys_unimplemented` contract, not new logic; (d) per LEARNINGS #316, `src/libc` C files never link into the kernel, so a kernel unit test for a libc stub would be unlinked and dishonest — the only honest libc vehicles are the existing Ring-3 classes. Optional QE handoff (not required for closure): one case asserting slot 36 returns ENOSYS(-38).
- **Existing classes (x86_64 debug, then release):**
  - `syscall_fastpath` — mask/count/FAST-vs-FULL parity; the class that catches a mask/list mismatch or an accidental hole (primary gate).
  - `syscall_core` — dispatch table + ABI surface assert (`syscall_abi_surface_mirror_and_version` pins MAX_SYSCALL = 105 and never pins 36, so retirement must leave it green).
  - `libc_syscall` and `basic_lib` — libc↔syscall mapping and in-tree libc image still link/boot after stub regeneration.
  - `libc_verify` — Ring-3 libc probe (the only honest libc vehicle).
  - `posix_time` — must stay green untouched, proving the 80–83 half was not disturbed (do NOT run `srv_timer`; `CONFIG_TIMER_SRV` OFF leg is out of scope).
- **Cross-arch parity (mandatory, and satisfied trivially by this change's shape):** the change is arch-neutral (one shared `syscall_table_` / one shared libc header), so no arch-equivalent implementation is needed in either direction. Evidence required: `tools/meson/build.sh` green for `x86_64`, `aarch64`, `riscv64`, plus `syscall_fastpath` and `libc_verify` run on `aarch64` and `riscv64` debug (riscv64 honours the 400 s expect budget). If #572 closes audit-only (Phase 2 dropped), no arch matrix is required and the closing comment must say so explicitly.
- **Harness discipline:** `python3 tools/meson/qemu_test.py --arch <arch> --build <build> --class <class>` only; one `test-history.txt` row appended per class run, including any abnormal termination.

## SIL 3 / SAFETY RISKS

- [S1] **Retired slot left as a hole or `nullptr`** — `syscall_table_[number]` is called unchecked (`syscall.cpp:195`, `:216`); an empty slot is a Ring-3-reachable null function call. Mitigation: step 7 pins slot 36 to `sys_unimplemented` (ENOSYS, fail-closed); `syscall_core` + `syscall_fastpath` runs confirm dispatch; the array must still have exactly 105 entries.
- [S1] **Silent behaviour change for a client not found by source grep** (out-of-tree or tracked prebuilt binary using 36 → now ENOSYS instead of a one-tick hlt). Mitigation: step 1's `git ls-files '*.elf'` sweep binds every tracked binary to audited sources; the change is fail-closed (error, never silent success); the audit report records the finding either way.
- [S2] **Deleting the `PAUSE` enumerator** would break the QE-owned `test_syscall_fastpath.cpp:121` and force a dev-lane edit into a test tree. Mitigation: tombstone the name (step 6); the mask test iterates `k_syscall_fast[]`, so a tombstone is invisible to it.
- [S2] **Generated-stub drift** — a stale or hand-edited `nexios_syscalls.*` keeps advertising syscall 36 (which now returns ENOSYS) and desynchronises the libc ABI from the kernel table. Mitigation: regenerate-and-`--verify` only (step 10); step 14 greps must be clean.
- [S2] **Phase-3 scope creep** ("retire 34/80–83 too while we're here") would break every libc time caller (no timer-srv client library exists), trip QE-owned POSIX tests, and — P1 — deadlock timer-srv on its own clock read. Mitigation: Phase 3 is explicitly no-code in this issue; P1–P4 are posted to #194 as that issue's preconditions.
- [S3] **Spec/bookkeeping drift** — `syscall-fastpath.md` and T-1 must move in the same change set as the code, or the next audit reads a spec claiming a FAST member that no longer exists. Mitigation: steps 12-13 land with steps 5-11 in one commit.
- [S3] **Removing the in-tree `int pause(void);` declarations** could collide with a picolibc sysroot declaration for userspace builds. Mitigation: the removal is limited to `src/libc/unistd.h`/`signal.h` (in-tree only); step 1 records whether any in-tree include site uses it (today: none) and step 14 re-greps.

## OPEN QUESTIONS

- none blocking. Recorded decision: PAUSE retirement is included in #572 because T-1 :150 gates exactly on this libc check ("libc check F20") and the check is clean; the MOVE half (34/80–83) is closed as *audited-and-parked* with P1–P4 handed to #194, per the issue's own fallback shape.
