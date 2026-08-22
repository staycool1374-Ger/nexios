# Jarvis RTOS Coding Style


## 1. Language & Build

- **Standard**: C++20 freestanding (`-ffreestanding -fno-exceptions -fno-rtti -nostdlib`)
- **Target**: `x86_64-elf`; no STL, exceptions, RTTI, stack-protector, thread-safe statics, dynamic linking on RT paths
- **Optimization**: `-O3` (debug), `-O2` (release)

## 2. Naming

| Category | Convention | Example |
|---|---|---|
| Classes/Structs/Enums | `PascalCase` | `TaskControlBlock` |
| Member/Free functions | `snake_case` | `alloc_page()` |
| Variables/Parameters | `snake_case` | `page_table_` |
| Constants (`constexpr`) | `SCREAMING_SNAKE` | `MAX_BUFFERS` |
| Macros | `SCREAMING_SNAKE` | `ENSURE()` |
| Namespaces | `lowercase` | `kernel`, `arch` |
| Template params | `PascalCase` | `typename T` |
| UDL | `_KiB`, `_MiB` | `16_KiB` |

## 3. File Structure

```cpp
/// @file <path>/<file>.hpp /// @brief <one-line>
#pragma once
#include <types.hpp>             // lib first
#include <constants.hpp>
#include <kernel/<module>/...>   // kernel headers
#include <lib/...>               // then lib
```

- **Guard**: `#pragma once` only
- **Include order**: `<types.hpp>` → `<kernel/...>` → `<lib/...>` → local
- Prefer forward declarations over includes
- No `using namespace` in headers (OK in `.cpp`)

## 4. Memory & Ownership

- No `new`/`delete`/`malloc`/`free` on RT paths — use pools/fixed arrays
- PMM: `alloc_user_page()` (user) vs `alloc_page()` (kernel)
- VMM: `map_page_in_pml4()` for non-default AS
- User memory: must use `CheckedPtr<T>` or `safe_copy_from_user()`; no raw ptr arithmetic

## 5. Error Handling

### 5.1 ENSURE (Fatal — Always On)
```cpp
ENSURE(ptr != nullptr);  // panics with file:line
```
For invariants that must never fail.

**ENSURE vs. error code — the deciding rule:** ENSURE is only for conditions
that are *impossible by design* (corrupted state, broken internal invariants).
Any condition that can be reached by legitimate operation — resource
exhaustion (`MAX_WAITERS` full, pool empty, ring full), bad user input,
timeout — must return an error code or `ErrorOr<T>`, never panic.
```cpp
// Good: reachable exhaustion → error
if (waiter_count_ >= MAX_WAITERS) return SYNC_ERR_MAX_WAITERS;
// Bad: kernel panic on a reachable condition
ENSURE(waiter_count_ < MAX_WAITERS);
```

### 5.2 ASSERT (Debug Only)
```cpp
ASSERT(pmm::Error::OOM);  // logs via Logger::error()
```
For expected failures (OOM, I/O errors). No-ops in release (`-UCONFIG_DEBUG`).

### 5.3 Module Error Headers
Each module defines error codes in `*_errors.hpp` via X-macro pattern with `error_string<E>()`.

**Error codes are mandatory, not optional:** if a module defines `*_errors.hpp`
codes, every failure path in that module returns a specific code — never a
generic `(uint64_t)-1`. Callers must be able to distinguish failure causes.
A defined-but-unused error enum is dead code and an audit finding.

### 5.4 ErrorOr<T>
```cpp
ErrorOr<uint64_t> r = alloc_page();
if (!r.ok()) return;
uint64_t page = *r;
```

## 6. Safety & Compliance

- MISRA C++:2023, AUTOSAR, ISO 26262 ASIL D / IEC 61508
- **Fully bounded loops**: deterministic max iteration; no `while(true)`/`for(;;)` without bound. Exception: boot-time hardware calibration (pre-scheduler, bounded by a named constant) — **not** runtime blocking waits
- No `volatile` for sync — use atomics or mutexes
- No primitive `reinterpret_cast` — use aligned punning
- No `dynamic_cast`, no `typeid`
- **Debug/Release must not diverge in behavior**: `#ifdef CONFIG_DEBUG` may add logging, diagnostics, and extra invariant checks, but must never change control flow, failure policy, or state transitions between builds. A detect-and-halt policy exists in release too (it may log less). Rationale: test campaigns that only run debug builds would never exercise the failure behavior production ships with.

## 7. Testing

- Test-first: write stub first, then implement
- Format:
  ```
  // Runmode: kernel
  // Testidea: <what>
  // Input: <setup>
  // Expect: <outcome>
  // Depends: <deps>
  JARVIS_TEST(name) { ... }
  ```
- Stubs: `JARVIS_TEST_PASS()` only
- Registration: `JARVIS_REGISTER_TEST(name)` (debug), `JARVIS_REGISTER_RELEASE_TEST(name)` (release)
- Tests on `testbed` branch, merged to `main`

### 7.1 Test Resource Management

- **Single-owner resources** use `UniquePtr<T, Deleter>` (e.g., `TaskPtr`, `SimpleTaskPtr` from `<kernel/test/task_ptr.hpp>`)
- **Multi-resource cleanup** uses `ScopeGuard` with a lambda
- **Never** `dismiss()` a `ScopeGuard` and manually repeat the same cleanup — let the destructor fire on all exit paths
- Custom deleters live in `src/kernel/test/task_ptr.hpp`

## 8. Formatting

- **Indent**: 4 spaces, no tabs
- **Line length**: 80 chars (90 for comments, data tables excluded)
- **Braces**: same line
- **Spaces**: around binary ops, after `if`/`while`/`for`; none after `(` or before `)`
- Trailing commas in multi-line init lists

## 9. Documentation

Doxygen for public APIs:
```cpp
/// @brief <one-line>
/// @param <name> <desc>
/// @return <desc>
```
File header: `/// @file`, `/// @brief`. Inline comments for complex logic only.

## 10. Kernel-Specific Rules (`src/kernel/` only)

### 10.1 Const Correctness
Everything not modified must be `const` — variables, parameters, member functions.

### 10.2 Prefer References Over Pointers
`T&` for non-nullable params; `T*` only for nullable/optional or output params.
```cpp
// Good
void enqueue_writer(TaskControlBlock& task);
bool find_by_id(uint64_t id, TaskControlBlock* result);
// Bad
void enqueue_writer(TaskControlBlock* task);  // never null — use ref
```

### 10.3 All Variables Must Be Initialized
```cpp
size_t count = 0;                          // Good
Message msg{};                             // Good
size_t count;                              // Bad
```

### 10.4 Constructor Initializer Lists
Member initializer list required — no body assignment.
```cpp
Task::Task(uint64_t id) : id_(id), state_(READY) {}    // Good
Task::Task(uint64_t id) { id_ = id; state_ = READY; }  // Bad
```

### 10.5 Meaningful Sentinel Definitions
Named constants (enums/constexpr) only; no raw magic numbers. **Each sentinel value must be unique across the project** (-1, -2, -3...).
```cpp
enum class BufferSentinel : int64_t { INVALID_HANDLE = -2 };
enum class VfsSentinel : int64_t    { INVALID_FD = -4 };
// Bad: duplicate -1 across enums
```

### 10.6 Descriptive Names — No Single Characters
- Minimum 3 chars; use `<thing>_<instance>` pattern: `msr_low`, `page_idx`, `virt_addr`
- **Blocklist**: `tmp`, `temp`, `ptr`, `p`, `t`, `v`, `val`
- **Allowlist** (abbreviations): `id`, `fd`, `va`, `cs`, `tv`, `vn`, `st`, `en`, `it`, `ok`, `to`, `q`, `n`, `m`, `y`, `c`, `s`, `r`, `lo`, `hi`, `tm`, `h`, `L`, `T`, `O`, `C`
- Loop indices `i`/`j`/`k`/`idx` OK for tight loops (body ≤ 5 lines); not for params or members

### 10.7 const_cast Is Forbidden
Use `mutable` or redesign instead.

## 11. Concurrency (`src/kernel/` only)

These rules exist because every one of them was violated at least once
(refer to `audits/audit-task-sync-v0.4.2.md` for the incident record).

### 11.1 Never Hold a Spinlock Across a Context Switch
`Scheduler::reschedule()` arms a *deferred* switch — the task keeps running
until the timer ISR applies it. Holding any spinlock across the reschedule
call deadlocks the ISR that takes it (timer IRQ → scheduler → spin forever).

**Pattern (template: `Notify::wait()`):**
```cpp
{
    SpinLockGuard<SpinLock> g(lock_);   // 1. inspect state, insert waiter
    if (try_acquire()) return OK;
    add_waiter(*task);
}                                        // 2. release BEFORE blocking
task->state = TaskState::BLOCKED;        // 3. transition under scheduler_lock_
Scheduler::dequeue_ready(*task);         // 4. MUST dequeue (see §11.2)
Scheduler::reschedule();
```

### 11.2 Every BLOCKED Transition Must Leave the Ready Queue
Setting `state = BLOCKED` without `Scheduler::dequeue_ready()` leaves a
blocked task physically queued (INV-2 desync). Debug builds halt on this;
release builds live-lock. Every blocking path must dequeue before
`reschedule()`, and should include the interrupts-disabled rollback branch
used by `Queue::send/receive`.

### 11.3 Blocking Waits Must Be Bounded or Scheduler-Mediated
A task that blocks on a primitive either (a) gets woken by a documented
waker, with a timeout fallback returning `*_ERR_TIMEOUT` /
`SYNC_ERR_INTERRUPTED`, or (b) is descheduled (BLOCKED + dequeued). Spinning
with `pause()` at raised priority on a condition another task must set is
forbidden — a lost wakeup then spins forever.

### 11.4 Priority Mutations Go Through the Scheduler Helper
Never write `tcb->priority = X` directly. Use the scheduler helper that
re-buckets a task sitting in the ready queue (`move_priority`). Direct
writes make PI boosts ineffective and desynchronize queue order from
priority.

### 11.5 PI Boosts Must Be Revertible and Symmetric
Every priority-inheritance boost stores the saved base priority in a holder
field, and every wake/unlock path restores it **and clears the field**
(uniform `>` semantics — see mutex `restore_priority()` as reference).
An uncleared holder field permanently inflates the task's base priority
(leaks across lock cycles).

### 11.6 Atomics Are All-or-Nothing Per Index
An object accessed concurrently must be accessed atomically from *every*
context — mixing plain reads with `atomic_store` on the same index is UB
(data race), even when "one side is the only writer". SPSC rings: declare
indices atomic everywhere or document single-threaded ownership explicitly.

## 12. Shared Resource & TCB Lifecycles

### 12.1 Dead-State Filter: REAPED and TERMINATED
Any code touching another task's TCB (waking, boosting, signalling) must
reject both `TERMINATED` **and** `REAPED`. A reaped-and-recycled TCB is a
use-after-free waiting to happen. Checking only `!= TERMINATED` is a bug:
cleanup marks tasks REAPED, not TERMINATED.

### 12.2 Generation-Tag Stored Pointers
Raw TCB pointers stored across operations (`waiter_`, `last_sender_`,
`owner_`) must carry a generation tag validated at use time. Pointer alone
cannot distinguish the original task from its recycled successor.
Reference: generation-tagged waiter arrays in `mutex.cpp` / `queue.cpp`.

### 12.3 Wakers Own the Wakeup Contract
A primitive that blocks tasks must wake them on *every* teardown/destruction
path of that primitive (destructor, dispose, last-release). If blocked
tasks can outlive the primitive, the destroy path drains waiters first —
documented invariant without enforcement does not count.

### 12.4 Single Ownership Discipline per Object Type
Each dynamically-managed type has exactly one alloc/release pair (e.g.
TCBs: `MemPool::alloc` ↔ `TaskControlBlock::cleanup()` + `MemPool::free`).
Mixing `delete` with pool free on the same objects is heap mismatch →
corruption/double-free on exactly the error paths the guards exist for.

### 12.5 Ownership Checks Are Not Optional on State-Changing Ops
For any handle-based resource, *every* operation that reads, maps, transfers,
or frees must validate ownership/generation — not just some. A `map()`
without an owner check next to `free()`/`transfer()` with checks is an
audit blocker (privilege escalation by handle guessing).
