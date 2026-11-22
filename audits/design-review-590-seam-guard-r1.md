# DESIGN REVIEW — #590 follow-up (fire_wait_publish IrqGuard seam fix)

PLAN: `audits/plan-590-seam-guard.md`
BASE: verified `0eb94f82d` (`git log --oneline -1` = `0eb94f82d Gate evidence: …`).
SCOPE: design-only review against sources on main tip. No patch, no test execution (per audit protocol).

## SOURCE VERIFICATION (all claims checked against tree)

- `fire_wait_publish` body `scheduler_lifecycle.cpp:549-554`: exchange-then-call, no guard. Include `<kernel/arch/hal/irq_guard.hpp>` at `:40`; TU already instantiates `IrqGuard` (`:121`, `:246`, `:346`, `:415`, `:474`). Plan step 1 needs no include edit — confirmed.
- `arm_wait_publish` UP+test-active gate `:537-540`; `WaitPublishSeamScope` `:556-572`; block `#if defined(CONFIG_DEBUG)` `:523-575`. Release header no-ops `wait_publish_seam.hpp:56-77` — zero release delta structural (preprocessor removes guard with block). Confirmed.
- Funnel A `terminate_err`: guard `:96`, wake `:152`, release `:165`. Funnel B `terminate_err_from_trap`: guard `:254`, release `:287`, wake `:293`. Both cited line numbers exact.
- `IrqGuard` semantics `irq_guard.hpp:135-189`: depth-counted per-CPU, `irq_was_` save/restore (`:138`, `:177-178`), saturating dtor (`:162-179`).
- Call-site depth: `proc_svc.cpp` contains zero `IrqGuard` (grep: no matches); fire site `:319-321` DEBUG-gated. Confirmed.
- Publication window `proc_svc.cpp:304-321`: dequeue `:310`, BLOCKED `:311`, pid `:312`, fence `:318`, fire `:319-321`. Confirmed.
- `wake_waiting_parent` `:639-725`: fence, `find_task` (bounded probe), `deliver_wait_status` `:667-668` (CR3 switch + armed store + shared tail `:600-632`, lock-free dmesg fault leg, comment `:586-588` "safe from … IRQ-masked fault context"), `dequeue_ready` + `set_task_ready` `:688-689`, no reschedule/switch/park. Confirmed.
- `set_task_ready` `:542-610`: own nested `IrqGuard` `:544`; `scheduler_lock_` only in target-not-up branch `:577-607` with `ENSURE(depth==0)` `:584`.
- Affinity clamp `scheduler_registry.cpp:172-180` (`mask &= allowed`, zero→CPU0); `queue_target` ctz (`scheduler_queue.cpp:535-540`).
- `id_table_find`/`remove` bounded `ID_TABLE_SIZE` probes (`scheduler_id_table.cpp:100-131`); `ID_TABLE_SIZE = 2*CONFIG_MAX_TASKS` (`scheduler.hpp:1509-1510`); `CONFIG_MAX_TASKS 64` (`nexios_config.h:58`) → bound is **128**, not 80.
- `TaskQueue` single-IrqGuard-unit mutations (`task_queue.hpp:13-17`); RQM enqueue own guard (`ready_queue_manager.cpp:33-39`).
- Registry: 10 `JARVIS_REGISTER_TEST` + 1 `JARVIS_REGISTER_RELEASE_TEST` (`test_waitpid.cpp:961-971`), class id `test_registry.cpp:815`; `process_waitpid` has no class row in `test-policy.tsv` → inherits `default|*|arch_scope|any` (all three arches). Arming scenarios at `:363` (arm `:394-395`) and `:625` (arm `:654-655`) confirmed.

## LOAD-BEARING CHECKS

1. **Placement — APPROVED (entry-guard correct; rationale overstated, see F2).** Entry-guard closes the true hazard (tick mid-hook over `dequeue→TERMINATED→wake→release`, matching observed 1/0/0/0 and 1/1/0/0 partials). Hook-only scope would also suffice; entry is the safe superset with the simpler invariant.
2. **Boundedness — APPROVED (numbers/citations to correct, see F1/F3/F4).** Only reachable spinlock `zombie_lock_` (leaf, short); `scheduler_lock_` branch unreachable on UP via affinity clamp + ctz (even-if-reached order is funnel-identical); probes bounded; no alloc/IPC/VFS/Logger/reschedule on success path; cold warn/fault legs pre-exist in the funnel under its own guard.
3. **Reentrancy — APPROVED.** Depth-counted guard, IF-save/restore, no stray `sti` when nested (`irq_was_=false`); call site depth 0; safe from ISR context by construction.
4. **Release identity — APPROVED.** Guard lives inside the existing DEBUG block; header `#else` no-ops and call site untouched → zero release bytes/instructions/behaviour, by preprocessor structure.
5. **Fidelity — APPROVED.** Both funnel cites exact. Seam takes IRQ discipline without `scheduler_lock_`: sound on UP (SMP excluded by arm gate, ISR masked, no voluntary switch in hook — same shape as S5 `set_task_ready`). Hook wake→release order matches `terminate_err`; trap-funnel release-first divergence disclosed with sound UP reasoning.
6. **Publisher-side window — out-of-scope + separate S2 issue CORRECT.** Reported signatures require hook-side interruption; a publisher-side NO_WAITER wedge would present as 1/1/1/1-complete hook + hung parent (never observed). Seam fix is complete for the reported wedge; no scope expansion.
7. **Check-7 / sanctity / false-close — APPROVED.** Retrieval table real: 2 graphify queries + shortest-path negative evidence + neighbors + 2 obsidian hits (incl. L-3 stray-`sti` caveat) + documented 0-match fallback + prior-plan refs. Dev adds no tests (test-owned files read-only respected); QE acceptance = both partial-signature scenarios must PASS (not merely not-wedge) + counter equality + 11/11 + adjacent classes + 3-arch + release leg + style + history rows.
8. **Vault fallback — ADEQUATE.** Specs live in-tree; fallback to `docs/irqguard-ledger.md:3-53` (authoritative production-site audit) + `audits/plan-590.md` is the correct corpus. Ledger omission justified and disclosed (DEBUG-only seam, ledger scopes production sites).

## FINDINGS

- [S3] `audits/plan-590-seam-guard.md:25` (INV-SEAM-IRQ-4) — wrong numeric bound
  WHY: "id-table probe ≤80" contradicts `ID_TABLE_SIZE = 2*CONFIG_MAX_TASKS = 128` (`scheduler.hpp:1509-1510`, `nexios_config.h:58`); the "80" is stale (historical comment in `scheduler_id_table.cpp:119-120`). Boundedness conclusion intact; correct to ≤128 in hand-off.
- [S3] `audits/plan-590-seam-guard.md:99` (S1) — exchange→cli sub-window rationale overstates
  WHY: a tick there lands pre-hook with consistent state (hook not started, no ISR touches the slot), so it cannot produce "the identical wedge"; hook-only scope would also close the true partial-update window. Entry-guard stays correct as the safe superset — keep placement, soften the claim to defense-in-depth.
- [S3] `audits/plan-590-seam-guard.md:18` (INV-SEAM-IRQ-2) — imprecise unreachability citation
  WHY: `scheduler_queue.cpp:469-476` is the user-task CPU0 clamp in `enqueue_ready`, not a kernel-parent clamp; UP unreachability actually follows from `set_affinity_err` mask clamping (`scheduler_registry.cpp:172-180`) + `queue_target` ctz → `target==here==0`. Conclusion holds; fix the citation and note the even-if-reached fallback is funnel-identical (IRQ-off→global, `ENSURE(depth==0)` at `:584`).
- [S3] `audits/plan-590-seam-guard.md:16-26` (INV-SEAM-IRQ-2/4) — `remove_child` sibling walk not inventoried
  WHY: `task.cpp:2814-2815` is a form-unbounded list walk; it is in fact bounded by `MAX_TASKS=64` live children and skipped on the seam path (`waiter_collects==true` for the BLOCKED parent, `:717-724`). Completeness note only; no path change needed.

Counts: S1 0, S2 0, S3 4.

DECISION: APPROVED
