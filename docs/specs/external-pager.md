# External Pager Protocol — User-Space #PF Delegation (issue #107, #105 Part A)

**Doc ID:** NEX-SPEC-2026-09-04-107
**Status:** DESIGN PAPER — no implementation before review
**Milestone target:** v0.4.3 (milestone 2, high-risk item)
**Related:** `docs/specs/death-notify.md` (registry pattern, supervisor pairing),
`docs/specs/shm.md` (#106 FrameCap/FrameUserMap pairing),
`docs/specs/syscall-fastpath.md` (#92 FAST/FULL tiering + canary relocation),
`docs/specs/ipc.md`, `docs/specs/memory.md`, `docs/specs/cspace.md`.
**High-risk:** the #PF path is exception-ISR context; a stalled or malicious
pager must NEVER be able to wedge a faulting task or the kernel. The **bounded
pager contract** (§4) is the mandatory, named, testable design element that
designs the deadlock footgun out.

## 1. Purpose

Route a subset of user-space page faults (#PF, vector 14) over the IPC
infrastructure to a capability-designated Ring-3 pager thread, instead of the
default SIGSEGV.  A client task registers a pager; when the client faults on a
not-present user page, the kernel records the fault (task id + fault VA +
error-code flags), pulses the pager, blocks the client, and the pager maps a
backing page into the client's address space and completes the fault, after
which the client retries the faulting instruction.  A hard, kernel-enforced
deadline bounds the whole exchange so no pager behaviour — stall, crash, or
malice — can wedge anything.

This is the missing user-space fault-isolation primitive: without it a
"crash supervisor" (#105 Part B) can only catch and restart a server that died;
with it a supervisor-grade pager can *resolve* demand faults (sparse
allocations, memory-mapped I/O backends, paged-out buffers) and keep the server
alive.

**Hard-RT framing (explicit):** fault delegation is **opt-in per client**
(§4.1).  A hard-RT task that cannot tolerate a fault simply does not register a
pager — it keeps default SIGSEGV and its WCET is unaffected.  The pager is for
supervisor-managed servers, where a timed-out fault → SIGSEGV → supervisor
restart (#105 Part B) is a deterministic, bounded recovery.  Scheduling never
depends on the pager: the kernel never waits on it, and the client's worst-case
fault latency is bounded by the kernel-enforced deadline regardless of pager
behaviour (§4.6).

## 2. Motivation & Non-Goals

### 2.1 Motivation

- A client wants sparse address spaces: it can touch pages that are not yet
  backed, and a designated pager provides the backing on demand (bounded,
  capability-gated, never kernel auto-paging).
- Fault isolation for servers: a pager decides *which* faults a client's
  pointer dereferences may recover from, rather than the kernel default of
  terminate-on-SIGSEGV.
- It pairs with the #105 Part B crash supervisor: the supervisor detects a dead
  or stalled pager (death pulse) and restarts it; the external pager protocol is
  what a restarted pager re-arms.

### 2.2 Non-Goals

- **No kernel swap-on-demand.**  The kernel never pages anything on its own;
  every delegated fault requires one explicit pager round-trip.
- **No copy-on-write (COW).**  Page tables are never re-pointed lazily; the
  pager explicitly maps backing frames.
- **No demand paging of kernel memory.**  Kernel-space faults (HHDM, kstack
  guard pages, recovery windows, reserved vectors) are untouched: they keep the
  existing panic / `g_user_access_recover_ip` / guard-page behaviour (§3.2).
- **No executable pager mappings in v1.**  `SYS_PAGER_MAP` maps frames
  non-executable (NX).  Instruction-fetch faults (`#PF` I/D=1) are therefore
  **not delegatable** in v1 — they keep the SIGSEGV path.  Executable pager
  mappings are a documented follow-on (requires a W^X/rights review; §OPEN
  QUESTIONS).
- **No multiple pagers per client, no pager chaining, no user-selected
  timeouts.**  One registered pager per client; the timeout is a kernel
  constant.
- **No paging for kernel tasks; no SMP implementation** (single-core today; the
  registry lock discipline is SMP-ready, §7).
- **No change to the FAST syscall subset** (syscall-fastpath.md §3.1) and no
  change to `g_user_access_recover_ip` semantics.

## 3. Fault Classification

### 3.1 The existing #PF paths (must remain untouched)

`handle_interrupt_c` (src/kernel/kernel.cpp:1409) orders #PF handling as:

1. `g_user_access_recover_ip` redirect (kernel.cpp:1490) — checked FIRST.  Any
   #PF while a `stac/clac` copy window is open jumps to the recovery label
   (checked_ptr.hpp).  This covers kernel-mode faults during
   `copy_from_user`/`copy_to_user` and the `is_user_string` probe.
2. From-user exception → signal path (kernel.cpp:1510): `vector==14` from a
   user CS logs CR2 and delivers SIGSEGV via `deliver_signal_to_user`.
3. Kernel-mode guard-page check for the kstack window (kernel.cpp:1536) →
   `stack_overflow_hook`/panic.
4. Everything else in kernel mode → CPU EXCEPTION panic.

### 3.2 Delegatable set (new)

A fault is eligible for pager delegation **iff all** of:

| # | Condition | Meaning |
|---|---|---|
| F1 | `vector == 14` and faulting CS is user and `t && t->is_user_` | a user task, user-mode fault |
| F2 | `g_user_access_recover_ip == 0` | NOT inside a stac/clac copy window (the single-slot recovery must stay authoritative — it is checked before us, §3.1.1) |
| F3 | error-code P bit (bit 0) == 0 | not-present fault — NOT a write-to-RO / NX / reserved-bit fault |
| F4 | error-code RSVD bit (bit 3) == 0 | page-table structure is sane |
| F5 | error-code U/S bit (bit 2) == 1 | genuine user-address access |
| F6 | error-code I/D bit (bit 4) == 0 | data access only (v1; §2.2 non-goal) |
| F7 | `CR2 < USER_SPACE_LIMIT` | fault VA is in the user half |
| F8 | a pager is registered for `t`, the pager is live and `pager != t` | delegation authority (B4) |
| F9 | the fault VA is not the poisoned-aborted VA latch (if set, §4.7) | no abort/re-fault loop |
| F10 | the fault VA does not collide with an installed canary slot of `t` (re-checked at map time) | canary integrity (#92) |

All other user #PFs keep today's SIGSEGV; all kernel #PFs keep the
recovery/guard/panic behaviour.  `F2` is structurally guaranteed because the
recover-IP redirect returns before the from-user branch is reached; `F6`/`F7`
are hard rejects that make instruction-fetch and out-of-range faults
non-delegatable.

## 4. The Pager Contract (CORE)

### 4.1 Registration (capability-designated, opt-in)

- `SYS_PAGER_REGISTER(pager_pid)` is called **by the client**, designating its
  own pager.  Authority gate mirrors DeathNotify::watch: the caller may only
  register a pager **for itself** (`pager_pid` resolves to a live task,
  `pager != client`).  A third party can never install a pager on a victim —
  there is no capability-less fault-injection.  `pager == client` is rejected
  (a task can never page itself — B4).
- One registration per client, keyed by `(client_id, client_gen)`, in a static
  bounded table `CONFIG_CAP_MAX_PAGER_CLIENTS` (default 8).  **Fails closed**
  when full (no dynamic allocation on RT paths, CODING_STYLE §4).
- `SYS_PAGER_UNREGISTER(client_pid)` — the client removes its own registration;
  the pager may drop a client it serves.  Both paths also abort any pending
  fault (§4.6).

### 4.2 The fault record

Per registration there is **at most one outstanding fault** — a client with a
pending fault is BLOCKED and cannot execute, so a second fault is
structurally impossible.  The record:

```c
struct PagerFault {          // kernel::ipc, lives in the registry slot
    uint64_t fault_id;       // sequence number (monotonic, for recv/map/abort)
    uint64_t client_id;      // + generation captured at record time
    uint32_t client_gen;
    uint64_t pager_id;       // + generation captured at record time
    uint32_t pager_gen;
    uint64_t fault_va;       // page-aligned faulting VA
    uint64_t fault_flags;    // raw #PF error-code bits (W/R, U/S, I/D — info)
    uint64_t deadline_tick;  // now + CONFIG_PAGER_FAULT_TIMEOUT_TICKS
    // ledger of pager-mapped pages for this fault (rollback on abort/timeout):
    uint64_t mapped_va[CONFIG_PAGER_MAX_PAGES_PER_FAULT];
    cap::FrameCap *pin[CONFIG_PAGER_MAX_PAGES_PER_FAULT]; // cap pins
    uint32_t mapped_count;
    bool map_in_progress;    // MAP_IN_PROGRESS pin (§7.2)
    bool poisoned_va;        // aborted VA latch (§4.7)
};
```

The delegation channel mirrors DeathNotify: the kernel pulses the pager's
`Notify` with `PAGER_FAULT_PULSE` (0x50414752, wakeup-only), and the pager
drains the record via `SYS_PAGER_RECV` (never blocks).  The message layout
delivered to the pager is exactly `{fault_id, client_id, fault_va,
fault_flags}`.

### 4.3 The fault → block → map → resume flow

1. Client faults; the #PF dispatcher classifies (§3.2).  If delegatable, it
   looks up the client's registration under the registry lock, creates the
   fault record (sets `deadline_tick`), collects the pager `(id,gen)`, releases
   the lock.
2. **Pulse** the pager's Notify (poke OUTSIDE the registry lock, id+gen
   revalidated — safe no-op on a dying pager; DeathNotify pattern).
3. **Block the client** (inside the #PF ISR, mirroring the `sys_receive`/Notify
   block sequence, CODING_STYLE §11.1/§11.2):
   `state = BLOCKED` → `dequeue_ready` → `reschedule()`.  The client holds no
   lock.  Its faulting context is preserved in its TCB; `iretq` on resume
   re-executes the faulting instruction.
4. Pager wakes (Notify), calls `SYS_PAGER_RECV`, gets `{fault_id, client_id,
   fault_va, fault_flags}`.
5. Pager obtains frames (`SYS_FRAME_CREATE`, #106) and calls
   `SYS_PAGER_MAP(fault_id, cap_handle, count, flags)`.  The kernel validates
   (caller == record pager, record pending, `count <=
   CONFIG_PAGER_MAX_PAGES_PER_FAULT`, cap is the pager's own live FrameCap),
   maps the frames into the **client's** PML4 at `fault_va` (user, RW-from-cap,
   NX, §5.2), ledger + cap-pin, consumes the record, and **wakes the client**
   exactly once.
6. Client resumes; the faulting instruction retries against the now-present
   page.

**Wake order with pending signals (RESOLVED — retry-first, hard-RT):** on
MAP-complete, if the client has `pending_signals` (e.g. SIGKILL while
fault-blocked), the wake re-queues and the faulting instruction retries FIRST;
the pending signal is delivered at the next syscall/exception boundary (current
kernel semantics).  Rationale: deterministic single-step completion (the pager
contract promises the access is now satisfied — leaving it unresolved on a
signal would make progress impossible under a signal storm), fixed WCET (retry
is a bounded single instruction re-execution vs the larger, less-predictable
signal-delivery path), and consistency with how signals are already delivered.

### 4.4 The bounded pager contract (named design element)

| ID | Bound | Enforcement |
|---|---|---|
| B1 | ≤ 1 outstanding fault per client | structural (blocked client can't fault again) |
| B2 | fault lifetime ≤ `CONFIG_PAGER_FAULT_TIMEOUT_TICKS` (default 1000 = 1 s @ CONFIG_TICK_HZ 1000) | scheduler `on_tick` watchdog scan (§4.6) |
| B3 | every pager syscall is non-blocking | `SYS_PAGER_RECV` never blocks; `MAP`/`ABORT` never wait on the client |
| B4 | a pager never delegates its own faults | `pager == client` rejected at register; a pager's own #PF → SIGSEGV (never delegated) |
| B5 | no paging recursion | a client with a pending fault is BLOCKED (cannot act as a pager); a pager can never be its own client |
| B6 | the kernel never blocks on the pager | pulse is a non-blocking poke; the client blocks only on its own registry record + the scheduler, never on any pager lock/queue |
| B7 | ≤ `CONFIG_PAGER_MAX_PAGES_PER_FAULT` (default 4) pages per fault | validated in `SYS_PAGER_MAP`; bounded unmap on abort |
| B8 | **deadlock footgun designed out** | the client's block is woken by exactly one of {MAP-complete, ABORT, watchdog-timeout, death-drain}; the timeout path (§4.6) guarantees the client is never stuck behind a stalled pager |
| B9 | **no priority-inversion dependency** | a pager's effective priority must be ≥ the highest-priority client it serves; otherwise a starved pager times every fault out to SIGSEGV (bounded, but the availability goal fails).  The kernel adds no priority boost of its own in v1 |

**The "pager blocked while faulting task waits" deadlock is impossible by
construction:** the faulting task never waits on the pager — it waits in the
registry (a passive record) and is made READY by the registry's own
state machine, which has three independent waker sources, one of which is a
kernel timer that does not depend on pager liveness.  The kernel, in turn,
never waits on the pager at all (B6).  The only way a client is ever woken is
consumption of its fault record; consumption happens with or without pager
cooperation.

### 4.5 Completion

- `SYS_PAGER_MAP(...)` — maps and completes (the client retries successfully).
- `SYS_PAGER_ABORT(fault_id)` — pager explicitly cannot satisfy this fault:
  kernel consumes the record, unmaps the ledger, wakes the client, and sets the
  **poisoned-VA latch** (the record's `poisoned_va`), so the immediate retry of
  the same VA re-faults and is classified non-delegable (F9) → SIGSEGV.  The
  latch is cleared on the next successful delegation of a different VA or on
  unregister.  The watchdog timeout uses the SAME latch (§4.6) — same-VA
  retries go SIGSEGV while a future different-VA fault delegates normally.
  Prevents the abort/re-delegate infinite loop.
- Watchdog timeout (§4.6).
- Death drain (§4.6).

### 4.6 Timeout & death (exactly-once, loop-free)

- **Watchdog** (`on_tick`, already under `scheduler_lock_` + IrqGuard): bounded
  scan of the registry for records with `deadline_tick <= now` and
  `map_in_progress == false` (§7.2).  The deadline is a **hard bounded latency,
  not a soft timeout**: for each overdue record — consume it under the registry
  lock (released before the wake), **rollback** (`VMM::unmap_frame_from_cap`
  each ledger page in the client's PML4, release the cap pins), set the
  **poisoned-VA latch** (§4.5, same as `SYS_PAGER_ABORT`), then wake the client.
  The client is made READY regardless of pager liveness (woken at ≤
  `CONFIG_PAGER_FAULT_TIMEOUT_TICKS` + one map window — §7.2 — no matter what
  the pager does).  The client's retry re-faults on the SAME VA → F9 false →
  SIGSEGV (loop broken, no infinite re-delegate).  **Per-fault fail-closed with
  registration kept:** THIS fault reverts to SIGSEGV, but the registration
  survives so a FUTURE fault on a different VA is served normally (a transient
  scheduling stall must not permanently disable paging).  **Eviction happens
  only on pager death** (§4.6) or explicit unregister.  No iret-frame injection
  is needed; a late `SYS_PAGER_MAP` after consumption finds the record gone →
  error, no second wake, no double unmap.
- **Pager death** (`PagerRegistry::drain_task(pager)` in the pager's
  `cleanup()`, before its Notify is destroyed): evict its registrations; for
  each client with a pending fault: consume, unmap ledger, release pins, wake
  the client (reverts to SIGSEGV).  The #105 Part B supervisor may be watching
  the pager and can restart it.
- **Client death** (`PagerRegistry::drain_task(client)` in the client's
  `cleanup()`, **before** `free_user_pages`): consume any pending fault, unmap
  the ledger in the (dying) client's PML4, release pins.  The client is BLOCKED
  so it cannot be reaped while fault-pending unless first killed; the drain is
  the guarantee that no pager-owned frame is ever freed by the client's
  `free_user_pages` (§5.3).
- **Exactly-once resume:** the record transitions `PENDING → consumed` under
  the registry lock; the wake is a side-effect of the transition, performed by
  exactly one of {MAP-complete, ABORT, watchdog, drain}.  Late arrivals
  (`SYS_PAGER_MAP` after timeout) find the record gone and return an error —
  no second wake, no double-unmap.

### 4.7 Abort vs timeout policy summary

| Path | Record | Ledger | Client | Registration | Client retry result |
|---|---|---|---|---|---|
| `SYS_PAGER_MAP` | consumed | kept (mapped) | READY, retries | kept | faulting instruction succeeds |
| `SYS_PAGER_ABORT` | consumed | unmapped | READY, retries | kept (+ VA poison latch) | re-fault → SIGSEGV (F9) |
| watchdog timeout | consumed | unmapped | READY, retries | kept (+ VA poison latch) | re-fault on SAME VA → SIGSEGV (F9); a future DIFFERENT VA delegates (per-fault fail-closed) |
| pager death | consumed | unmapped | READY, retries | evicted | re-fault → SIGSEGV (F8) |
| client death | consumed | unmapped (pre free_user_pages) | (dead) | evicted | — |

### 4.8 Block-inside-#PF-ISR mechanism (highest implementation risk)

The delegation path must **block the faulting client from inside the #PF
exception handler** and resume it later so the faulting instruction retries.
This is a *new* kernel mechanism — no existing path blocks from exception-ISR
context (`sys_receive`/`Notify` block in task context; the #NM handler returns
without blocking; signal delivery reschedules but never blocks).  The contract:

1. **Entry state.**  The client faults; `handle_interrupt_c` (vector 14, user
   CS) classifies (§3.2) and, if delegatable, calls
   `PagerRegistry::delegate_fault(t, error_code, regs, cr2)`.  The exception
   frame (`regs[]`) already holds the interrupted user state — the `iretq`
   target `regs[17]` (RIP) + `regs[18]` (CS) + RSP are untouched.  The faulting
   instruction will re-execute on resume; nothing is rewritten.
2. **Record + pulse + block.**  Under the registry lock: create the fault
   record (`deadline_tick` set), capture `(client_id, client_gen)` and
   `(pager_id, pager_gen)`.  Release the lock, pulse the pager's Notify
   (outside the lock, id+gen revalidated).  Then block the client
   **without returning through the ISR epilogue**:
   `state = BLOCKED; blocked_on_pager_fault = &record;
   dequeue_ready(t); Scheduler::reschedule();` — the ISR never `iretq`'s to the
   client; control stays in the kernel and the scheduler switches to a ready
   task.  The client's kernel stack (with the exception frame) is preserved as
   its suspended context.
3. **Resume.**  On record consumption (MAP-complete / ABORT / watchdog / death-
   drain), the registry wakes the client (`state = READY`, re-enqueue) via the
   DeathNotify collect-under-lock / wake-outside-lock pattern.  The scheduler's
   normal `switch_to_task` restores the client's kernel stack — the exception
   frame — and the ISR epilogue `iretq` re-enters the user instruction that
   faulted.
4. **Exactly-once.**  The exception frame is restored by exactly one resume (the
   `PENDING → consumed` transition is the single gate, §7.3).  A second
   block/resume on the same fault is structurally impossible (the client is
   BLOCKED, so it cannot fault again — B1).
5. **Preemption safety.**  The block runs inside the ISR with the registry lock
   ALREADY released (step 2) and holds no other lock; the ISR may be
   interrupted by the timer watchdog without a lock-order inversion (§7.1).
   `blocked_on_pager_fault` is cleared by the resume path (under the scheduler
   discipline, mirroring `waiting_on_*`), so a stale pointer is never read.

The v1 implementation keeps the client's stack intact for the whole block; the
exception frame lives on the kernel stack, which is never freed while the task
is BLOCKED (the reaper only frees it after the task is TERMINATED + drained,
and `PagerRegistry::drain_task(client)` runs before `free_user_pages`, §5.3).

## 5. Mapping & Ownership Model

### 5.1 What the pager maps

`SYS_PAGER_MAP(fault_id, cap_handle, count, flags)` resolves `cap_handle` in
the **pager's** CSpace to a live `FrameCap` (type + rights + generation via
`cap::lookup`; the pager must hold the cap it maps — capability-gated).  The
kernel maps `count` frames via `VMM::map_page_in_pml4(va, phys, user=true,
executable=false, client->page_table_)` — a cross-address-space mapping gated
by the fault record, which is the only authority that lets a task touch another
task's PML4.  No new VMM API is required (existing `map_page_in_pml4` +
`unmap_frame_from_cap`).

### 5.2 Capability pinning (UAF guard, FrameUserMap precedent)

Every mapped page records `cap::FrameCap *pin` in the fault ledger and the
registry holds an `acquire()` reference on it for the fault-mapping lifetime.
The unmap path can therefore never dereference a freed cap.  **Revocation
closure:** `FrameCap::dispose()`/`revoke()` call
`PagerRegistry::invalidate_cap(this)` which unmaps every ledger entry backed by
the cap (across all clients) and releases the pins before the cap block is
freed — identical to `FrameUserMap::invalidate_cap` (shm.md).

### 5.3 Frame ownership & the client's teardown

The frames **remain owned by the pager's FrameCap**.  The client's PML4
references the frames only through the fault ledger.  `cleanup()` /
`exec_into_current` call `PagerRegistry::drain_task(client)` **before**
`free_user_pages`, and the ledger unmap clears the client's PTEs first — so
`free_user_pages` (which frees every USER-owned page in the PML4) never sees a
pager-owned frame.  Same ordering precedent as `FrameUserMap::drain_task`
(shm.md INV-SHM4).

## 6. Coexistence Analysis

### 6.1 `g_user_access_recover_ip` (single-slot recovery)

Unchanged and authoritative.  It is checked at the very top of
`handle_interrupt_c` (kernel.cpp:1490), before the from-user branch; the
delegated-fault dispatch sits *inside* the from-user branch, so any #PF inside
a `stac/clac` window is still redirected to the recovery label and can never be
delegated (F2).  The recovery contract ("set by copy paths, cleared on exit,
checked before everything") is untouched; the pager adds no new writers or
readers of that slot.

### 6.2 SMAP / AC windows

The pager syscalls are ordinary FULL-path syscalls.  `SYS_PAGER_RECV` writes a
`PagerFault` to a user buffer via `CheckedPtr` (stac/clac + recover-IP, exactly
like `DEATH_RECV`); `SYS_PAGER_MAP` validates `cap_handle` from registers (no
user deref) and maps into a kernel-side PML4.  No SMAP interaction is added to
the #PF ISR path itself (the ISR touches only kernel structures).  The debug
AC-leak detector (`panic("MP-4: AC flag leaked ...")`, syscall.cpp:136) applies
to these syscalls like any other.

### 6.3 Scheduler canary sampling (#92 CONFIG_CANARY_SAMPLE_TICKS)

`canary_check_in_scheduler_hooks` is a pure page-table read of the **current**
task's segment canaries; it never faults and never runs for a BLOCKED client.
A pager-mapped page cannot disturb the canaries: (a) canary slots are mapped
pages, so a fault on them has P=1 and fails F3 (never delegated); (b)
`SYS_PAGER_MAP` re-rejects a target VA that collides with any installed
`canary_before/canary_after` of the client (F10).  FAST-path membership is
unchanged: the five new `SYS_PAGER_*` syscalls are NOT in `k_syscall_fast[]`
(they touch user memory / cross-task page tables — a canary-skip there would be
a privilege hole, shm.md discipline).

### 6.4 #106 FrameUserMap / FrameCap pairing

The pager's backing pages come from `SYS_FRAME_CREATE` (FrameCap).  The
revocation closure (§5.2) is the same `invalidate_cap` discipline as
`FrameUserMap`; `FrameUserMap` itself is unaffected (a pager may also map into
its own window for its own data plane).  Shared rings (#106 Part B) are
reclaimed via `FrameUserMap::drain_task` exactly as today.

### 6.5 #105 DeathNotify pairing

A death-pulse is the recovery trigger for a dead **pager**: the supervisor
drains the death record and restarts the pager, which re-arms by the clients
re-registering (or the supervisor re-registering on the clients' behalf —
clients are opt-in).  Pending faults for a dead pager are aborted to SIGSEGV
(§4.6), so no client is stranded while the supervisor restarts the pager.

### 6.6 Kernel-mode #PF (guard pages, recover, reserved vectors)

All untouched: the delegated dispatch is reachable only from the from-user
branch; kernel-mode #PFs take the existing recover/guard/panic paths (§3.1).

## 7. Concurrency & SIL 3 Safety Analysis

### 7.1 Lock ordering

`PagerRegistry::s_lock_` is a leaf SpinLock (DeathNotify precedent).  The
single direction in ISR context is `scheduler_lock_ → s_lock_` (`on_tick` →
watchdog; `cleanup()` → drain).  In task context (`SYS_PAGER_*`) the registry
lock is taken and **released before** any scheduler call — the wake of a client
uses the DeathNotify "collect under the lock, poke/wake outside it" pattern
with id+gen revalidation.  No code path ever holds `s_lock_` while acquiring
`scheduler_lock_` or a Notify lock → no cycle, even across the two contexts.

### 7.2 Map vs timeout race (MAP_IN_PROGRESS pin)

`SYS_PAGER_MAP` runs in the pager's task context; the watchdog runs in the
timer ISR and is the only preemptor on single-core.  The VMM map must stay
OUTSIDE `s_lock_` (it may allocate PMM page-table pages; FrameUserMap
discipline).  To close the map-vs-timeout TOCTOU:

1. Under `s_lock_`: validate the record (pager match, pending, cap live) and
   set `map_in_progress = true`.
2. Release `s_lock_`; perform the VMM map.
3. Re-acquire `s_lock_`: insert ledger entries + pins, consume the record,
   clear `map_in_progress`; release `s_lock_`; wake the client.

The watchdog, seeing `map_in_progress`, **defers** that record for this tick
(the record is not consumed/unmapped while the map is mid-flight); the generous
`CONFIG_PAGER_FAULT_TIMEOUT_TICKS` makes a multi-tick deferral harmless.
Client `drain_task` cannot race the map: a BLOCKED pager-fault client cannot be
running or reaped concurrently (its cleanup requires it to be RUNNING first,
which requires a wake — which requires the record to be consumed).  SMP note:
the same pin+defer mechanism serializes map vs watchdog on any CPU through
`s_lock_`.

### 7.3 Exactly-once resume & rollback

The record's `PENDING → consumed` transition under `s_lock_` is the single
gate; the wake happens exactly once per fault (B8).  Timeout rollback unmaps
exactly the ledger pages and releases exactly the pins of that fault —
bounded by `CONFIG_PAGER_MAX_PAGES_PER_FAULT` (B7).  A late
`SYS_PAGER_MAP`/`ABORT` on a consumed record returns an error and is a no-op.

### 7.4 UAF / dead-task safety

- Every stored task reference carries `(id, generation)` and is re-resolved
  through `Scheduler::find_task` + live check (magic + `!= TERMINATED/REAPED`,
  DeathNotify `task_live` pattern) before any wake or map.
- Cap pins (§5.2) prevent a freed `FrameCap` dereference; `invalidate_cap`
  covers revoke/dispose.
- Drain ordering in `cleanup()`: `PagerRegistry::drain_task(client)` runs
  before `free_user_pages` (§5.3) and the registry drain for the pager runs
  before its `Notify` is destroyed.

### 7.5 ResourceTracker accounting

New counters, zero-delta across every test (enforced in `any_leak`):
`pager_registrations` (install/evict/unregister), `pager_faults`
(record create/consume), `pager_mappings` (ledger page insert/unmap).
`snapshot_reset` clears the registry, releases pins, and rewinds the counters.

### 7.6 WCET / allocation discipline

The #PF ISR path performs no allocation and no VMM walk beyond the fault
record update — it only touches the static registry and a Notify pulse
(bounded by `CONFIG_CAP_MAX_PAGER_CLIENTS`).  All page-table work is on the
pager's syscall path or the tick watchdog, both bounded (fixed-depth walk, ≤ 4
pages/fault).

## 8. Syscall ABI

Numbers 69–73 (all NON-FAST — `RECV` writes user memory; `MAP` touches
cross-task page tables).

| Syscall | # | Signature | Semantics |
|---|---|---|---|
| `SYS_PAGER_REGISTER` | 69 | `(pager_pid)` → 0/-1 | Client designates its own pager (live, ≠ self); fail-closed when full |
| `SYS_PAGER_RECV` | 70 | `(PagerFault* out)` → 1/0/-1 | Copy next pending fault (never blocks); **validates `out` before consuming** (auditor S2 pattern from death-notify) |
| `SYS_PAGER_MAP` | 71 | `(fault_id, cap_handle, count, flags)` → 0/-1 | Map pager's FrameCap into the client's PML4 at fault_va + complete the fault + wake client |
| `SYS_PAGER_ABORT` | 72 | `(fault_id)` → 0/-1 | Explicit fail: unmap ledger, wake client, poison the VA latch |
| `SYS_PAGER_UNREGISTER` | 73 | `(client_pid)` → 0 | Client removes own pager, or pager drops a client; aborts pending fault |

`MAX_SYSCALL` 69 → 74.

## 9. Integration Points

| File | Change |
|---|---|
| `src/kernel/ipc/pager_registry.{hpp,cpp}` (**new**) | `PagerRegistry` — registration table, fault records, pulse, recv/map/abort/unregister, watchdog scan helper, `drain_task(client/pager)`, `invalidate_cap`, `snapshot_reset`, live-count accessors. Mirrors `death_notify.{hpp,cpp}`. |
| `src/kernel/kernel.cpp` | In the from-user `vector==14` branch (kernel.cpp:1510), BEFORE `deliver_signal_to_user`: `if (pager_try_delegate_fault(t, error_code, regs)) return;` — classifies (§3.2), records + pulses + blocks + reschedules (inside the ISR, exactly-once; the ISR epilogue saves the faulting context, so resume = iretq retry). |
| `src/kernel/task/task.hpp` | New field `PagerFault *blocked_on_pager_fault` (mirrors `waiting_on_*` pattern; set/cleared only under the registry/scheduler discipline) + all memset sites must initialize it (TCB sentinel gotcha, STATE.md). |
| `src/kernel/task/task.cpp` | `cleanup()`: `PagerRegistry::drain_task(*this)` before `free_user_pages` and before the pager's Notify is destroyed. |
| `src/kernel/elf/elf.cpp` | `exec_into_current`: drain the client's registry before swapping the address space (stale PML4 / stale fault VAs). |
| `src/kernel/task/scheduler.cpp` | `on_tick`: watchdog scan (near the canary sampling block, scheduler.cpp:1609) — expire overdue records (§4.6). |
| `src/kernel/syscall/syscall.hpp` + `syscall_handlers_pager.cpp` (**new**) | Five handlers, NOT in `k_syscall_fast[]`, `MAX_SYSCALL` 74. |
| `src/kernel/cap/frame.{hpp,cpp}` | `FrameCap::dispose/revoke` call `PagerRegistry::invalidate_cap`. |
| `src/kernel/memory/vmm.{hpp,cpp}` | Reuse `map_page_in_pml4` (executable=false) + `unmap_frame_from_cap` — no new VMM API required. |
| `src/kernel/nexios_config.h` | `CONFIG_CAP_MAX_PAGER_CLIENTS` (8), `CONFIG_PAGER_FAULT_TIMEOUT_TICKS` (1000), `CONFIG_PAGER_MAX_PAGES_PER_FAULT` (4). |
| `src/kernel/test/resource_tracker.{hpp,cpp}` + `test_isolate.cpp` | `pager_registrations`/`pager_faults`/`pager_mappings` counters + `any_leak` + snapshot restore. |

## 10. Test Strategy (class `cap_pager`, TF_KERNEL, stub-first)

Landed as stubs (JARVIS_TEST_PASS) first, on `testbed`; the registration is the
stub-first gate.  Tests drive **real** user faults (a real-PML4 client task
dereferences an unmapped VA, as `test_cap_shm` builds real-PML4 tasks) plus the
synthetic-frame `exception_dispatch_probe` (#91) where classification ordering
is exercised via the live dispatcher.

| Test | Asserts |
|---|---|
| `pager_register_authority` | client designates own pager OK; third-party designation rejected; self-pager rejected; full registry fails closed |
| `pager_fault_roundtrip` | client faults on unmapped VA; pager RECVs `{id, va, flags}`, MAPs a FrameCap; client instruction completes; PTE present in client PML4; record consumed; ResourceTracker zero-delta |
| `pager_fault_timeout_aborts` | pager never responds; watchdog expires the fault; client re-faults on the same VA → SIGSEGV/terminates; registration **kept** (a different VA still delegates); ledger unmapped (no PTE in client PML4) |
| `pager_map_after_timeout_denied` | late `SYS_PAGER_MAP` returns error; no mapping installed; no double wake |
| `pager_abort_poisons_va` | `SYS_PAGER_ABORT` → client re-faults once → SIGSEGV (no re-delegate loop); a different VA still delegates |
| `pager_dead_drains_faults` | pager dies mid-fault: client woken, registration evicted, ledger unmapped, cap pins released |
| `pager_client_death_unmaps` | client dies with a pager-mapped page: `drain_task` clears the PTE before `free_user_pages`; pager's FrameCap stays live |
| `pager_cap_revoke_unmaps` | pager's FrameCap revoked while fault-mapped: `invalidate_cap` unmaps the client PTE + drops pin |
| `pager_deadlock_designed_out` | **the footgun regression:** a pager that parks/stalls itself cannot wedge the kernel — while the pager never replies, other tasks (incl. a harness progress counter) keep running and the timeout fires |
| `pager_recv_nonblocking_none` | empty RECV returns 0 immediately (never blocks) |
| `pager_recover_ip_not_delegated` | a fault inside a `g_user_access_recover_ip` window is NOT delegated; the copy returns false |
| `pager_classification_rejects` | P=1 (write-to-RO), I/D=1, RSVD, and kernel-mode #PFs are not delegated (real RO-write fault + probe-assisted frames) |
| `pager_smap_canary_coexist` | canary sampling still fires for a pager-served task; `SYS_PAGER_MAP` refuses a canary-slot VA collision |

Validation: `make execute-test x86_64 debug cap_pager`, then the regression
classes (cap_death, cap_shm, syscall_fastpath, cap_mmio_user, cap_irq_notify,
cap_core, cap_lifecycle, cap_syscall, cap_untyped, ipc_robustness), then
debug `all` + release `all`, test-history rows per AGENTS.md.

## 11. OPEN QUESTIONS

1. **Executable pager mappings (I/D faults).**  v1 makes `SYS_PAGER_MAP`
   NX-only and I/D=1 faults non-delegable (§2.2).  If demand-paged executable
   pages are needed, a rights review (which capability right authorizes an
   executable cross-task map? today `map_page_in_pml4` has no exec right gate)
   is required before v2.  **Blocks nothing in v1.**
2. **Timeout policy — RESOLVED: per-fault fail-closed, registration KEPT.**  The
   deadline is a hard bounded latency; the watchdog consumes the record, unmaps
   the ledger, sets the poisoned-VA latch (F9), and wakes the client (never
   stuck, ≤ timeout + map window regardless of pager liveness).  THIS fault
   reverts to SIGSEGV, but the registration survives — a transient scheduling
   stall does not permanently disable paging; eviction happens only on pager
   death or explicit unregister.  Priority invariant B9: a pager that cannot
   outrank its clients will time every fault out; deployment must ensure the
   pager's effective priority ≥ its clients'.  See §4.6/§4.7.
3. **Client wakeup with pending signals — RESOLVED: retry-first.**  The faulting
   instruction re-executes before any pending signal is delivered (signal
   delivery at the next syscall/exception boundary).  Hard-real-time rationale:
   deterministic completion of the access the pager already satisfied, no
   signal-storm livelock, fixed WCET.  See §4.3.
4. **Dedicated blocked state — RESOLVED: BLOCKED + `blocked_on_pager_fault`
   field.**  Reuses `TaskState::BLOCKED` with a `waiting_on_*`-style field
   (matches the existing pattern, minimal state-transition audit burden); a
   dedicated `TaskState` is a documented follow-on for debugger observability.
5. **Map-then-faulted-elsewhere.**  If a pager maps pages for fault A and the
   client's instruction faults on a *different* page before the first PTE is
   used, the second fault creates a second record and a second MAP.  Confirm
   per-page round-trips (no batched/vectored fault delivery) are acceptable for
   v1.
6. **SMP.**  The design is single-core today; §7.2 documents the SMP-safe pin.
   Confirm SMP is out of scope for the implementation (tracked by #85).
7. **Poisoned-VA latch lifetime — RESOLVED.**  One aborted-VA latch per
   registration, shared by `SYS_PAGER_ABORT` and the watchdog timeout (§4.5);
   cleared on the next successful delegation of a different VA or on
   unregister.  A permanent latch is a follow-on (needs a VA-reuse policy).

## 12. Non-Goals (reprise)

Kernel swap-on-demand; COW; kernel-memory demand paging; executable pager
mappings (v1); multi-pager / pager chaining; user-configurable timeouts;
kernel-task paging; SMP implementation; FAST-path membership for `SYS_PAGER_*`;
any change to `g_user_access_recover_ip`, SMAP, the canary sampler, or the
#PF signal/panic paths.