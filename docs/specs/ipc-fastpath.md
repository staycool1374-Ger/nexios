# In-Register IPC Fastpath — Register-Passing SEND/RECEIVE/SEND_SYNC (issue #11)

**Doc ID:** NEX-SPEC-2026-09-04-011
**Status:** DESIGN PAPER — decisions locked (user sign-off + developer
hard-RT review); no implementation before audit
**Milestone target:** v0.4.3 (High-Performance Zero-Copy IPC & Fault Isolation)
**Related:** `docs/specs/ipc.md` (mailbox model, blocking contract, H2 deferred
switch), `docs/specs/syscall-fastpath.md` (FAST/FULL tiered dispatch, canary
relocation, ONE-audited-dispatcher rule), `docs/specs/boundary.md`
("validate at the boundary, once", MP-3/MP-4),
`src/kernel/syscall/syscall_handlers_ipc.cpp`,
`src/kernel/syscall/syscall.hpp`, `src/kernel/ipc/ipc.cpp`,
`src/kernel/ipc/ipc.hpp` (IPC::send_sync gains a reply-clamp parameter, §3.9),
`src/kernel/task/task.hpp` (MessageQueue struct — hosts the new
`pop_clamped`/`find_best_index` members, §3.9),
`src/kernel/arch/x86_64/isr_stubs.asm`, `src/libc/syscall.h`.
**High-risk:** the fastpath must NOT weaken isolation or the blocking
contract.  The named design element is the **boundary-preserving register
gather/scatter** (§3.4) — the fast IPC handler is the existing audited
handler with the user-copy sections *swapped* for register-gather/scatter,
never a new validation path.

## 0. Issue reconciliation (issue #11)

The issue stub is sparse and its register list is mangled ("rcx, rdx, rsi,
r8, r9…").  This paper grounds every claim in the live source.  Four
findings shape the design:

1. **The mangled register list is a ghost of the dead `syscall_entry.asm`.**
   Its argument shuffle (`mov rsi, rbx; mov rdx, rcx; mov rcx, rdx;
   mov r8, rsi; mov r9, rdi`) is **DEAD CODE** (unassembled, `mk/rules.mk`;
   the file's own header documents the P7/#6 LSTAR/GS-base landmine).
   The **live** entry is `int $0x80` → trap gate `isr_128` → `isr_common`
   → `handle_interrupt_c` → `syscall_handler` → `Syscall::handle`
   (kernel.cpp:1623-1625).  The live ABI maps `regs[0]=rax=number,
   regs[1]=rbx=arg0, regs[2]=rcx=arg1, regs[3]=rdx=arg2, regs[4]=rsi=arg3`
   (libc `__syscall5`, syscall.h:78-91).
2. **"Skipping full TCB context saves" has no literal counterpart in the
   kernel.**  There is no per-syscall callee-saved TCB save: `isr_common`
   pushes all 15 GPRs exactly once (isr_stubs.asm:132-146) and the
   deferred-switch machinery saves **only RSP** (`mov [rax], rsp`,
   isr_stubs.asm:252), never callee-saved GPRs (they live in the task's own
   stack frame).  The correct reading is *avoiding the user-buffer round
   trip and rejoining the FAST class* — see §3.1.
3. **IPC cannot join FAST today because SEND/RECEIVE/SEND_SYNC deref user
   buffers** (`checked_ptr` read loop, `safe_copy_to_user`, reply write
   loop — syscall_handlers_ipc.cpp:46-51/110-112/142-143).  A
register-passing variant is **pointer-free by construction**, which is
    exactly the audited re-entry condition the FAST spec demands
    (syscall-fastpath.md §3.1/§4).  This is the "pointer-deref review" that
    the tiered-dispatch paper deferred.
4. **`MessageQueue` has NO non-destructive primitive.**  `pop()`
   (ipc.cpp:102-155) is destructive (best-priority scan + removal + gap
   compaction); `IPC::recv` = `pop` (ipc.cpp:280-287) and `send_sync` ends
   with `pop(reply)` (ipc.cpp:421).  The fail-closed oversized-message rule
   therefore needs a **new primitive, `MessageQueue::pop_clamped`**, added
   by this paper — not present today (§3.9).

## 1. Current State (verified)

- **Live IPC syscalls** (syscall_handlers_ipc.cpp):
  - `sys_send(dest_id, data_ptr, type, data_size)` — `checked(data_ptr,
    size)`, per-byte `data.read(i)` into a `Message`, then
    `IPC::send(dest_id, msg, flags)`.
  - `sys_receive(max_size, timeout)` — `checked(buf, max_size)`, block loop
    (`IPC::recv` + VULN-W3 deadline + sporadic-server completion/activation
    + `dequeue_ready` + `reschedule` + `sti/hlt/cli`), then
    `safe_copy_to_user` (stac/clac + `g_user_access_recover_ip` window).
  - `sys_send_sync(dest_id, data_ptr, type, data_size)` — `checked` read
    loop, `IPC::send_sync` (blocking reply wait, ipc.cpp:368-422), then a
    per-byte reply write back through the `checked` ptr.
- **Message model** (task.hpp:53-60): `{sender_id, type, priority,
  data[IPC_MAX_MSG_SIZE], data_size, buf_handle}`; `IPC_MAX_MSG_SIZE` =
  `CONFIG_IPC_MAX_MSG_SIZE` = **64**; queue = `Message msgs[16]` per TCB,
  priority-ordered pop, spinlock-protected.  `MessageQueue` is embedded in
  the destination TCB (`tcb->msg_queue`).  Transport is **store-and-forward
  and time-decoupled**: `IPC::send` always `push`es to the queue and wakes a
  BLOCKED receiver (`set_task_ready`); a blocked receiver is still resumed
  through its own queue, never directly written (ipc.cpp:241-276).  The
  receiver pops **into its own context**.  `MessageQueue` exposes only the
  destructive `pop()` (ipc.cpp:102-155) — there is no non-destructive peek;
  the new `pop_clamped` primitive (§3.9) is required for the fail-closed
  oversized rule.
- **Blocking contract** (ipc.md §1/§4, ipc.cpp:495-540): full queue →
  `block_sender` (BLOCKED + dequeue + priority-inheritance boost under
  `q.lock_` + IrqGuard) → `reschedule()` → spin-wait.  The H2 deferred-switch
  machinery (generation-lock, scratch-save healing, apply-side RSP-owner
  check, isr_stubs.asm:189-331) carries every block.  **This machinery is
  untouched by this design.**
- **Authority model:** the live SEND path uses **task-ID ambient authority**
  (`Scheduler::find_task(dest_id)` + not-TERMINATED, ipc.cpp:184-186).
  `send_via_cap`/`recv_via_cap` exist as kernel-internal APIs
  (cap::Endpoint) but are **not exposed as syscalls**; `sys_create_mailbox`
  is a stub returning 0.  There is no cap-gated SEND syscall to match
  against today.
- **Dispatch:** `Syscall::handle` bounds-checks, then tests
  `SYSCALL_FAST_MASK` and dispatches inline without the canary walk
  (syscall.cpp:124-158); `k_syscall_fast[]` is the single source of truth.
  The debug-only MP-4 AC-leak check runs on **both** paths.  Canary detection
  for FAST-only tasks is relocated to context-switch + tick sampling.
- **Register frame:** the trap gate preserves all 15 GPRs; the kernel already
  writes the caller's own `regs[]` frame for signal delivery
  (`regs[17] = ...`, kernel.cpp:1358) — writing the caller's own frame is an
  established mechanism.

## 2. Problem

Hard-real-time framing makes **worst-case** IPC latency the metric that
matters (syscall-fastpath.md §2; external-pager.md §hard-RT framing).
Today every SEND/RECEIVE/SEND_SYNC pays, on entry:

1. the FULL-path canary walk (page-table reads, production too),
2. `checked_ptr` range validation + a per-byte user read loop with SMAP
   discipline and a fault-recovery window on the sender,
3. `safe_copy_to_user` (stac/clac + `g_user_access_recover_ip`) on the
   receiver and `send_sync` reply copy-out,
4. two user-buffer round trips (copy-in, copy-out) on top of the queue's
   `Message` copy.

For short control messages (status pings, capability handles, small
commands) all four are pure overhead: the payload fits in the registers the
trap gate has **already saved**.  Worst-case IPC latency is dominated by
bounded-but-large validation + copy work, not by the queue transfer itself.
The fastpath removes 1-4 for the ≤-budget case while leaving the blocking
contract byte-for-byte identical.

## 3. Design

### 3.1 Interpretation of "skip full TCB context saves" (RESOLVED)

The phrase is not implementable literally, and this paper rejects the
"reduced-context-switch" reading with source evidence:

| Claim | Evidence |
|---|---|
| There is no per-syscall callee-saved TCB save | `isr_common` pushes all 15 GPRs once at trap entry (isr_stubs.asm:132-146); the deferred switch saves only `rsp` (isr_stubs.asm:252); sysret path is dead (syscall_entry.asm:18). |
| A "reduced" context switch would change blocking semantics | The scheduler switch cost is fixed and carries the IPC contract (ipc.md §4). Touching it would re-open the H2 race surface. **Not in scope.** |
| The real win is the user-buffer round trip + canary walk | Payload lives in the already-saved `regs[]` frame (kernel memory). No `checked_ptr`, no `safe_copy_to_user`, no SMAP window, and a pointer-free handler is eligible for the FAST class (skips the canary walk). |
| Cross-task "deliver into the receiver's live register frame" is **unsafe and impossible** in the mailbox model | The transport is store-and-forward and time-decoupled: a sender pushes to `tcb->msg_queue` (ipc.cpp:241), and the receiver's `regs[]` frame exists only while *it* is inside its own syscall. Writing a peer's frame would require a rendezvous the mailbox model does not provide. |

**Correct interpretation:** the fastpath is a **register-passing variant of
SEND/RECEIVE/SEND_SYNC**.  The sender's payload is gathered from its own
`regs[]` frame; the receiver's payload is scattered into its own `regs[]`
frame.  Only the **caller's own** kernel-stack frame is ever read or written
— the same trust relationship the kernel already exercises for signal
delivery (kernel.cpp:1358).  "Skipping full TCB context saves" is therefore
better read as *"skipping the full user-register round trip"*: the message
never has to be saved to / restored from a user buffer because it stays in
the register context.

### 3.2 Syscall ABI and register budget

Three new syscall numbers (NON-cap, task-ID ambient authority, matching
today's SEND/RECEIVE):

| Syscall | # | Signature (handler args) | Payload source/dest |
|---|---|---|---|
| `SEND_FAST` | 74 | `(dest_id, type, data_size, w0)` + words in `regs[]` | gather from caller's `regs[]` |
| `RECV_FAST` | 75 | `(max_size=arg2, timeout_ticks=arg3)` — mirrors `sys_receive` arg2/arg3 slots | scatter into caller's `regs[]`; `msg.type` in `rax` |
| `SEND_SYNC_FAST` | 76 | `(dest_id, type, data_size, w0)` + words in `regs[]` | gather request, scatter reply |

`MAX_SYSCALL` 74 → **77**.

**Register map** (x86_64, live `int $0x80` ABI; user C sets these via
extended-asm register constraints in a `__syscall_fast_*` wrapper):

| Carrier | regs[] idx | SysV/asm role | Payload |
|---|---|---|---|
| `rax` | 0 | number | `SEND_FAST`/`SEND_SYNC_FAST` = 74/76; `RECV_FAST` = 75; return `msg.type` on all |
| `rbx` | 1 | arg0 | `dest_id` (SEND/SEND_SYNC); unused (0) on RECV |
| `rcx` | 2 | arg1 | `type` (SEND/SEND_SYNC); unused (0) on RECV |
| `rdx` | 3 | arg2 | `data_size` (SEND/SEND_SYNC); `max_size` (RECV) |
| `rsi` | 4 | arg3 | payload word 0 (SEND/SEND_SYNC); `timeout_ticks` **in** then payload word 0 **out** (RECV — the timeout is read before the scatter, in/out reuse per §3.2 wrapper contract) |
| `rdi` | 5 | (preserved) | payload word 1 |
| `r8`  | 7 | (preserved) | payload word 2 |
| `r9`  | 8 | (preserved) | payload word 3 |
| `r10` | 9 | (preserved) | payload word 4 |
| `r11` | 10 | (preserved) | payload word 5 |

**Budget: `CONFIG_IPC_FAST_PAYLOAD_BYTES` = 48** (6 words × 8 B; words 0-5
in `rsi/rdi/r8/r9/r10/r11`).  `rbp` (idx 6) is **deliberately excluded** to
stay correct under both `-fomit-frame-pointer` and default frame-pointer
builds of user code.  `rcx/rdx` (the issue's literal list) are unavailable
for payload — they carry `type`/`data_size` in the live ABI; the mangled
list is from the dead asm (§0.1).  The trap gate preserves every listed
register, so no user-side save/restore beyond the wrapper's asm operands is
needed.

**Wrapper contract:** `SEND_SYNC_FAST` uses in/out operands (`+S`, `+D`,
`+r8`, …) — the request payload is consumed by the kernel *before* it
writes the reply into the same slots.  `RECV_FAST` declares `rsi` as in/out
(`+S`: `timeout_ticks` in, payload word 0 out) and the other payload regs as
outputs only.  `data_size = 0` is valid (empty payload); `msg.type`
is always returned in `rax`.  On any failure the kernel returns `-1`
(fail-closed); payload regs are undefined on failure and must not be read.

### 3.3 Path diagram

```
SEND_FAST (int $0x80)
  └─ Syscall::handle: bounds check → FAST bit set → inline dispatch (no canary walk)
      └─ sys_send_fast:
           1. data_size <= CONFIG_IPC_FAST_PAYLOAD_BYTES ? else -1
           2. Message m{}; m.type=arg1; m.data_size=arg2; m.sender_id=cur->id
           3. gather payload words arg3 + regs[5]/regs[7..10] → m.data   (kernel mem → kernel mem)
           4. IPC::send(dest_id, m, 0)      ← existing authority + blocking, unchanged
              ├─ queue has space → push (wake receiver if BLOCKED) → return 0
              └─ queue full → block_sender (PI boost) → reschedule → spin-wait
                              → resumes, re-lookup dest, push → return 0/-1

RECV_FAST (int $0x80)
  └─ Syscall::handle: FAST bit → sys_recv_fast:
     clamp = min(max_size, CONFIG_IPC_FAST_PAYLOAD_BYTES)
     for (;;) {
       ok = cur->msg_queue.pop_clamped(msg, clamp)      // ONE lock acquisition
       if (ok)  break;                                  // fit → scatter below
       if (!cur->msg_queue.is_empty())  return -1;      // best is oversized → stays queued
       // empty → block EXACTLY as sys_receive (VULN-W3 deadline,
       // sporadic completion/activation, BLOCKED + dequeue_ready +
       // reschedule + sti/hlt/cli)                      ← block body verbatim
     }
     if (was_blocked) { remaining_ticks reset; sporadic on_activation }
     scatter msg.data → regs[4]/regs[5]/regs[7..10]     (kernel mem → kernel mem)
     return msg.type

SEND_SYNC_FAST (int $0x80)
  └─ Syscall::handle: FAST bit → sys_send_sync_fast:
     1. data_size <= budget ? else -1; gather request → m (BEFORE any block)
     2. IPC::send_sync(dest_id, m, reply, reply_max_size = budget)
          └─ existing blocking loop unchanged; the final pop(reply)
             (ipc.cpp:421) becomes pop_clamped(reply, budget) — a fit is
             delivered; an oversized reply STAYS queued and send_sync
             returns false
     3. if !ok → -1 (peer-dead OR oversized reply — both fail-closed; caller
        drains any queued reply via a subsequent full RECEIVE)
     4. scatter reply → regs[4]/regs[5]/regs[7..10]; return reply.type
```

### 3.4 Validation / isolation / boundary (MP-3 / MP-4 preserved)

| Concern | Fastpath behaviour |
|---|---|
| User-pointer deref | **None, structurally.** Payload is gathered/scattered only in the caller's own kernel-stack `regs[]` frame. `regs[]` is kernel memory owned by the current task's syscall entry; it is validated by the hardware trap (privilege change) and already trusted by `handle_interrupt_c` (it is the ISR frame itself). |
| Boundary ("validate at the boundary, once") | The **only** new validation is `data_size <= CONFIG_IPC_FAST_PAYLOAD_BYTES` (register compare, before any queue access) and the `max_size` clamp on RECV. The queue authority/blocking paths are the existing `IPC::send`/`IPC::recv` machinery; the only queue-side addition is `pop_clamped` (§3.9) — a selection+removal primitive with a size clamp, NOT a second validation path. |
| Cap / authority | **Audited alternative:** the fastpath binds to the same task-ID ambient authority as `SEND` — `IPC::send` performs `find_task(dest_id)` + liveness (ipc.cpp:184-186), self-send guard, and the full-queue block. When a cap-gated SEND syscall (via `send_via_cap`/Endpoint) lands, the fast variant must be gated by the same `cap::lookup` + Endpoint validation or stay NON-FAST (INV-7). |
| MP-3 (canary) | FAST membership means the canary walk is skipped at entry — acceptable because nothing is dereferenced; MP-3 detection is preserved by the scheduler context-switch/tick sampling (syscall-fastpath.md §4, same as every FAST member). |
| MP-4 (SMAP/AC) | No user access → no `stac`/`clac` and no `g_user_access_recover_ip` window on the fastpath. The debug-only AC-leak detector remains on both paths (syscall.cpp:129-138). |
| Isolation | A malicious caller can only ever have its **own** `regs[]` frame read/written by the kernel; no peer frame, no user buffer, no foreign PML4 is touched. Shorter attack surface than the full path (no checked_ptr, no copy window). |

### 3.5 Blocking semantics and hard-RT (unchanged contract)

- **SEND_FAST to a full mailbox blocks exactly like SEND.**  The payload is
  gathered into a kernel-stack `Message` **before** `IPC::send`, so the
  block is identical (BLOCKED + dequeue + PI boost + reschedule +
  spin-wait); the `regs[]` frame is read-only after the gather.  No
  priority inversion is introduced (the PI boost lives in
  `block_sender`, unchanged).  The H2 deferred-switch machinery is not
  touched — the fastpath is a drop-in caller of the same blocking IPC.
- **RECV_FAST on an empty queue blocks exactly like RECEIVE** (the mirror
  loop in §3.3 is `sys_receive`'s, verbatim, including the VULN-W3 deadline
  and sporadic-server bookkeeping).
- **Oversized-message rule (fail-closed):** RECV_FAST delivers via
  `MessageQueue::pop_clamped` (§3.9) — one atomic lock acquisition removes
  the best-priority message ONLY if `data_size ≤ clamp`.  An oversized best
  is **not consumed** (stays queued) and RECV_FAST returns `-1` for the full
  `RECEIVE` to drain.  SEND_FAST / SEND_SYNC_FAST reject `data_size > budget`
  with `-1` before any queue access; a SEND_SYNC_FAST oversized **reply**
  also stays queued (§4 INV-5).
- **Worst-case bound:** for `data_size ≤ budget`, every operation the
  fastpath performs is a strict subset of the full path's operations
  (dispatch → bounds → Message build → queue push/pop → block machinery),
  **minus** the checked_ptr walk, the copy-in/copy-out loops, the SMAP
  windows, and the canary walk.  Hence `WCET(SEND_FAST) ≤ WCET(SEND)` and
  `WCET(RECV_FAST) ≤ WCET(RECV)` structurally; blocking-case WCET is
  identical to the full path (the block dominates and is unchanged).
  Measured evidence per the #101/#102 relative methodology (§5
  `fast_latency_vs_full`).

### 3.6 Assembly vs C (ONE audited dispatcher)

**C-level fast dispatch — no new asm entry.**  `SEND_FAST`/`RECV_FAST`/
`SEND_SYNC_FAST` are **pointer-free**, so they join `k_syscall_fast[]` and
`Syscall::handle`'s existing FAST-bit test dispatches them inline without
the canary walk.  This requires: (a) a pointer-deref review of the three
new handlers (this paper IS that review — they contain no user-pointer
access), (b) adding the three numbers to `k_syscall_fast[]` (single source
of truth; `SYSCALL_FAST_MASK` derives automatically), (c) `MAX_SYSCALL`
74 → 77.  No `syscall_entry.asm` revival, no duplicated bounds/AC/signal
checks in assembly — exactly the syscall-fastpath.md §3.2 rule.  The
gather/scatter helpers operate on the `regs[]` the handler already
receives; `handle_fast` is untouched.

### 3.7 What stays FULL

Anything touching user memory or cross-task state stays on the FULL path,
unchanged:

- `SEND`/`RECEIVE`/`SEND_SYNC` (pointer-based) — unchanged, still FULL.
- `RECV_FAST` falling back for oversized messages (the fallback is the
  existing `RECEIVE`, not a second fast path).
- BufferPool transfer (`msg.buf_handle`) — not in the register fastpath
  (needs cap/VA semantics); SEND_FAST always sends `buf_handle = 0`.
- Scatter/gather / vectored IPC, async notification variants, multi-word
  messages > 48 B — non-goals (§6).

### 3.8 Config and numbering

| Item | Value |
|---|---|
| `CONFIG_IPC_FAST_PAYLOAD_BYTES` | 48 (must be ≤ `CONFIG_IPC_MAX_MSG_SIZE` = 64; static_assert) |
| `SEND_FAST` | `SyscallNumber` 74 |
| `RECV_FAST` | `SyscallNumber` 75 |
| `SEND_SYNC_FAST` | `SyscallNumber` 76 |
| `MAX_SYSCALL` | 74 → 77 |
| libc (`src/libc/syscall.h`) | `SYS_SEND_FAST 74`, `SYS_RECV_FAST 75`, `SYS_SEND_SYNC_FAST 76`; `__syscall_fast_send/recv/send_sync` wrappers with `+S/+D/+r8/+r9/+r10/+r11` operands |

The three numbers join `k_syscall_fast[]` — the `fast_mask_matches_config`
gate (syscall-fastpath.md §5 test 1) re-validates membership at build time.

### 3.9 New MessageQueue primitive — `pop_clamped` (added by this paper)

Today `MessageQueue` exposes only destructive `pop()` (ipc.cpp:102-155):
best-priority scan, removal, and gap compaction under one lock acquisition.
`IPC::recv` = `pop` (ipc.cpp:280-287); `send_sync` ends with `pop(reply)`
(ipc.cpp:421).  The fail-closed oversized rule (§3.5) cannot be expressed on
top of that API — a destructive pop can never "look without taking".

**Chosen primitive (one clean step, zero TOCTOU):**

```cpp
// task.hpp — MessageQueue
bool pop_clamped(Message &out, uint32_t max_size);
```

Semantics under **one** `lock_` acquisition (same discipline as `pop()`):

1. empty → return `false` (nothing removed);
2. `find_best_index()` (shared helper below) → `best`;
3. `msgs[best].data_size > max_size` → return `false` **without removing**
   (the oversized best stays queued);
4. else remove `best` (identical removal/compaction to `pop()`), copy into
   `out`, return `true`.

**Anti-drift invariant (INV-P):** the best-priority scan in `pop()` is
extracted into a private `size_t find_best_index() const` (precondition:
caller holds `lock_`; returns `IPC_MAX_QUEUE_MSG` when empty).  `pop()` and
`pop_clamped()` MUST both call it — selection can never drift (enforced by
construction; asserted by `fast_pop_clamped_matches_pop_selection`).

**Empty-vs-oversized disambiguation:** `pop_clamped` returns `false` for
both.  The caller is the **sole consumer of its own queue** (producers only
push; no peer task can pop the current task's `msg_queue`), so a
pre-check `is_empty()` (atomic relaxed count) disambiguates: empty → block
(§3.3 RECV_FAST loop); non-empty + `false` → the best is oversized → fail
closed without consuming.

**A two-step peek-then-pop was considered and REJECTED:** two separate lock
acquisitions between a non-destructive peek and a destructive pop reopen a
TOCTOU window on `count` and on the selected index; the single-lock
`pop_clamped` removes that hazard structurally.

**`send_sync` reply clamp:** `IPC::send_sync(dest_id, msg, reply,
uint32_t reply_max_size = 0)` (ipc.hpp).  The final `pop(reply)`
(ipc.cpp:421) becomes `pop_clamped(reply, reply_max_size)` when clamped;
default `0` keeps the FULL path byte-identical (plain `pop`).  The FULL-path
handlers (`sys_receive`, `sys_send_sync`) are unchanged — `pop_clamped` is
used only by the three fast handlers.

## 4. Semantics / Invariants

- INV-1: `SEND_FAST`/`SEND_SYNC_FAST` accept `data_size ≤
  CONFIG_IPC_FAST_PAYLOAD_BYTES`; a larger `data_size` returns `-1` before
  any queue access.  `RECV_FAST` never delivers more than `min(max_size,
  budget)` bytes.
- INV-2: the fastpath performs **no user-pointer dereference** — payload is
  read/written only in the caller's own `regs[]` frame (enforced by the
  pointer-free handler review + FAST-mask membership gate, syscall-fastpath
  §4).
- INV-3: blocking semantics are identical to the full path: SEND_FAST on a
  full queue blocks via the existing `block_sender` machinery, and RECV_FAST
  on an empty queue via the existing `sys_receive` block body (WEDGE-
  invariant, PI boost, deferred switch, H2 guards) — no new blocking code
  exists to audit.  *Auditor S3 note:* for a **kernel-context** caller the
  RECV_FAST block body adds `else { arch::hlt(); }` (the user-task
  `sti/hlt/cli` is `is_user_`-gated, and the `hlt` mirrors the already-
  audited `send_sync` kernel-task pattern, ipc.cpp §send_sync).  Safe under
  IF=1 (scheduler-mediated); the full-path `sys_receive` lacks the kernel-
  `hlt` and is left untouched.
- INV-4: RECV_FAST **never consumes** an oversized message — delivery is via
  `pop_clamped` (§3.9), which removes the best-priority message only if
  `data_size ≤ clamp`; an oversized best stays queued (`pop_clamped` false
  on a non-empty queue) and RECV_FAST returns `-1` for the full `RECEIVE`.
  *Auditor S3 note:* a concurrent producer push between `pop_clamped`'s lock
  release and the caller's `is_empty()` re-read yields a spurious `-1` with
  the message preserved (fail-safe; the design's sole-consumer model §3.9
  makes this benign — no peer can have removed the caller's own message).
- INV-5: SEND_SYNC_FAST gathers the request **before** `IPC::send_sync`
  (the frame is read-only after the gather, so a block cannot invalidate the
  payload); the reply pop is clamped (`reply_max_size = budget`).  A fitting
  reply is delivered in the caller's registers; an oversized reply **stays
  queued** and the call returns false → `-1` (fail-closed), drained by a
  subsequent full `RECEIVE`.  The unclamped default path
  (`reply_max_size = 0`) is byte-identical to today.
- INV-6: `regs` is required to be non-null (real `int $0x80` always
  supplies it); kernel-context callers fabricate a frame (as
  `fast_call_correctness` does).  Null `regs` → `-1`.
- INV-7: authority is identical to the full path — `dest_id` validated by
  `IPC::send` (find_task + liveness).  If/when a cap-gated SEND syscall
  lands, the fast variant is gated by the same cap validation or remains
  NON-FAST (no capability-less bypass).
- INV-8: MP-4 debug AC-leak detector applies (entry check, both paths); the
  fastpath itself opens no AC window.
- INV-9: `data_size = 0` sends an empty payload; `msg.type` is always
  returned in `rax`; `-1` on failure with payload registers undefined.
- INV-P (anti-drift): `pop()` and `pop_clamped()` MUST select the same
  message for the same queue state — both call the shared `find_best_index()`
  helper (§3.9); asserted by `fast_pop_clamped_matches_pop_selection`.

## 5. Test Plan (class `ipc_fastpath`, TF_KERNEL, house style)

Kernel-context drivers (real dispatch, real `regs[]` frames, real queues) —
the test_isolate snapshot/restore discipline applies (ResourceTracker
zero-delta asserted).

| Test | Asserts |
|---|---|
| `fast_mask_membership` | the three new numbers are in `SYSCALL_FAST_MASK`, each `< MAX_SYSCALL`, popcount == list size (the existing `fast_mask_matches_config` gate extended). |
| `fast_abi_register_layout` | the gather/scatter helpers map word *i* to the documented regs[] index (unit test against the §3.2 table). |
| `fast_pop_clamped_matches_pop_selection` | **primitive parity:** for mixed-priority queues, `pop()` and `pop_clamped()` (clamp ≥ all sizes) select and remove the same messages in the same order (`find_best_index` anti-drift, INV-P); a clamp smaller than the best leaves it queued (`is_empty()` still false). |
| `fast_send_rejects_oversize` | `data_size > budget` → `-1`; destination queue count unchanged. |
| `fast_send_receive_roundtrip` | real sender task `SEND_FAST`s a ≤48-B payload; receiver `RECV_FAST`s it; payload byte-for-byte across all 6 words, `type` in `rax`, `sender_id` preserved; ResourceTracker zero-delta. |
| `fast_recv_oversized_stays_queued` | **clamp path:** oversized (64-B) + small message in queue; RECV_FAST → `-1`, oversized **not consumed** (still queued); full `RECEIVE` then drains both; order/priority preserved. |
| `fast_send_sync_oversized_reply_stays_queued` | peer replies with `> 48 B` to SEND_SYNC_FAST → `-1`; the oversized reply **remains in the sender's queue**; a subsequent full `RECEIVE` drains it (INV-5). |
| `fast_send_full_queue_blocks` | SEND_FAST to a full mailbox blocks exactly like SEND: sender BLOCKED + dequeued, owner PI-boosted, receiver drains, sender resumes, completes; no WEDGE, no inversion, no ResourceTracker delta. |
| `fast_recv_empty_blocks` | RECV_FAST on empty queue blocks until a sender delivers; wake + priority restore correct. |
| `fast_send_sync_roundtrip` | SEND_SYNC_FAST: request payload delivered to peer, reply delivered in caller's registers, `reply.type` in `rax`. |
| `fast_authority_same_as_send` | SEND_FAST to a dead/TERMINATED task → `-1`; self-send to full queue refused; identical outcome set to full SEND for the same inputs. |
| `fast_no_user_deref_canary` | the three fast handlers are driven from harness context (real FAST dispatch, fabricated regs[]): they complete without faulting and return the pointer-free fail-closed results (SEND_FAST to a non-existent task → `-1`; RECV_FAST empty + 1-tick deadline → `-1`, harness never blocks).  Canary-skip for the three members is inherited structurally from FAST membership (the handler review proves pointer-freedom), NOT asserted via a tampered-canary user task (auditor S3 note — weaker than the initial draft's intent). |
| `fast_latency_vs_full` | relative latency, #101/#102 methodology: `avg(SEND_FAST) ≤ avg(SEND) * 2` (fair fail-closed pair: both to an invalid dest → `-1`) with a magnitude-sanity canary; no absolute cycle bound (TCG quantization). |
| `fast_hybrid_mixed_queue` | interleaved fast + full sends/receives on one mailbox preserve FIFO/priority order and both drain correctly. |

Validation: `make execute-test x86_64 debug ipc_fastpath`, then the
regression classes (ipc_blocking, ipc_robustness, syscall_fastpath, cap_ipc,
cap_pager, cap_death, cap_shm), then debug `all` + release `all`,
test-history rows per AGENTS.md.

## 6. Non-Goals

- No new asm syscall entry and no `syscall_entry.asm` revival; tiered
  dispatch stays in C with the single audited dispatcher.
- No change to the mailbox model, blocking semantics, priority inheritance,
  the H2 deferred-switch machinery, or the queue layout.
- No cross-task register-frame delivery (rejected in §3.1 — store-and-
  forward decoupling makes it impossible; a true rendezvous is a separate
  feature with a new blocking model).
- No "reduced context-switch cost" receiver wake (the scheduler switch is
  fixed and contract-carrying; not in scope).
- No messages > 48 B on the fastpath; no scatter/gather; no BufferPool
  transfer via the fastpath; no async/notification variants.
- No cap-gated fast SEND (follow-on when `send_via_cap` becomes a syscall).
- No FPU/SIMD payload registers (XMM/ymm are not preserved by the trap
  gate; register payloads are GPR-only).
- No change to `g_user_access_recover_ip`, SMAP, canary sampling, or the
  FULL-path IPC handlers.

## 7. Decisions (locked — user sign-off + developer hard-RT review)

All six open questions are RESOLVED; the design above reflects the locked
values.

| # | Question | Locked decision |
|---|---|---|
| Q1 | Register budget | **48 B** via `rsi/rdi/r8/r9/r10/r11` (6 words), `rbp` excluded. |
| Q2 | Syscall ABI | **Distinct numbers** `SEND_FAST=74`, `RECV_FAST=75`, `SEND_SYNC_FAST=76`; `MAX_SYSCALL` 74 → 77. |
| Q3 | `SEND_SYNC_FAST` scope | **In v1 scope.** |
| Q4 | Oversized reply | **Fail-closed:** return `-1` and leave the reply queued for a subsequent full `RECEIVE` (INV-5). |
| Q5 | FAST membership | **Approved** — pointer-free handlers join `k_syscall_fast[]`; MP-3 via scheduler canary sampling. |
| Q6 | Config | `CONFIG_IPC_FAST_PAYLOAD_BYTES` = 48, `static_assert ≤ CONFIG_IPC_MAX_MSG_SIZE` (64). Name/default kept. |

Plus the mandatory hard-RT review fix: the oversized-message rule now rides
the new **`MessageQueue::pop_clamped`** primitive (§3.9) — closing the gap
between the spec and the (destructive-only) `MessageQueue` API.