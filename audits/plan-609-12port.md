# PLAN: #609 QE follow-up — port the 3 SchedOracles stubs to the 12-counter family

Lane: QE / branch `testbed` (edits under `src/kernel/test/` only; zero production changes).
Tip at planning time: `testbed` =`2b28dce14` (#601 12th oracle merged via `358c0d707`; main `0e7c15801`).
Spec: `docs/specs/schedoracles.md` (12 members, INV-ORA-1..10). API: `src/kernel/task/scheduler.hpp:508-527`.

## Objective
1. Replace the three `JARVIS_TEST_PASS()` stubs with the real assertions their doc-block pseudocode already specifies, ported to the **12-field** `SchedOracles` family (`edf_unreachable` added by #601).
2. Unblock #604 item 4b evidence chain: family readout/reset is proven at 12 fields, positive-firing lands in the expected field only, healthy-window deltas are zero, and the IPC wake-rescue sweep stays silent.
3. Keep registration/counts/wiring byte-stable: no test added or removed, no class wiring change, no `test_isolate.cpp` change.

## Retrieval artifacts (mandatory, done)
- `graphify query_graph("Scheduler read_sched_oracles reset_sched_oracles_for_test SchedOracles oracle family test assertions")` — **used**: confirmed existing family consumers `test_sync.cpp:629-632` (before/after equality, no reset), `test_park_lock.cpp:662/753` (reset, lock_order class), `test_cap_tcb.cpp:387-391` (delta on `flagged_orphan_rescue`). This is what forces delta-style gates over absolute ones.
- `obsidian_search_vault "schedoracles …" / "609 SchedOracles audit"` — **0 matches** (vault has no SchedOracles note); disposition: spec taken from in-repo `docs/specs/schedoracles.md` + audit `audits/report-2026-11-25T00-00-00Z-601-602.md`.

## Files to touch
1. `src/kernel/test/test_scheduler.cpp`
   - anonymous namespace (near `WaitpidTestContext`, L35-87): add the 12-slot flatten helper + structural static_assert (see below).
   - `sched_oracle_family_single_reset` (L854-883): doc-block "eleven" → "twelve", drop "(stub)", real body.
   - `sched_oracle_must_read_zero` (L885-914): same, real body; registrations L939-940 comments only (stub → 12-oracle gate).
2. `src/kernel/test/test_ipc_blocking.cpp`
   - `ipc_wake_rescue_must_be_zero` (L284-307): doc-block "(stub)"/"11-field" → twelve/real; real body; inline structural static_assert.
3. `src/kernel/test/test_expected_counts.hpp` — comment refresh only, counts unchanged
   (L53 `scheduler_core 41`, L308 `core_sched 133`, L313 `ipc 107`: "… stubs x2" → "… 12-oracle family/must-zero gates x2").
4. `audits/plan-609-12port.md` (this file) + a QE follow-up GitHub issue comment/issue referencing closed #609 (work anchoring; no code impact).

Not touched: `test_registry.cpp` (wiring already correct: `ipc_blocking` L872-876 and `ipc` aggregate L1238 place the wake-rescue gate LAST), `test_isolate.cpp`, any `src/kernel/**` production file, `docs/specs/schedoracles.md`.

## Shared helper (test_scheduler.cpp, anonymous namespace)
```cpp
/// #601 added edf_unreachable as the twelfth family member.
constexpr uint64_t SCHED_ORACLE_COUNT = 12;

struct SchedOracleSnapshot {
    uint64_t values[SCHED_ORACLE_COUNT];
};

/// Structural proof that the single readout exposes all twelve counters:
/// the member list below fails to compile if a field is renamed/dropped,
/// and the static_assert fails if the POD grows a 13th slot.
static_assert(sizeof(kernel::Scheduler::SchedOracles) ==
                  SCHED_ORACLE_COUNT * sizeof(uint64_t),
              "SchedOracles must stay 12 x uint64_t (#601)");

inline SchedOracleSnapshot flatten_sched_oracles() noexcept {
    const kernel::Scheduler::SchedOracles family =
        kernel::Scheduler::read_sched_oracles();
    SchedOracleSnapshot flat{};
    flat.values[0] = family.stuck_rescue;
    flat.values[1] = family.flagged_orphan_rescue;
    flat.values[2] = family.edf_orphan_rescue;
    flat.values[3] = family.runq_nowhere_linked;
    flat.values[4] = family.edf_chain_drop;
    flat.values[5] = family.link_flag_desync;
    flat.values[6] = family.desync_forced_clear;
    flat.values[7] = family.restore_drop_rebuilt;
    flat.values[8] = family.enqueue_refused_already;
    flat.values[9] = family.enqueue_refused_invalid;
    flat.values[10] = family.ipc_wake_rescue;
    flat.values[11] = family.edf_unreachable;
    return flat;
}
```
Slot order == declared field order == spec §1 table order (INV-ORA-6). Diagnostic messages use the numeric slot (`"oracle slot %lu …"`), never `%s` (no dependence on `Logger::vprint_raw` string support).

## Assertion design

### A. `sched_oracle_family_single_reset` (core_sched)
Purpose: one readout returns 12, one reset zeroes 12, a production firing lands in exactly its own field.
1. `TaskPtr rescued = create_test_task(5, 0);` assert non-null (outside the guard).
2. `{ arch::IrqGuard irq_guard;` — **reset moved inside the guard** (pseudocode line order kept semantically; deviation is deliberate and required): with IF=0 no tick can fire a counter between `reset` and the post-reset read, so the *absolute* "all twelve are 0" and "`stuck_rescue == 1`" assertions become deterministic instead of racy.
3. `Scheduler::reset_sched_oracles_for_test();`
4. `const SchedOracleSnapshot zeroed = flatten_sched_oracles();` loop `slot = 0..11`: `JARVIS_ASSERT_FMT(zeroed.values[slot] == 0, "family reset left oracle slot %lu = %lu", slot, zeroed.values[slot]);`
5. Fixture (verbatim shape of `scheduler_stranded_ready_rescued` L656-693): `rescued->state = TaskState::READY;` assert `!in_ready_queue_`, `!in_edf_queue_`, `rq_own().peek_highest() != rescued.get()` (blindness proof).
6. `Scheduler::on_tick();` (stranded-READY sweep; the #582/#249 production rescue path).
7. `const SchedOracleSnapshot fired = flatten_sched_oracles();`
8. `Scheduler::dequeue_ready(*rescued);` — **before** the numeric asserts, so an early-return failure cannot leave a READY task linked (TaskPtr RAII is the second net: `remove_task` → `dequeue_ready`, scheduler_registry.cpp:759).
9. Assert `fired.values[0] == 1` (slot 0 = `stuck_rescue`) with FMT message; assert dispatchability (`in_ready_queue_ || SwSlots::next_task_id() == rescued->id`) and state `READY || RUNNING`.
10. Loop `slot = 1..11`: `JARVIS_ASSERT_FMT(fired.values[slot] == 0, "cross-talk: oracle slot %lu fired = %lu", slot, …)` — the field-isolation half of INV-ORA-6.
11. Close guard; `JARVIS_TEST_PASS();` (keep — matches every real test in this file; `test_structure` is warning-severity).

### B. `sched_oracle_must_read_zero` (core_sched)
Purpose: aggregate delta-zero gate over a bench-shaped relay/soak window on a healthy scheduler. **No reset in this test** (rewind-free evidence, INV-ORA-5).
1. `auto *sched_harness = Scheduler::current_task();` assert non-null.
2. `const SchedOracleSnapshot before = flatten_sched_oracles();`
3. Relay fixtures, exact `scheduler_breaker_lower_progress` shape (L806-819): `TaskControlBlock *relay_near = nullptr; TaskControlBlock *relay_far = nullptr;` then `auto relay_teardown = ScopeGuard([&]() { kernel::test::terminate_and_drain2(relay_near, relay_far); });` registered **before** creation (null-safe `terminate_if_live`), then `create(forever_entry, sched_harness->priority + 1, 10)` / `+2`, `set_sched_policy(…, SchedPolicy::FIXED)` on both, then `{ arch::IrqGuard guard; Scheduler::add_task(*relay_near); Scheduler::add_task(*relay_far); }`.
4. Relay: `constexpr uint64_t RELAY_TRIPS = 16;` `for (trip = 0; trip < RELAY_TRIPS; ++trip) { kernel::test::yield_to_task(*relay_near); kernel::test::yield_to_task(*relay_far); Scheduler::on_tick(); }`
5. Soak: `constexpr uint64_t SOAK_CYCLES = 32;` `for (cycle = 0; cycle < SOAK_CYCLES; ++cycle) { TaskPtr churn = create_test_task(5, 10); JARVIS_ASSERT(churn.get() != nullptr); Scheduler::on_tick(); }` (TaskPtr dtor does register/unregister churn per cycle).
6. `const SchedOracleSnapshot after = flatten_sched_oracles();`
7. Delta loop `slot = 0..11`: `JARVIS_ASSERT_FMT(after.values[slot] == before.values[slot], "oracle slot %lu drifted over healthy window: before=%lu after=%lu", …)` — per-field delta only, never cross-field ordering (spec §2: the POD is a non-atomic multi-field snapshot).
8. No `dismiss()`: teardown fires at scope exit on every path (CODING_STYLE §7.1) — this replaces the pseudocode's "cleanup before final assert" while guaranteeing no leak on an early return.
9. `JARVIS_TEST_PASS();`

### C. `ipc_wake_rescue_must_be_zero` (ipc / ipc_blocking)
Purpose: level-triggered IPC-wake sweep (scheduler_tick.cpp:692-740) is detection-first; any firing is a lost wakeup on a plain-recv channel. Cumulative, **no reset**.
1. Inline `static_assert(sizeof(kernel::Scheduler::SchedOracles) == 12 * sizeof(uint64_t), "…");` (file self-contained; no cross-file test helper).
2. `const kernel::Scheduler::SchedOracles family = kernel::Scheduler::read_sched_oracles();`
3. `JARVIS_ASSERT_EQ(0ULL, family.ipc_wake_rescue);`
4. `JARVIS_TEST_PASS();`
Cumulative-by-design rationale in the doc-block: per-class QEMU boot means the counter reflects only this class; the gate is registered LAST in both the `ipc_blocking` fine class and the `ipc` aggregate (test_registry.cpp:876, :1238) — that ordering must not change.

## Risks
- **Monotonic counters.** `snapshot_restore` resets only migration counts + lockpark (test_isolate.cpp:987-988); oracle counters are never rewound (INV-ORA-5). Any absolute assertion outside a reset-scoped IRQ-off window is invalid → A uses guard-scoped absolute values, B is delta-only, C is one cumulative field.
- **Reset blindness.** A's reset zeroes all 12 mid-class; in a single-boot `all`/`selftest` run it narrows C's cumulative window to post-reset firings (stricter, never weaker). Accepted per spec §4 INV-ORA-7; reset must not be added to `ipc`/`ipc_blocking`/`selftest`.
- **Stranded-ready isolation.** Fixture left READY+queued on early return → A dequeues before the numeric asserts and relies on `TaskPtr` → `remove_task` → `dequeue_ready` (scheduler_registry.cpp:759).
- **Relay leak.** Raw `TaskControlBlock::create` fixtures are not RAII → `ScopeGuard` teardown registered before creation, no dismissal.
- **Relay starvation/timing.** Two FOREVER spinners ≥1 band above the harness starve it; progress depends on the #242 breaker (`kStarvationBreakerTicks = 20`) → worst case ≈ 16 × 20 = 320 ticks plus soak. This is the dominant runtime risk and also the dominant exposure to the known H2 deferred-switch flake; K=16/M=32 are the spec'd window and must not be silently shrunk — a TIMEOUT is evidence to report.
- **`edf_unreachable` coverage is zero-delta only.** The 12th counter has no positive-firing test; the audit's own single-slot pending livelock (audits/report-2026-11-25T00-00-00Z-601-602.md, scheduler_queue.cpp:306-312) means a healthy zero does not prove the audit is live. Document in the doc-block; a corruption-seam test needs production changes → out of QE scope.
- **Style.** Test dir is excluded from kernel-only ERROR checkers; `test_structure` (stub+asserts) is warning-severity — keeping `JARVIS_TEST_PASS()` adds 3 warnings and 0 errors. `descriptive_names` min_length 3: no `o`/`f`/`a`/`b` locals. Loops bounded by constexpr (REL/soak counts).

## Verification
1. Remote station usability gate (PROMPT-testdev §2c) — ask before the first remote command; station `nexios-qe` (192.168.20.52), container `qe-station`, workdir `/work/nexios`.
2. Sync the container checkout to the recorded `testbed` tip (fetch + ff-only; record `git log --oneline -1` with every gate).
3. Style: `python3 tools/validate_style.py` → **Errors: 0** (no new errors; +3 `test_structure` warnings expected and accepted) and `make check-style`.
4. Builds (3-arch mandate, debug): `tools/meson/build.sh --arch x86_64` / `--arch aarch64` / `--arch riscv64` — all green, no new warnings in the two test TUs.
5. `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class core_sched` → **133/133**.
6. `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class ipc` → **107/107**.
7. `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class selftest` → **165/165** (count-table row says 157 — if a count warning appears, capture `--class dump-counts` before touching the table).
8. `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class all` (full debug registry) — confirming no cross-class interference from A's reset.
9. Append one `test-history.txt` row per class run (mandatory), post results + tip + audit-free note on the follow-up issue; close only at 0 failures on steps 5-7.
