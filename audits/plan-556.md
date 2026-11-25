# PLAN: Reply token + REPLY_RECV (N5) + in-syscall direct switch on Call/REPLY_RECV — M3 (#556)

## RETRIEVAL ARTIFACTS (PROMPT-audit check 7 — paste queries + one-line disposition)

> Required before any source read; absence is S2 REJECT independent of code quality.
> Queries below were run against graphify-out/graph.json and the obsidian vault;
> staleness doctrine applies (graph = relationships, files = current text).

- `graphify query "IPC reply token ticket direct switch REPLY_RECV" --limit 20` — used: located mint_reply_token, kReplyTicketLive, deliver_with_reply_rule, MessageQueue::reply_tok sidecar, Scheduler::try_direct_switch, arch::sched_apply_trigger.
- `graphify query "blocked_in_recv wake gate lost wakeup" --limit 20` — used: located IPC::send wake gate (reply_wait/blocked_on_queue/blocked_in_recv), scheduler_tick.cpp IPC sweep (blocked_in_recv && !is_empty), wake_reply_waiters_of §11.6 death funnel.
- `graphify path "IPC::send_sync" "Scheduler::try_direct_switch"` — used: proved single-publisher chain send_sync → try_direct_switch → switch_to_task; ignored stale H2-residual path.
- `graphify get_neighbors "MessageQueue" --relation_filter writes` — used: confirmed push_locked(tok) + reply_tok[] parallel write, remove_at compaction preserves token.
- `obsidian_search_vault "microkernel-transition M3 direct switch"` — used: INV-MK-5 depth 1/2/2, §5.6 slope <1.0 structural gate, Q3 frozen per_hop_bound=3 ticks.
- `obsidian_search_vault "ipc.md reply matching token"` — used: §2 one-shot token closes same-sender stale residual, drop counter ipc_reply_dropped_count.
- `obsidian_search_vault "audits/done ipc_sched_trace CONFIG_DEBUG_IPC_SCHED"` — used: prior M3 audit r3 APPROVED with S3 notes; bench relay parked due to #582 unlink sweep blind spot — kept parked until sweep fix verified.
- `obsidian_search_vault "irqguard-ledger park lock INV-PARK APPLY"` — used: INV-PARK-2 / INV-APPLY-1/2/3, lock order scheduler_lock_ → queue lock → leaf, resched_park gate bit 2 waiver semantics.

## AFFECTED FILES

Tree state (read this first). The M3 *core* is already present in this tree and is **not** to be re-implemented: token mint + ticket arm (ipc/ipc.cpp:mint_reply_token, kReplyTicketLive), atomic consume-stamp-push (ipc/ipc.cpp:deliver_with_reply_rule, MessageQueue::push_locked(tok) + reply_tok[] sidecar), token-scoped probe_pop_reply_from / pop_reply_from / stamp_from_try, IPC::note_delivered_request, IPC::reply_recv_err, syscall 97 + handler + dispatch + libc stub, MAX_SYSCALL = 105, Scheduler::try_direct_switch, arch::sched_apply_trigger on all three arches, 10 QE tests, expected counts (ipc 107 / ipc_core 37). What is **open** is: two latent defects (stale blocked_in_recv, reply/ticket TOCTOU), one transient-STALE window, the syscall's non-typed face, the unenforced structural gate (slope < 1.0), missing fast-path observability, a doc/code drift, and cross-arch/gate evidence. Entries marked [LANDED—verify] are audit/verify-only.

- src/kernel/task/scheduler_switch.cpp [CHANGE] — add DEBUG-only direct-switch outcome counters (relaxed fetch_add outside decision; no control-flow divergence)
- src/kernel/task/scheduler.hpp [CHANGE] — declare counter accessors (DEBUG-only, nodiscard)
- src/kernel/test/park_seam.hpp [CHANGE] — declare direct_switch_ctx_waivable — per-TCB conjunctive waiver (park_test_member && park_seam_active && !isr_context_park_denied depth-0 only; bit 2 waiver, never global)
- src/kernel/test/test_park_seam.cpp [CHANGE] — implement waivable (test-context-only, production IF=0 path never waived)
- src/lib/test.hpp [CHANGE] — TF_DIRECT_DEPTH0 (seam flag, debug builds only)
- src/lib/test.cpp [CHANGE] — arm seam (single writer, test harness only)
- src/kernel/ipc/ipc.cpp [CHANGE] — (1) clear blocked_in_recv on all 4 reply_recv_err exits, (2) ordered ticket publish (tag→flag→ticket→stamp) inside same IrqGuard, (3) expected-ticket generation re-check at deliver + REPLY_RECV entry, (4) drop counter ipc_reply_dropped_count on Pushed (non-PushedReply)
- src/kernel/ipc/ipc.hpp [CHANGE] — doc updates (ticket lifetime, drop semantics)
- src/kernel/syscall/syscall_handlers_ipc.cpp [CHANGE] — typed face for REPLY_RECV (IpcError mapping; next_cap must be 0 fail-closed, never reaches core)
- src/libc/syscall.h, src/libc/gen/nexios_syscalls.{h,c} [CHANGE] — docs + typed wrapper comment
- src/kernel/test/bench_mk_ipc_baseline.cpp [CHANGE] — un-park M3 measurement ONLY when #582 sweep fix verified; slope gate FAIL (not WARN) slope_ticks_per_hop_x100 < 100
- src/kernel/test/test_ipc_reply.cpp [CHANGE] — new tests: stale-recv leak, TOCTOU Contended stamp, transient-STALE retry, drop counter, depth-0 waiver not production
- src/kernel/test/test_expected_counts.hpp [CHANGE] — bump rows (measured, never hand-sum)
- src/kernel/arch/aarch64/test_aarch64.cpp, src/kernel/arch/riscv64/test_riscv64.cpp [CHANGE] — 3-arch parity rows for direct switch (same counts, same assertions; empty-by-design documented)
- docs/specs/ipc.md, microkernel-transition.md, irqguard-ledger.md [CHANGE] — close doc/code drift (§2 one-shot, §5.6 direct switch, ledger lock order + park waiver)

## INVARIANTS (normative, enforcement point, status)

| ID | Invariant | Enforcement | Status |
|---|---|---|---|
| INV-TOK-1 | One-shot token: each Call mints a fresh token (g_reply_token_seq, 0 reserved, 31-bit wrap skip 0) and the ticket holds token|kReplyTicketLive; deliver consumes via atomic exchange(0) exactly once; consumed token never matches again. | ipc.cpp: mint_reply_token, kReplyTicketLive, deliver_with_reply_rule exchange | TRUE after fix |
| INV-TKT-1 | Ticket lifetime: tag(dest_id, dest_gen) published BEFORE flag+ticket (all RELEASE), deliver observes triple with ACQUIRE; server death keeps ticket live for sweep-stamp recovery, never forged (sender_id kernel-stamped). | ipc.cpp: send_sync publish order, reply_recv_err validation 1383 | PARTIAL — needs ordered proof in plan |
| INV-RECV-1 | blocked_in_recv parity: every BLOCKED transition via blocked_in_recv sets flag true under IrqGuard + dequeue_ready before BLOCKED; every exit (success, STALE, timeout, death) clears flag false before RUNNING. | syscall_handlers_ipc.cpp:124-186, ipc.cpp:1453-1510 | VIOLATED — reply_recv_err success paths leak (fix step 4) |
| INV-LOCK-1 | Lock order: scheduler_lock_ → MessageQueue::lock_ → leaf; no scheduler_lock_ held at BLOCKED (INV-C1/4b resched_park_gate). | sync/spinlock_order.hpp, scheduler_switch.cpp:1164 | TRUE — gated by spinlock_contract_check_clean |
| INV-SW-1 | Single-publisher: only Scheduler::try_direct_switch publishes via switch_to_task; no direct slot writes; arm in flight (save_rsp_to!=0) refuses second publish. | scheduler_switch.cpp:1158-1179 | TRUE |
| INV-BOUNDED-1 | Bounded: direct switch attempts per wait ≤ kDirectMaxAttempts=3, counter reset at wait entry (cur->ipc_direct_attempts=0); refusal falls through to tick path, never spin. | ipc.cpp:909, 1424 | TRUE if reset proven |
| INV-IRQ-1 | IRQ windows: try_direct_switch requires isr_nesting_own==0 && irq_guard_depth_own==0 && spinlock_contract_check_clean; no lock at BLOCKED. | scheduler_switch.cpp:1160-1167 | TRUE — waiver must stay conjunctive (step 3) |
| INV-ZERO-1 | Zero allocation / zero new cap: reply path never allocates (push_locked only), no new CapType, no PMM. | ipc.cpp: deliver_with_reply_rule | TRUE |
| INV-RET-1 | Return-state hygiene: every reply_recv_err exit clears reply_ticket, reply_wait, wait_dest_id/gen, and restores RUNNING + sti when was_blocked, dequeues if in_ready_queue_. | ipc.cpp: 937-1145 | PARTIAL — blocked_in_recv missing |

## INVARIANTS & STEP-BY-STEP

13 steps, each FAIL-closed; Q1 structural gate slope <1.0 is FAIL (not WARN). Baseline evidence first.

| Step | File:line | Action | GREEN if |
|---|---|---|---|
| 0 | — | Baseline evidence on HEAD: run ipc/ipc_core/bench on x86_64 debug + aarch64/riscv64 link; record slope_ticks_per_hop_x100=100 (TCG), direct_switch counters 0, blocked_in_recv leak repro via added assert | logs + test-history rows attached |
| 1 | scheduler_switch.cpp:1133 | Add DEBUG-only direct-switch outcome counters (attempted/succeeded/refused_armed/refused_nesting/refused_lock). Relaxed fetch_add, never in decision predicate; accessors nodiscard | build 3-arch green, invariants INV-SW-1/INV-BOUNDED-1 unchanged |
| 2 | scheduler.hpp:285 | Declare counter accessors (DEBUG-only) | header-only, no arch ifdef |
| 3 | park_seam.hpp / test_park_seam.cpp / lib/test.hpp/.cpp | Declare TF_DIRECT_DEPTH0 + direct_switch_ctx_waivable() + arm seam. Waiver is per-TCB conjunctive: park_test_member && park_seam_active && depth-0 deny only (isr_context_park_denied bit 2). Production path (park_test_member==false) never waived; all other gate bits (0,1,3,4,5,9-13) unchanged | seam test proves waived park reaches resched_park, unwaived stays denied; no production IF=1 waiver |
| 4 | ipc.cpp:1369-1512 | Fix stale blocked_in_recv: clear flag on all 4 reply_recv_err exits — (a) loop-top pop success (1437), (b) probe_hit success (1458), (c) post-hlt pop success (1500), (d) STALE/NO_TOKEN early returns keep existing clears. Add ENSURE parity assert at return | reply_recv sweep test: BLOCKED server with blocked_in_recv+empty inbox stays BLOCKED; after reply consumed flag==false |
| 5 | ipc.cpp:1048-1063, 1283-1290 | Ordered ticket publish: inside publish_guard publish wait_dest_id → wait_dest_gen → reply_wait(true) → reply_ticket(token|kReplyTicketLive) → stamp_from_try, all RELEASE; document Contended==live holder whose wake gate observes publish, not a failure | TOCTOU test: Contended stamp still delivers within 1 tick; no double-publish |
| 6 | ipc.cpp:562-601, 1380-1386 | Expected-ticket + generation validation: deliver re-checks sender generation via find_task under queue lock; reply_recv_err entry re-checks caller generation + ticket live, returns STALE without queue mutation | forgery test: recycled TCB with same id but new generation returns STALE, drop counter +1 |
| 7 | ipc.cpp:598-600, 1398 | Drop counter: non-PushedReply due to no live ticket increments ipc_reply_dropped_count (DEBUG, relaxed); never fails the sender | counter test: unmatched reply dropped, counter 1, sender still success when matching reply arrives |
| 8 | syscall_handlers_ipc.cpp:538-588 | Typed face: sys_reply_recv validates next_cap==0, reply_len ≤ IPC_MAX_MSG_SIZE, last_caller_id!=0 else REPLY_NO_TOKEN, generation+ticket else REPLY_STALE, returns typed IpcError mapped to SYS_ERR_GENERIC_FAIL fail-closed; never passes garbage to core | fuzz test: garbage next_cap!=0 returns fail-closed without touching queue |
| 9 | ipc.hpp / libc | Doc updates: ticket lives only in tickets+sidecar, never on wire; drop semantics; typed errors in header | docs build green |
| 10 | bench_mk_ipc_baseline.cpp:137,373 | Un-park M3 relay ONLY if #582 sweep fix verified green; otherwise keep parked. Slope gate is FAIL: slope_ticks_per_hop_x100 < 100, per_hop_bound=3 ticks, end-to-end = depth*3 + 0. Bench runs 3 consecutive greens host-quiet | bench 39/39 or parked with reason in file |
| 11 | test_ipc_reply.cpp | New tests: blocked_in_recv parity, Contended stamp, transient STALE retry helper, drop counter, waiver not production, next_cap fuzz | ipc 107+? measured, ipc_core 37+? |
| 12 | arch/aarch64,test_riscv64,test_expected_counts.hpp | Arch legs + count bumps: aarch64 SGI11, riscv64 SSIP, x86_64 0xE1 all depth-2, idempotent, zero-alloc; bumps from measured runs not hand sums | 3-arch link green, counts match registry |
| 13 | docs/specs/* | Close drift: ipc.md §2 one-shot, microkernel-transition §5.6 direct switch + INV-MK-5 table, irqguard-ledger lock order + park waiver conjunct | spec references cite path:symbol, line numbers advisory |

Q1 gate: slope <1.0 structural (no measurement needed beyond the FAIL threshold); measurement gate per_hop_bound=3 ticks (§5.6 frozen).

## TEST STRATEGY & SIL3 RISKS

- Strategy: smallest applicable class first (ipc, ipc_core, bench, core_sched), then `bash scripts/run_all_classes.sh x86_64 debug` and aarch64/riscv64 link; per-class test-history.txt rows appended with measured TIME (never deduped). No raw QEMU, no bare meson test --suite.
- Sanctity: new tests live on testbed only until main merge; no weakening of existing asserts; no `#if 0` parked tests without documented sweep-heal replacement.
- S1 risks: WEDGE orphan (BLOCKED+inrq), lost wakeup (blocked_in_recv leak), PID reuse forgery, single-publisher violation → strand or privilege escalation.
- S2 risks: TOCTOU ticket/stamp race → transient STALE spin 1 tick (bounded), unbounded direct-switch loop, cross-arch trigger asymmetry → silent fail on aarch64/riscv64.
- S3 notes: counter catalog reservation, doc line-number drift, style 0.

