# PLAN: TLS on context switch (#74) — TCB tls_base_ + TLS_SET(85) + per-arch dispatch load

Source: planner subagent 2026-09-19, v2 rework 2026-09-19 (applies audits/rejected_plan-74.md
verbatim + ARM64/RISC-V arch specifics, all anchors evidence-verified).
Binding spec: docs/specs/syscall-abi-picolibc.md §7 + §12.
Status: PLAN REVIEW v2 (no code changed yet; audits/pending_patch.diff does not exist).

## AFFECTED FILES
- `src/kernel/task/task.hpp` — new `uint64_t tls_base_` field + ctor init-list entry (mirrors `iopb_slot_` at task.hpp:278,542)
- `src/kernel/task/task.cpp` — 3 memset sites: `create` (~911), `create_user` (~1155), `clone` (~1353); clone inherit override
- `src/kernel/elf/elf.cpp` — `finalize_loaded_task` memset site (~513) + `exec_into_current` reset + live clear (~773, after `page_table_`/`is_user_` swap)
- `src/kernel/syscall/syscall.hpp` — declare `sys_tls_set`; table slot 85 `sys_unimplemented` → `sys_tls_set` (TLS_SET=85, MAX_SYSCALL=86 already present); TLS_SET stays out of `k_syscall_fast[]`/`SYSCALL_FAST_MASK`
- `src/kernel/syscall/syscall_handlers_tls.cpp` (new, auto-globbed via `mk/rules.mk:15`) — `sys_tls_set` implementation (validate + delegate to Scheduler)
- `src/kernel/task/scheduler.hpp` — declare `set_tls_base_err(TaskControlBlock&, uint64_t)` beside `set_affinity_err` (:240); add `SwSlots::load_tls_from()` + `scheduler_load_tls_from[CONFIG_MAX_CPUS]` decl
- `src/kernel/task/scheduler.cpp` — implement `set_tls_base_err`; publish in `switch_to_task` (~3599–3635) AND `switch_away_from_terminating` second publish site (~3972–3992); clear beside EVERY `load_cr3_from` clear (:605,947,1802,1827,1984,2013,3810,4674,4192,4209) + capture_percpu/restore_percpu (:4071/:4100)
- `src/kernel/syscall/syscall_errors.hpp` — `X(TLS_INVALID_BASE, 109, "Invalid TLS base address")` (109 = true first free ID above task band 101–108); derives `SYS_ERR_TLS_INVALID_BASE` via the `SYS_ERR_##name` X-macro (:129)
- `src/kernel/arch/x86_64/hal/msr_impl.hpp` — add `MSR_FS_BASE = 0xC0000100` beside `MSR_GS_BASE`
- `src/kernel/arch/aarch64/hal/io_impl.hpp` — add `write_tpidr_el0(uint64_t)` beside `write_ttbr0_el1` (:251–255), with trailing `isb()` (TPIDR_EL0 write must be observed before ERET to EL0)
- `src/kernel/arch/riscv64/hal/io_impl.hpp` — add `write_tp(uint64_t)` beside the satp helpers (:167–195) via `asm volatile("mv tp, %0")` (tp is ABI-reserved, compiler never allocates it; write is exact)
- `src/kernel/core/global_state.{hpp,cpp}` — define `scheduler_load_tls_from[CONFIG_MAX_CPUS]` beside `scheduler_load_cr3_from`
- `src/kernel/arch/x86_64/isr_stubs.asm` — FS apply after `.load_cr3` clear (:425, before `jmp .restore` :426) + clear in `.abort_switch` beside CR3/next-id clears (:434–438)
- `src/kernel/arch/aarch64/vectors.S` — TPIDR apply in `irq_context_switch_common`: retarget `cbz x3, .restore` (:239) → `cbz x3, .tls_apply`; new `.tls_apply` block between CR3 clear (:248) and `isb` (:249)
- `src/kernel/arch/riscv64/syscall_entry.S` — TLS consume after satp block (:210): load slot, unconditional clear, conditional `sd` into trap-frame `OFF_TP(sp)` (:26,69), before CSR/GPR restore
- `src/kernel/test/test_tls.cpp` (new) + test registry/`test_expected_counts.hpp` — new `tls` class
- `src/libc/syscall.h` — verify-only (SYS_TLS_SET=85, SYS_MAX=86 present); add `sys_tls_set` wrapper only if absent

## INVARIANTS & CONCURRENCY BOUNDARIES
- TCB memset discipline (LEARNINGS.md:134, `iopb_slot_` precedent): explicit `tls_base_ = 0` at all 4 sites + `tls_base_(0)` ctor init; clone inherits `parent->tls_base_`; exec resets to 0 (fresh image never inherits).
- `tls_base_==0` means unset: publish 0, every arch apply path loads nothing.
- Setter lock discipline (auditor S2 fix): Syscall NEVER touches `scheduler_lock_` directly (private, scheduler.hpp:933). New `Scheduler::set_tls_base_err` mirrors `set_affinity_err` (scheduler.cpp:651): `arch::IrqGuard` + `quiesce_enter()` (only if the affinity precedent's quiesce is required for same-class mutation — else IrqGuard + `SpinLockGuard(scheduler_lock_)`, re-fetch current, store). Returns `errors::SchedulerError` (`OK` / `NO_CURRENT`, scheduler_errors.hpp:32,37). Handler maps to `0` / `(uint64_t)-SYS_ERR_SCHED_NO_CURRENT`. Apply-live-register step runs AFTER lock release (pure register write, no shared state).
- Setter live-apply (auditor S2 fix, §13 crt0 TLS_SET-then-use with no preemption between): when target is the running task (`is_current_on_any_cpu` precedent, scheduler.cpp:678), immediately load the live register after unlock — x86_64 `arch::wrmsr(MSR_FS_BASE, base)` (msr_impl.hpp:36; WRMSR privileged, task-context syscall = ring 0, legal); aarch64 `write_tpidr_el0(base)` (new helper; EL1→TPIDR_EL0 legal); riscv64 `write_tp(base)` (new helper; writes the live tp of the running task). Base 0 clears the live register the same way. Non-running targets need no live write (publish/apply covers them at dispatch).
- Deferred-switch single-writer/single-applier per CPU (BRIEFING §2): `switch_to_task` + `switch_away_from_terminating` publish `load_tls_from[cpu]` alongside CR3 with `__ATOMIC_RELEASE` under caller lock/IRQ-off; exactly one epilogue applier per CPU consumes before user return; last writer wins; EVERY clear site that zeroes `load_cr3_from` zeroes `load_tls_from` beside it with the same order (:605,947,1802,1827,1984,2013,3810,4674 + BSP 4192 + clear_switch_globals 4209 + capture/restore 4071/4100 + x86 `.abort_switch`).
- swapgs invariant: x86_64 touches FS_BASE (0xC0000100) only; `MSR_GS_BASE`/`MSR_KERNEL_GS_BASE`/`swapgs`/per-CPU GS (percpu.cpp:25,46) untouched; regression asserts GS_BASE unchanged.
- Validation gates (arch-specific, all ENSURE-free per CODING_STYLE §5):
  - x86_64: `VirtualAddress(arg0).is_canonical()` (48-bit, address.hpp:111) AND `arg0 < CONFIG_USER_SPACE_LIMIT` (0x00007FFFFFFFFFFF, 47-bit, nexios_config.h:141). LA57: NO `LA57/paging_levels/five-level` symbol exists tree-wide (verified zero matches) — the kernel never enables LA57, so the 48-bit gate is exact, not an approximation. No live-mode query (auditor S3 fix — the planned branch was dead untestable code).
  - aarch64: `arg0 < CONFIG_USER_SPACE_LIMIT` (0x0000FFFFFFFFFFFF, 48-bit, nexios_config.h:152) SUBSUMES canonical (values < 2^48 have zero top-16 = sign of bit 47); `is_canonical()` additionally applied where the helper is arch-generic, harmless duplicate.
  - riscv64 Sv39: `CONFIG_USER_SPACE_LIMIT` (0x000000FFFFFFFFFF, 40-bit, nexios_config.h:163) admits non-canonical Sv39 values, so an EXPLICIT canonical check is mandatory: `(arg0 >> 39) == 0` (Sv39 user half = bits[63:39] zero; satp programmed Sv39-only, page_table_impl.hpp:52). Reject otherwise with the dedicated code.
  - All archs: `arg0 == 0` bypasses validation = unset; any other failure → `(uint64_t)-SYS_ERR_TLS_INVALID_BASE`. No kernel pointer can result (§2: success returns are 0 only).
- x86_64 asm scratch discipline (evidence-verified): `.restore` pops ALL GPRs from the ISR frame (:473–487), and the CR3 block already uses rax as scratch (:415–425); r11 = `[gs:0x10]` cpu index (:167, percpu.hpp:53) preserved via push/pop around calls (:217/219, :391/393). TLS insert uses rax/rcx/rdx as scratch (restored by :473–487 pops), r11 READ-ONLY index, no calls. Exact preserve set, no over-saving.
- aarch64 asm discipline: comment :232–233 (all user regs in save area, no caller-saved regs to protect) + CR3 block precedent reuses x3/x4 (:237–248) — TLS block reuses x3/x4 identically. adrp/ldr NON-indexed base access mirrors the file's existing pattern (:215–248). Single-core (sched_up_cpus returns 1 off-x86, scheduler.cpp:645) so base == [0] is exact today; SwSlots accessor keeps the C++ side per-CPU for SMP-proofing.
- aarch64 no-abort invariant: `irq_context_switch_common` has NO abort path (apply-as-observed, documented S3 at :208–211); the `cbz x0, .restore` (:217) no-switch path skips TLS — sound because publish+arm are atomic under the same lock/IRQ-off discipline, so slot==0 whenever no switch is armed. Stated as relied-upon invariant + pinned by `tls_cancel_clears_slot` on x86 cancel paths.
- riscv64 frame discipline: trap frame layout SAVE_SIZE=296, OFF_TP=24 (:9,:26); entry saves x4→OFF_TP (:69), satp switch at :203–210 (NON-indexed base, single-core), GPR restore reloads x4 from OFF_TP at :227 BEFORE sret (:259). TLS consume MUST target the OFF_TP save area (live-tp write at the satp site is clobbered by :227 — auditor S2). t0/t1 already used as scratch in this path (:204–216), reuse them.
- Clone inherits; exec/finalize reset; snapshot parity via TaskFields POD (`tls_base` beside `iopb_slot`, capture :4265/restore :4353, buffer sized by sizeof).
- No new ResourceTracker type (plain uint64): zero-delta by construction. No `new/delete`, no `const_cast`, 80-col, `#pragma once`, `arch::pause()` in spins — `make build` Errors 0.

## STEP-BY-STEP CHANGES
1. `task.hpp`: add `uint64_t tls_base_;` beside `iopb_slot_` (:542) with doc (`user VA, 0=unset, issue #74`); add `tls_base_(0)` to ctor init-list beside `iopb_slot_(IOPB_SLOT_NONE)` (:278).
2. `task.cpp` 3 sites: after each `memset(tcb,0,…)` + `iopb_slot_=IOPB_SLOT_NONE` (:915/:1159/:1357) insert `tcb->tls_base_ = 0;`. In `clone` only, after the inherit block add `tcb->tls_base_ = parent->tls_base_;`.
3. `elf.cpp`: `finalize_loaded_task` after :514 insert `tcb->tls_base_ = 0;`. `exec_into_current` after the swap commits (`page_table_`/`is_user_` :773–774, inside the success path): insert `tcb->tls_base_ = 0;` + live clear — x86_64 `arch::wrmsr(arch::MSR_FS_BASE, 0)`; aarch64 `arch::write_tpidr_el0(0)`; riscv64 `arch::write_tp(0)` — each under its `CONFIG_ARCH_*` guard. Rationale: exec reuses the live TCB; without reset+clear the old image's base survives into the new image (auditor S2).
4. `syscall_errors.hpp`: append `X(TLS_INVALID_BASE, 109, "Invalid TLS base address")` after the task band (ends 108, :44); confirm no error-count static_assert needs updating (check `error_string` coverage test / count asserts).
5. `msr_impl.hpp` (x86_64): add `inline constexpr uint32_t MSR_FS_BASE = 0xC0000100;` beside `MSR_GS_BASE`, same style.
6. `aarch64/hal/io_impl.hpp`: add `inline void write_tpidr_el0(uint64_t v) { asm volatile("msr tpidr_el0, %0" : : "r"(v) : "memory"); isb(); }` beside `write_ttbr0_el1` (:251–255). `riscv64/hal/io_impl.hpp`: add `inline void write_tp(uint64_t v) { asm volatile("mv tp, %0" : : "r"(v) : "memory"); }` beside satp helpers (:167–195).
7. Publish slot: `global_state.{hpp,cpp}` define `extern "C" uint64_t scheduler_load_tls_from[CONFIG_MAX_CPUS]` beside `scheduler_load_cr3_from` (mirror scheduler.hpp:1051–1057 decl block incl. the NOLINTNEXTLINE comment); `SwSlots::load_tls_from()` accessor mirroring `load_cr3_from()` (:870–871, `this_cpu()` index); extend `capture_percpu`/`restore_percpu` (:4071/:4100), `cancel_pending_switch_cpu` (:605), BSP clear (:4192), `clear_switch_globals` (:4209), and EVERY other `load_cr3_from` clear (:947,1802,1827,1984,2013,3810,4674) — each new clear directly beside its CR3 clear, same `__ATOMIC_RELEASE`.
8. New `syscall_handlers_tls.cpp`: `uint64_t Syscall::sys_tls_set(arg0=base, …)`: (a) `arg0==0` → `set_tls_base_err(*current, 0)` path (still validates current exists); (b) arch gate per INVARIANTS (x86: `is_canonical()` + LIMIT; aarch64: LIMIT (+ `is_canonical()` if generic); riscv64: `(arg0>>39)==0` + LIMIT); fail → `(uint64_t)-SYS_ERR_TLS_INVALID_BASE`; (c) null-current check FIRST (before validation? NO — validate first is wrong if no current; order: fetch current, null → `(uint64_t)-SYS_ERR_SCHED_NO_CURRENT`, then validate, then `set_tls_base_err`); (d) map `errors::SchedulerError::NO_CURRENT` → `(uint64_t)-SYS_ERR_SCHED_NO_CURRENT`, `OK` → 0. No user-memory dereference (scalar only, no CheckedPtr), no blocking, no reschedule. Declare `sys_tls_set` in `syscall.hpp` beside `sys_abi_version`; table entry 85 `sys_unimplemented // 85 TLS_SET` → `&Syscall::sys_tls_set`; do NOT touch `k_syscall_fast[]` (FAST = audited pointer-free list, fastpath §0/§3).
9. New `Scheduler::set_tls_base_err(TaskControlBlock &task, uint64_t base)` (decl scheduler.hpp:240 beside `set_affinity_err`, same `errors::SchedulerError … noexcept` signature): `arch::IrqGuard irq_guard{};` + `SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);` (mirrors set_affinity_err :655–657; add `quiesce_enter()` ONLY if the affinity quiesce rationale — AP current-apply racing — applies to this scalar store; default OMIT with one-line justification, AP apply reads the per-CPU slot atomically, not the TCB); re-fetch `current_task()`, null → `NO_CURRENT`; store `task.tls_base_ = base`; release lock (guard scope ends); then, if `&task == Scheduler::current_task()` (:414 precedent — the write affects only THIS cpu's register, so identity with the running task is the exact gate, not the any-cpu scan), live-load per arch (wrmsr / write_tpidr_el0 / write_tp; base 0 clears). Returns `errors::SchedulerError::OK`.
10. `scheduler.cpp switch_to_task` (~3599–3635): after the CR3 publish if/else (:3606/:3622), insert in BOTH branches `__atomic_store_n(&SwSlots::load_tls_from(), (next.is_user_ ? next.tls_base_ : 0), __ATOMIC_RELEASE);` before `arch::iopb_switch_to(next)` (:3635), under caller lock/IRQ-off. PCID tagging (#156) applies to CR3 only — TLS value published raw. `switch_away_from_terminating` second site: after the CR3 if/else (:3987/:3990), insert the same two-branch publish before the READY re-enqueue (:3994).
11. `isr_stubs.asm` x86_64: after :425 (`mov qword [rax + r11*8], 0` CR3 clear), before `jmp .restore` (:426), insert:
```
    lea rax, [rel scheduler_load_tls_from]
    mov rax, [rax + r11*8]
    test rax, rax
    jz .tls_done
    mov rcx, rax          ; save value; rax reused below (scratch-safe per INVARIANTS)
    lea rax, [rel scheduler_load_tls_from]
    mov qword [rax + r11*8], 0
    mov rdx, rcx
    shr rdx, 32
    mov eax, ecx
    mov ecx, 0xC0000100  ; MSR_FS_BASE
    wrmsr
    jmp .restore
.tls_done:
    lea rax, [rel scheduler_load_tls_from]
    mov qword [rax + r11*8], 0
    jmp .restore
```
    (Consume-unconditionally: slot cleared on both paths; WRMSR only if nonzero. rax/rcx/rdx scratch-safe — `.restore` pops all GPRs :473–487; r11 untouched; no calls.) `.abort_switch`: beside :434–438 clears add `lea rax, [rel scheduler_load_tls_from]` / `mov qword [rax + r11*8], 0`.
12. `vectors.S` aarch64 (CONFIG_ARCH_AARCH64): retarget :239 `cbz x3, .restore` → `cbz x3, .tls_apply`; insert `.tls_apply` between :248 CR3 clear and :249 `isb` (the isb is the SHARED join point — never skipped on any path):
```
.tls_apply:
    adrp    x3, scheduler_load_tls_from
    ldr     x3, [x3, #:lo12:scheduler_load_tls_from]
    cbz     x3, 8f
    msr     tpidr_el0, x3
    adrp    x4, scheduler_load_tls_from
    str     xzr, [x4, #:lo12:scheduler_load_tls_from]
8:  isb                     // was :249; pairs with the TTBR0 switch+TLBI above AND the TPIDR_EL0 write here
.restore:
```
    (Fixes re-audit S2: the v2 layout branched TLS-zero straight to `.restore`, stranding a TTBR0 switch + TLBI with no isb. x3/x4 reuse licensed by :232–233 + CR3 precedent :237–248; non-indexed base = [0] exact single-core.)
13. `syscall_entry.S` riscv64 (CONFIG_ARCH_RISCV64): after the satp block (:203–210), before `7:` (:212), insert:
```
    # Consume TLS slot into the trap-frame TP save area (live tp is
    # reloaded from OFF_TP at :227, so a direct tp write here would die)
    la t0, scheduler_load_tls_from
    ld t1, 0(t0)
    sd zero, 0(t0)
    beqz t1, 8f
    sd t1, OFF_TP(sp)
8:
```
    (Label `8`/`8f` — verified free: file uses only 1,5,6,7 as numeric labels. t0/t1 scratch-safe — used as temporaries at :204–216; unconditional consume; non-indexed base = [0] exact single-core per scheduler.hpp:1047–1048; `sfence.vma` NOT needed for tp.)
14. `scheduler.hpp TaskFields`: add `uint64_t tls_base;` after `iopb_slot` (:608); `capture_task_fields` add beside :4265; `restore_task_fields` add beside :4353 (same ID/position match scope, harness-RSP rule untouched).
15. `syscall.h`: verify `SYS_TLS_SET 85` + `SYS_MAX 86` present; add `sys_tls_set` inline wrapper only if absent.
16. `make build` Errors 0 (80-col, `#pragma once`, init-lists, no `new/delete`, no `const_cast`, `arch::pause()` in spins).

## TEST STRATEGY
- New class `tls` (`src/kernel/test/test_tls.cpp`, JARVIS_TEST + REGISTER, `test_expected_counts.hpp` row, registry regen):
  1. `tls_set_zero_clears` — TLS_SET(0) → 0, `tls_base_==0`, live FS_BASE/TPIDR/tp == 0 on return (arch-guarded readback: x86 RDMSR FS_BASE, aarch64 `mrs TPIDR_EL0`, riscv `mv` from tp).
  2. `tls_set_rejects_noncanonical` — x86 `0x0000800000000000` → `SYS_ERR_TLS_INVALID_BASE`, stored unchanged; riscv64 `0x4000000000` (bit 39, passes LIMIT, fails Sv39 canonical) → same code (arch-specific vectors per LIMIT table).
  3. `tls_set_rejects_kernel_half` — `0xFFFF800000000000` → dedicated code on all archs.
  4. `tls_set_accepts_user_half` — `0x00007FFF00000000` (x86; arch-scaled per-LIMIT values elsewhere) → 0 + readback + live-register equals base (self-apply proof).
  5. `tls_clone_inherits` — child `tls_base_==parent`.
  6. `tls_snapshot_parity` — nonzero survives `snapshot_create/restore` (mirrors iopb_slot parity test).
  7. `tls_not_fast` — 85 ∉ `SYSCALL_FAST_MASK`, `handle(85,…)` takes FULL path.
  8. `tls_publish_applies` — implemented via REAL dispatch (reschedule() only sets need_resched; arming is tick-side): synthetic-user task observes its live base after genuine site-1 dispatch; kernel task with rigged nonzero field leaves the live register untouched (GS_BASE unchanged).
  9. `tls_preemption_ping_pong_errno` (acceptance) — two TLS-distinct tasks loop TLS_SET(self)+YIELD over N preemptions, never observe peer's base.
  10. `tls_exec_clears` — DROPPED at implementation: `exec_into_current` operates on the RUNNING task (driving it in-test would replace the harness image). The reset is audit-verified; its primitives (field store + live-register clear) are covered by tests 1/4.
  11. `tls_cancel_clears_slot` — each x86 cancel path (:947,1802,1827,1984,2013,3810,4674) leaves `load_tls_from[cpu]==0` (unit-drive the cancel helpers or assert post-abort slot state).
  12. `tls_second_publish_site` — drive `switch_away_from_terminating` tick path, assert TLS published alongside CR3.
  13. `tls_no_stale_slot_after_cancel` — arch-portable (compiles/runs on all three archs): publish a nonzero slot via the C++ `SwSlots` accessor, invoke the arch-independent clear helpers (`clear_switch_globals`/`cancel_pending_switch_cpu` equivalents), assert slot==0. Pins the publish+arm atomicity invariant on non-x86 builds too (live aarch64/riscv64 epilogue coverage awaits real-arch test execution, #28 family — documented gap, not a silent hole).
- Implemented count: 12 tests (`tls_exec_clears` dropped per above), `{"tls", 12, 12, 12}`.
- Test 8 implemented via public `reschedule()` arming (the test-local `switch_to_task` is file-static in scheduler.cpp, not a Scheduler member); test 9/11 use synthetic-user tasks (lambda cannot run as true userspace — test_ipc_blocking.cpp constraint), full ring-3 acceptance defers to #75.
- Negative pins: `-4095` errno boundary intact; `handle(MAX_SYSCALL)` → -1; `static_assert(table size==MAX_SYSCALL==86)`.
- Regressions (targeted, NOT test-full): `tls`, `syscall`, `syscall_abi` (bands incl. 85), `smp`, `core`, `arch_cross`, `selftest`; then debug `all` + release `all`; `test-history.txt` row per run.
- ResourceTracker: zero delta via `test_isolate` snapshot_restore on every new test (no `track_*` added).

## SIL 3 / SAFETY RISKS
- S1 — Arbitrary FS/TPIDR/tp base as privilege hole: closed by per-arch canonical + user-half gates (x86 48-bit + 47-bit LIMIT; aarch64 LIMIT-subsumed; riscv64 explicit `>>39` + 40-bit LIMIT), dedicated fail-closed code, 0 = unset only; no kernel pointer returnable.
- S1 — GS/swapgs corruption: FS_BASE-only on x86 (MSR 0xC0000100; GS_BASE/KERNEL_GS_BASE/swapgs untouched); aarch64 touches TPIDR_EL0 only (TTBR1 kernel window, EL1 state untouched); riscv writes tp/OFF_TP only (satp/sstatus/sepc untouched); GS test pins x86.
- S2 — Wrong-thread TLS: publish-alongside-CR3 at BOTH publish sites + apply-before-return on all three archs incl. abort-clear (x86) / consume-unconditional (riscv) / retargeted-cbz (aarch64); kernel tasks publish 0; ping-pong + publish tests pin it.
- S2 — Stale base across exec: exec reset + live clear (step 3).
- S2 — Setter-on-stale-register (§13 crt0): live-apply-after-unlock in `set_tls_base_err` (step 9).
- S2 — Lock across switch (§11.1): scheduler lock held for scalar store only; live-apply + epilogue-apply lock-free.
- S3 — aarch64 no-abort/skip paths: relied-upon publish+arm atomicity invariant stated + x86 cancel tests + arch-portable `tls_no_stale_slot_after_cancel` (test 13); live-epilogue coverage on real aarch64/riscv64 deferred to #28-family execution (documented, not silent).
- S3 — TCB memset amnesia (LEARNINGS.md:134): 4 sites + ctor + parity + inherit + exec tests.
- S3 — Debug/release divergence (§6): no CONFIG_DEBUG-gated control flow in store/publish/apply (trace only).
- S3 — LA57: kernel has zero LA57 support tree-wide (verified); 48-bit gate exact. If LA57 ever lands, the gate must widen — recorded here as the trigger condition.

## OPEN QUESTIONS
- `set_tls_base_err`: include `quiesce_enter()` (set_affinity precedent :653–657) or omit? DEFAULT: omit with one-line justification (AP apply path reads the per-CPU slot atomically, never the TCB; no quiesce window needed for a scalar store). Auditor to confirm.
- Spec follow-up (non-blocking, post-#74): `docs/specs/syscall-abi-picolibc.md` §7 still mandates an LA57 live-mode query; the plan's static 48-bit gate is exact only because LA57 has zero tree-wide support. File a docs tweak to §7 (or fold into the LA57-landing issue as a trigger condition) — NOT part of this implementation.
- none other
