# PLAN: #609 item 4b — retire per-counter sched oracles, port call sites to the single `SchedOracles` family

## AFFECTED FILES
- `src/kernel/task/scheduler.hpp` — delete the eleven public per-counter getters (`runq_link_flag_desync_count`, `runq_desync_forced_clear_count`, `restore_drop_rebuilt_count`, `rqm_enqueue_refused_already_count`, `rqm_enqueue_refused_invalid_count` at L367-376; `stuck_rescue_count`, `orphan_rescue_count`, `runq_nowhere_linked_count`, `edf_orphan_rescue_count`, `edf_chain_drop_count` at L515-548; private `ipc_wake_rescue_count` at L1678) and the seven per-counter resets (`reset_stuck…`, `reset_orphan…`, `reset_runq_nowhere_linked…`, `reset_edf_orphan…`, `reset_edf_chain_drop…` at L517-548, `runq_desync_reset_for_test` at L384, `reset_ipc_wake_rescue_count_for_test` at L1676); rewrite the twelve member doc blocks (L1608-1657, L1669-1673) to name the family as the readout/reset. Keeps every counter member and every `note_*`/`Slots` writer.
- `src/kernel/task/scheduler_registry.cpp` — reimplement `read_sched_oracles()` as eleven direct `__ATOMIC_RELAXED` loads and `reset_sched_oracles_for_test()` as eleven direct `__ATOMIC_RELAXED` stores (both currently compose the accessors/resets that this change deletes); delete the eighteen accessor/reset definitions at L350-408, L430-436, L451-457. Counter definitions (L87-102) and all `note_*` writers (L410-428) stay byte-identical.
- `src/kernel/test/test_scheduler.cpp` — port 3 reset calls (L659, L714, L761) and 4 read expressions (L673, L685, L739, L781) in `scheduler_stranded_ready_rescued`, `scheduler_evict_current_then_rescued`, `scheduler_evict_queued_current_drops_safely`; refresh the 3 `Depends:` lines that name `stuck_rescue_count`. The two `#609` stubs and their registrations are untouched.
- `src/kernel/test/test_park_lock.cpp` — port 2 tests (`lock_park_at0_contention` L662-663/L702-703/L727-728, `lock_park_at0_arm_block_wake_retry` L754-755/L798-799/L816-817): 4 reset calls → 2 family resets, 8 read expressions → fields; `%lu` Logger argument order preserved.
- `src/kernel/test/test_resched_park.cpp` — port 3 tests (`resched_park_at0_woken_return` L468-469/L506-507/L526-527, `resched_park_at0_gate_negative` L549-550/L588-589, `resched_park_at0_gate_bits_premise` L611-612/L634-635/L644-645): 6 resets → 3 family resets, 10 reads → fields.
- `src/kernel/test/test_sync.cpp` — `sync_notify_latch_no_rescue` L629/L632: before/after `stuck_rescue` equality via the family read (no reset — semantics must stay rewind-free).
- `src/kernel/test/test_cap_tcb.cpp` — `tcb_suspend_absorb_backstop_refuses_link` L387/L391: `flagged_orphan_rescue` via the family read (note: not `edf_orphan_rescue`); doc lines L344-345 refreshed.
- `docs/specs/scheduler.md` — one-line note on the INV-RQ-3 row (L86) that the counter family's single readout is `read_sched_oracles()` / `reset_sched_oracles_for_test()`; no counter semantics change.
- NOT touched, deliberately: `src/kernel/test/test_ipc_blocking.cpp` (stub prose names the removed private accessor — QE-owned), `src/kernel/test/test_expected_counts.hpp` (no test added/removed ⇒ rows unchanged), `src/kernel/test/test_isolate.cpp`, and the adjacent families `scheduler_apply_gate_reset_for_test`, `lockpark_reset_for_test`, `admin_stats_reset_for_test` (#557), `reset_migration_counts` (#61), `reset_starvation_breaker` (#598).

## INVARIANTS & CONCURRENCY BOUNDARIES
- **INV-ORA-1 — one readout, one reset.** After the change no per-counter public entry point remains.
- **INV-ORA-2 — API-only deletion (zero behaviour change).**
- **INV-ORA-3 — lock discipline unchanged.**
- **INV-ORA-4 — reset is a test seam only.**
- **INV-ORA-5 — monotonic-no-rewind preserved.**
- **INV-ORA-6 — exact field mapping.**
- **INV-ORA-7 — no detection blindness.**
- **INV-ORA-8 — test sanctity scope.**
- **INV-ORA-9 — step-green.**
- **INV-ORA-10 — accounting unaffected.**
- **Cross-arch (mandatory):** arch-neutral change

## STEP-BY-STEP CHANGES
1. Precondition gate (no code edits; record on #609). Confirm ancestry and shape.
2. `src/kernel/test/test_scheduler.cpp`: port 3 reset calls and 4 read expressions.
3. `src/kernel/test/test_park_lock.cpp`: port 2 tests.
4. `src/kernel/test/test_resched_park.cpp`: port 3 tests.
5. `src/kernel/test/test_sync.cpp` + `src/kernel/test/test_cap_tcb.cpp`: port.
6. `src/kernel/task/scheduler_registry.cpp`: rewrite `read_sched_oracles()`.
7. `src/kernel/task/scheduler_registry.cpp`: rewrite `reset_sched_oracles_for_test()`.
8. `src/kernel/task/scheduler_registry.cpp`: delete eleven getter and seven reset definitions.
9. `src/kernel/task/scheduler.hpp`: delete 18 declarations and rewrite doc blocks.
10. Static verification (no code change).
11. Dynamic verification + evidence.

## TEST STRATEGY
- New tests: none
- Existing classes: `scheduler_core`, `synchronization_lock_order`, `cap_tcb`, `ipc`, `selftest`, `all`, 3-arch build+link

## SIL 3 / SAFETY RISKS
- [S1] Detection blindness by reset widening
- [S1] Wrong-field mapping
- [S2] Family reset implemented with fewer than eleven stores
- [S2] Plain read/write race
- [S2] Assertion drift
- [S2] Deleting `runq_desync_reset_for_test()` while caller survives
- [S3] Stale prose

## OPEN QUESTIONS
- Base-state blocker: HEAD 84dbfbada contains no `SchedOracles` family — C4 not reachable, hold.
