# SchedOracles — Ready-Queue & Orphan Rescue Violation Oracles

**Semantics:** single-family readout/reset discipline for the scheduler's violation oracles. Replaces twelve per-counter accessors/resets with one lock-free snapshot and one test-seam reset. All counters are monotonic, never rewound by `snapshot_restore`, and must read `0` in a healthy run.

## 1. Members (12 counters)

| Field | Counter member | Owning issue | Counts |
|---|---|---|---|
| `stuck_rescue` | `stuck_rescue_count_` | #582 | tick sweep rescued a `BLOCKED` task that was still `in_ready_queue_` |
| `flagged_orphan_rescue` | `orphan_rescue_count_` | #582 | `reap_orphans` / `drain` rescued a `READY` task whose `parent_id` is dead but `in_ready_queue_` was set |
| `edf_orphan_rescue` | `edf_orphan_rescue_count_` | #600 | `edf_orphan_rescue` rescued an EDF-orphaned task |
| `runq_nowhere_linked` | `runq_nowhere_linked_count_` | #600 | `runq_link_flag_desync` diagnostic: `READY` + `in_ready_queue_` but nowhere linked |
| `edf_chain_drop` | `edf_chain_drop_count_` | #600 | EDF chain head force-dropped (non-membership-marked or cycle) |
| `link_flag_desync` | `runq_link_flag_desync_count_` | #582 | `runq_link_flag_desync` detected |
| `desync_forced_clear` | `runq_desync_forced_clear_count_` | #582 | desync forced clear |
| `restore_drop_rebuilt` | `restore_drop_rebuilt_count_` | #582 | restore drop rebuilt |
| `enqueue_refused_already` | `rqm_enqueue_refused_already_count_` | #582 | `enqueue_ready` refused `AlreadyQueued` |
| `enqueue_refused_invalid` | `rqm_enqueue_refused_invalid_count_` | #582 | `enqueue_ready` refused `RefusedInvalid` |
| `ipc_wake_rescue` | `ipc_wake_rescue_count_` | #557 | IPC wake rescue |
| `edf_unreachable` | `edf_unreachable_count_` | #601 | confirmed EDF member unreachable from its CPU head (2-pass + next-pass confirm) |

All members are `uint64_t`, `__atomic` writers (`note_*` / `Slots`), `__atomic_RELAXED` readers. No plain reads/writes.

## 2. Readout — `read_sched_oracles()`

```cpp
struct SchedOracles {
    uint64_t stuck_rescue;
    uint64_t flagged_orphan_rescue;
    uint64_t edf_orphan_rescue;
    uint64_t runq_nowhere_linked;
    uint64_t edf_chain_drop;
    uint64_t link_flag_desync;
    uint64_t desync_forced_clear;
    uint64_t restore_drop_rebuilt;
    uint64_t enqueue_refused_already;
    uint64_t enqueue_refused_invalid;
    uint64_t ipc_wake_rescue;
    uint64_t edf_unreachable;
};
static SchedOracles read_sched_oracles() noexcept;
```

- Lock-free: twelve `__atomic_load_n(..., __ATOMIC_RELAXED)` in field order, no `scheduler_lock_`, no `IrqGuard`.
- Test/diagnostic only, never from tick/ISR production.
- Returned POD is a **non-atomic multi-field snapshot** — callers use per-field deltas only, never cross-field ordering (a tick may fire between loads).

## 3. Reset — `reset_sched_oracles_for_test()`

```cpp
static void reset_sched_oracles_for_test() noexcept;
```

- Twelve `__atomic_store_n(..., 0ULL, __ATOMIC_RELAXED)` in field order.
- Test seam only, never reachable from tick/ISR/production.
- The family's **only rewind**; `snapshot_restore` leaves counters untouched (monotonic-no-rewind, INV-ORA-5).

Per-counter `reset_*_for_test()` are deleted — this is the single reset.

## 4. Invariants

- **INV-ORA-1 — one readout, one reset.** After #609 item 4b, no per-counter `*_count()` / `reset_*_for_test()` entry point remains.
- **INV-ORA-2 — API-only deletion.** No counter storage, write site, atomic order, tick/dispatch/queue-path edit.
- **INV-ORA-3 — lock discipline.** Readout is lock-free, `__ATOMIC_RELAXED` only, per CODING_STYLE §11.6.
- **INV-ORA-4 — reset is test seam only.** Seven old resets had zero production callers.
- **INV-ORA-5 — monotonic-no-rewind.** Counters never rewound by `snapshot_restore`.
- **INV-ORA-6 — exact field mapping.** See table above; e.g. `stuck_rescue_count` → `stuck_rescue`, never `edf_orphan_rescue`.
- **INV-ORA-7 — no detection blindness.** Family reset zeroes all twelve, so calls exist only in `scheduler_core`, `synchronization_lock_order`, `cap_tcb` — never in `ipc`/`ipc_blocking`/`selftest` where `ipc_wake_rescue_must_be_zero` is cumulative.
- **INV-ORA-8 — test sanctity scope.** Only reset lines, read expressions, `Depends:` prose may change; no assertion/fixture/priority/registration change.
- **INV-ORA-9 — step-green.** Test-file ports precede production deletion; family rewritten before accessors deleted.
- **INV-ORA-10 — accounting unaffected.** No allocation, no new TU, no `gen.mk` change.

## 5. Usage — must-read-0 gates

- `sched_oracle_family_single_reset` (`core_sched`): family read returns 12 counters, reset zeroes all 12, production firing lands in expected field.
- `sched_oracle_must_read_zero` (`core_sched`): aggregate must-read-0 over bench relay/soak window — all 12 deltas zero.
- `ipc_wake_rescue_must_be_zero` (`ipc`): `ipc_wake_rescue` stays 0 over `ipc` + `ipc_blocking`.

After #609, every oracle assertion uses `read_sched_oracles().<field>` and every reset uses `reset_sched_oracles_for_test()`.

## 6. Cross-arch & Build

Arch-neutral (no `src/kernel/arch/**` change), no arch-visible behaviour. All three arches (`x86_64`, `aarch64`, `riscv64`) build+link green in debug and release. Adjacent families (`admin_stats`, `migration_counts`, `starvation_breaker`, `apply_gate`) explicitly out of scope.

## 7. References

- #604 scheduler simplification, C4 addendum `99f92a1da` (family + aliases)
- #609 item 4b (per-counter removal, this spec)
- #582, #600, #557 owning issues per counter
- `src/kernel/task/scheduler.hpp:1608-1657`, `scheduler_registry.cpp:87-102` (members), `read/reset` at `L367-376`, `L515-548`
