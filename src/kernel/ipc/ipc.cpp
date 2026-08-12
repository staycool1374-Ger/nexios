/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file ipc.cpp
/// @brief Priority-based IPC implementation — message queue, send/recv, sync
/// send, priority inheritance.

#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/debug/ipc_sched_trace.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <assert.hpp>

namespace kernel {

// ---------------------------------------------------------------------------
// MessageQueue
// ---------------------------------------------------------------------------

/// @brief Destructor — wake all blocked senders before queue memory is freed.
MessageQueue::~MessageQueue() {
    // Wake any blocked senders before the queue memory is freed,
    // so they can fast-fail instead of blocking on a zombie destination.
    auto *task = blocked_senders_head;
    while (task) {
        auto *next = task->blocked_next;
        task->blocked_next = nullptr;
        task->blocked_on_queue = nullptr;
        if (task->state != TaskState::TERMINATED)
            Scheduler::set_task_ready(*task);
        task = next;
    }
    blocked_senders_head = nullptr;
    blocked_senders_tail = nullptr;
}

/// @brief Initialise or reset the message queue to empty.
void MessageQueue::init() {
    head = 0;
    tail = 0;
    count = 0;
    prio_bitmap = 0;
    blocked_senders_head = nullptr;
    blocked_senders_tail = nullptr;
    lock_.unlock(); // Ensure SpinLock is unlocked (constructor not called on
                    // MemPool alloc)
}

/// @brief Insert a message at the tail of the queue.
/// @return true on success, false if the queue is full.
bool MessageQueue::push(const Message &msg) {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (is_full())
        return false;

    msgs[tail] = msg;
    prio_bitmap |= (1ULL << msg.priority);
    tail = (tail + 1) % IPC_MAX_QUEUE_MSG;
    count = count + 1;
    return true;
}

/// @brief Remove the highest-priority message (FIFO within same priority).
/// @return true if a message was dequeued.
bool MessageQueue::pop(Message &out) {
    SpinLockGuard<sync::SpinLock> guard(lock_);
    if (is_empty())
        return false;

    // Find message with highest priority (lowest priority number).
    // For same-priority messages, scan from head to preserve FIFO order.
    size_t best_prio = IPC_PRIORITY_LEVELS + 1;
    size_t best_idx = IPC_MAX_QUEUE_MSG;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (head + i) % IPC_MAX_QUEUE_MSG;
        if (msgs[idx].priority < best_prio) {
            best_prio = msgs[idx].priority;
            best_idx = idx;
            if (best_prio == 0)
                break;
        }
    }
    if (best_idx >= IPC_MAX_QUEUE_MSG)
        return false;

    out = msgs[best_idx];

    // Compact: remove the gap by either advancing head (if best_idx == head)
    // or shifting trailing entries left by one.
    if (best_idx == head) {
        head = (head + 1) % IPC_MAX_QUEUE_MSG;
    } else {
        size_t pos = best_idx;
        size_t iter = 0;
        for (iter = 0; iter < IPC_MAX_QUEUE_MSG; ++iter) {
            size_t next = (pos + 1) % IPC_MAX_QUEUE_MSG;
            if (next == tail)
                break;
            msgs[pos] = msgs[next];
            pos = next;
        }
        ENSURE(iter < IPC_MAX_QUEUE_MSG && "queue compaction: tail not found");
        (void)iter;
        if (tail == 0) {
            tail = IPC_MAX_QUEUE_MSG - 1;
        } else {
            tail = tail - 1;
        }
    }
    count = count - 1;

    // Rebuild priority bitmap
    prio_bitmap = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (head + i) % IPC_MAX_QUEUE_MSG;
        prio_bitmap |= (1ULL << msgs[idx].priority);
    }

    return true;
}

/// @brief Return the highest priority that has at least one message.
/// @return IPC_PRIORITY_LEVELS if empty.
size_t MessageQueue::highest_priority() const {
    if (prio_bitmap == 0)
        return IPC_PRIORITY_LEVELS;
    return static_cast<size_t>(__builtin_ctzll(prio_bitmap));
}

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------

/// @brief Initialise the IPC subsystem (per-task queues are set up in task
/// creation).
void IPC::init() {
    // Message queues are initialised per-task in create/create_user/clone.
}

// Forward declaration for rollback helper used by send().
static void unblock_sender_rollback(MessageQueue &q,
                                    TaskControlBlock &task) noexcept;

/// @brief Send a message to a destination task (blocks if queue is full, unless
/// IPC_NONBLOCK).
bool IPC::send(uint64_t dest_id, const Message &msg, uint64_t flags) {
    auto *tcb = Scheduler::find_task(dest_id);
    if (!tcb || tcb->state == TaskState::TERMINATED)
        return false;

    // If the queue is already full, either return immediately (NONBLOCK)
    // or block the sender until space becomes available.
    if (tcb->msg_queue.is_full_locked()) {
        if (flags & IPC_NONBLOCK)
            return false;

        auto *cur = Scheduler::current_task();
        if (!cur)
            return false;

        // Self-send to a full queue can never be drained — return immediately.
        if (dest_id == cur->id)
            return false;

        block_sender(tcb->msg_queue, *cur);
        Scheduler::reschedule();

        // reschedule() is deferred (INV-4) — the current task continues
        // running with state=BLOCKED.  Spin-wait until the timer ISR
        // dispatches the receiver, which drains and wakes us.
        // If interrupts are off (e.g. under IrqGuard) the ISR can't fire,
        // so the receiver can never run — roll back and fail.
        if (arch::interrupts_enabled()) {
            while (cur->state == TaskState::BLOCKED) {
                arch::pause();
            }
        } else {
            unblock_sender_rollback(tcb->msg_queue, *cur);
            return false;
        }

        // Woken up — destination may have been cleaned up while we were
        // blocked.  Re-lookup to avoid accessing a dangling reference.
        tcb = Scheduler::find_task(dest_id);
        if (!tcb || tcb->state == TaskState::TERMINATED)
            return false;

        // Queue should have space now
        if (tcb->msg_queue.is_full())
            return false;
    }

    Message m = msg;
    bool transferred = false;
    if (msg.buf_handle != 0) {
        auto *cur = Scheduler::current_task();
        if (cur) {
            transferred = BufferPool::transfer(msg.buf_handle, *cur, *tcb);
            if (!transferred)
                m.buf_handle = 0;   // drop buffer association; keep message
        }
    }

    bool ok = tcb->msg_queue.push(m);
    if (!ok) {
        if (transferred) {
            auto *cur = Scheduler::current_task();
            if (cur)
                BufferPool::transfer(msg.buf_handle, *tcb, *cur);  // rollback
        }
        return false;
    }

    IPC_SCHED_TRACE("[SEND]", "to=", dest_id, "from=",
                    (Scheduler::current_task() ? Scheduler::current_task()->id : 0),
                    "ty=", m.type, "q=", tcb->msg_queue.count);

    // Wake a task blocked in send_sync() waiting for a reply on its own queue.
    // send_sync sets reply_wait and blocks; without this, the reply would sit
    // in the queue and the sender would hang forever (only rescued by a
    // spurious re-dispatch).  This is the reply-delivery wakeup path.
    if (tcb->reply_wait) {
        tcb->reply_wait = false;
        if (tcb->state == TaskState::BLOCKED) {
            IPC_SCHED_TRACE("[WAKE]", "dest=", dest_id, "st=",
                            static_cast<uint64_t>(tcb->state), "inrq=",
                            tcb->in_ready_queue_ ? 1u : 0u, "q=",
                            tcb->msg_queue.count);
            Scheduler::set_task_ready(*tcb);
            tcb->remaining_ticks = tcb->period_ticks;
        }
    }

    if (tcb->state == TaskState::BLOCKED) {
        Scheduler::set_task_ready(*tcb);
        tcb->remaining_ticks = tcb->period_ticks;
    }
    return ok;
}

/// @brief Receive a message from the calling task's own queue.
bool IPC::recv(Message &msg) {
    auto *cur = Scheduler::current_task();
    if (!cur)
        return false;

    bool ok = cur->msg_queue.pop(msg);
    if (ok && cur->msg_queue.blocked_senders_head) {
        wake_sender(cur->msg_queue, *cur);
    }
    return ok;
}

/// @brief Send and block until a reply arrives (client-side synchronous IPC).
bool IPC::send_sync(uint64_t dest_id, const Message &msg, Message &reply) {
    // Send the request message
    if (!send(dest_id, msg))
        return false;

    // Block waiting for a reply on our own queue
    auto *cur = Scheduler::current_task();
    if (!cur)
        return false;

    bool was_blocked = false;
    IPC_SCHED_TRACE("[SYNC]", "cur=", cur->id, "dest=", dest_id, "ty=",
                    msg.type, "q=", cur->msg_queue.count);
    while (cur->msg_queue.is_empty()) {
        // If destination died while we were waiting for a reply, bail out.
        // BUT: a reply may already be queued (the peer delivered its reply and
        // then terminated normally).  In that case the IPC contract is
        // satisfied — a reply is waiting in our own queue — so we must NOT
        // discard it; break out and let the pop() below consume it.  Only a
        // genuinely empty queue means the peer died before replying.
        auto *dest = Scheduler::find_task(dest_id);
        if (!dest || dest->state == TaskState::TERMINATED) {
            if (cur->msg_queue.is_empty()) {
                IPC_SCHED_TRACE("[SYNC-FAIL]", "dest-gone-empty cur=", cur->id,
                                "dest=", dest_id, "q=", cur->msg_queue.count,
                                "x=", 0u);
                return false;
            }
            IPC_SCHED_TRACE("[SYNC-FAIL]", "dest-gone-reply cur=", cur->id,
                            "dest=", dest_id, "q=", cur->msg_queue.count,
                            "x=", 0u);
            break;
        }

        cur->reply_wait = true;
        cur->state = TaskState::BLOCKED;
        Scheduler::dequeue_ready(*cur);
        was_blocked = true;
        Scheduler::reschedule();
        // v0.4.0 MP-1: sti/hlt/cli is the USER-task blocked-wait pattern.
        if (cur->is_user_) {
            arch::sti();
            arch::hlt();
            arch::cli();
        } else {
            arch::hlt();
        }
    }
    cur->reply_wait = false;
    if (was_blocked) {
        cur->remaining_ticks = cur->period_ticks;
    }

    return cur->msg_queue.pop(reply);
}

/// @brief Return a reference to a task's message queue (asserts existence).
MessageQueue &IPC::queue(uint64_t task_id) {
    auto *tcb = Scheduler::find_task(task_id);
    ENSURE(tcb != nullptr );
    return tcb->msg_queue;
}

/// @brief Block the current task on a full queue (priority inheritance if
/// sender is more urgent).
///
/// # Scheduling contract (WEDGE-invariant)
///
/// After this call the task's state is `BLOCKED`.  The scheduler's
/// [WEDGE] detector (scheduler.cpp:838) enforces that a BLOCKED task
/// must NEVER have `in_ready_queue_ == true`.  Violating this invariant
/// triggers the [WEDGE] diagnostic and, if an orphan (non-linkable)
/// READY/RUNNING task is also present, a hard halt.
///
/// Every call MUST dequeue the task from the ready queue BEFORE setting
/// state = BLOCKED:
///
///   task.state = TaskState::BLOCKED;
///   kernel::Scheduler::dequeue_ready(task);   // <-- mandatory
///
/// **Precondition:** `task` must be a valid, live TaskControlBlock
/// (magic == TCB_MAGIC).  The caller is typically the current task
/// (established via `Scheduler::set_current()` or RMS dispatch).
///
/// **Postcondition:**
///   - `task.state == TaskState::BLOCKED`
///   - `task.in_ready_queue_ == false`  — ready-queue membership revoked
///   - `task.blocked_on_queue == &q`    — linked into q's sender list
///   - `task.priority` may be lowered by the priority-inheritance boost
///     (owner's priority raised if sender is more urgent).
///
/// **Caller discipline:**
///   - The scheduler lock (`scheduler_lock_`) must NOT be held when
///     entering this function.  Note: `ReadyQueueManager::remove` is
///     lock-free (ready_queue_manager.cpp:75-91) — the section is protected
///     by IRQ exclusion (`arch::IrqGuard`, see docs/irqguard-ledger.md S1),
///     not by a lock; dequeue_ready does not acquire scheduler_lock_.
///     If the caller holds the lock it must release it first, otherwise any
///     later non-lock-free scheduler call will spin-wait on the spinlock
///     until released, which can deadlock if the holder is the same CPU
///     (non-recursive spinlock).
///   - Interrupts: `dequeue_ready` does not disable interrupts.  Callers
///     should ensure IRQ safety is managed externally (e.g., via
///     `arch::IrqGuard` in `IPC::send`).
///   - After return, the caller MUST invoke `Scheduler::reschedule()` to
///     request a deferred context switch (INV-4).  Because of the deferred
///     model, the caller continues executing with state=BLOCKED until the
///     next timer tick applies the switch.  The spin-wait loop in
///     `IPC::send()` handles this window.  Calling `reschedule()` without
///     having dequeued the task first would leave it BLOCKED+inrq and
///     trigger the [WEDGE] invariant violation on the very next tick.
///   - When IPC::send's spin-wait is active, the task's state is temporarily
///     BLOCKED on-CPU — this is the one legitimate case of a BLOCKED task
///     having CPU ownership (INV-4 deferred switch).  The ready-queue
///     invariant is what matters; CPU ownership is separate.
///
/// # WEDGE-avoidance summary
///
/// Every transition to BLOCKED must be atomic with dequeue_ready:
///   | code                                | invariant |
///   |-------------------------------------|-----------|
///   | state = BLOCKED; + dequeue_ready(); | OK        |
///   | state = BLOCKED; (no dequeue)        | WEDGE     |
///
/// This function implements the correct pattern.  Callers that set
/// `state = BLOCKED` independently (e.g., `monitor_task_entry`,
/// `Scheduler::terminate`) must also dequeue first.
bool IPC::block_sender(MessageQueue &q, TaskControlBlock &task) {
    task.state = TaskState::BLOCKED;
    kernel::Scheduler::dequeue_ready(task);

    // Dequeue_ready must complete BEFORE taking q.lock_ (lock ordering:
    // scheduler_lock_ first, then queue lock).  The queue lock protects
    // the blocked_senders list and priority-inheritance boost.
    {
        SpinLockGuard<sync::SpinLock> guard(q.lock_);

        task.blocked_on_queue = &q;
        TaskControlBlock **pp = &q.blocked_senders_head;
        while (*pp && (*pp)->priority >= task.priority)
            pp = &(*pp)->blocked_next;
        task.blocked_next = *pp;
        *pp = &task;
        if (!task.blocked_next)
            q.blocked_senders_tail = &task;

        // Priority inheritance — boost owner when higher-priority sender blocks.
        // FIX(sched-race): the read-modify-write of q.owner->priority and the
        // move_priority() re-index must be atomic against the timer ISR's
        // deadline demote / sporadic priority changes.  q.lock_ is a plain
        // spinlock (does not mask IRQs); IrqGuard excludes the ISR so
        // old/new effective_priority snapshots stay consistent.
        if (q.owner && task.priority > q.owner->priority) {
            arch::IrqGuard irq_guard{};
            // G2: q.owner is magic-guarded live here — bind a reference
            // (docs/irqguard-ledger.md §G2-A).  move_priority MUST stay
            // between the old/new effective_priority snapshots.
            TaskControlBlock &owner = *q.owner;
            uint64_t old_prio = Scheduler::effective_priority(&owner);
            owner.priority = task.priority;
            uint64_t new_prio = Scheduler::effective_priority(&owner);
            // Only re-bucket when the owner is a ready-queue member.  A
            // BLOCKED owner has no bucket; enqueueing it here would insert a
            // BLOCKED task into the runq ([WEDGE] INV-2).  The field write
            // above is always correct — set_task_ready() later enqueues it at
            // the boosted priority.  Mirrors Scheduler::set_priority.
            if (old_prio != new_prio && owner.in_ready_queue_)
                Scheduler::move_priority(owner, old_prio, new_prio);
        }
    }

    return true;
}

/// @brief Unblock the oldest blocked sender and restore receiver priority.
void IPC::wake_sender(MessageQueue &q, TaskControlBlock &receiver) {
    TaskControlBlock *task;

    // Pop sender from head under the queue lock.  set_task_ready and priority
    // restore run outside the lock (they take scheduler_lock_ internally).
    {
        SpinLockGuard<sync::SpinLock> guard(q.lock_);

        if (!q.blocked_senders_head)
            return;

        task = q.blocked_senders_head;
        q.blocked_senders_head = task->blocked_next;
        if (!q.blocked_senders_head)
            q.blocked_senders_tail = nullptr;
        task->blocked_next = nullptr;
        task->blocked_on_queue = nullptr;
    }

    if (task->state != TaskState::TERMINATED) {
        Scheduler::set_task_ready(*task);
        task->remaining_ticks = task->period_ticks;
    }

    // Priority inheritance — restore receiver priority after sender removal.
    // FIX(sched-race): same IRQ-safety rationale as block_sender's boost.
    {
        SpinLockGuard<sync::SpinLock> guard(q.lock_);
        uint64_t max_prio = receiver.base_priority;
        auto *cur_bs = q.blocked_senders_head;
        while (cur_bs) {
            if (cur_bs->priority > max_prio)
                max_prio = cur_bs->priority;
            cur_bs = cur_bs->blocked_next;
        }
        arch::IrqGuard irq_guard{};
        uint64_t old_prio = Scheduler::effective_priority(&receiver);
        receiver.priority = max_prio;
        uint64_t new_prio = Scheduler::effective_priority(&receiver);
        // Re-bucket only runq members; a BLOCKED/RUNNING receiver has no
        // bucket to move and is re-enqueued later via effective_priority.
        if (old_prio != new_prio && receiver.in_ready_queue_)
            Scheduler::move_priority(receiver, old_prio, new_prio);
    }
}

/// @brief Roll back a block_sender() mutation when IPC::send() is called with
///        interrupts disabled and the deferred switch can never apply.
///        Removes @p task from @p q's blocked-senders list, restores its state
///        to RUNNING, and re-enqueues it on the ready queue.
/// @pre task.blocked_on_queue == &q
/// @param q   The message queue whose blocked-senders list contains @p task.
/// @param task The task to unblock.
static void unblock_sender_rollback(MessageQueue &q,
                                    TaskControlBlock &task) noexcept {
    ENSURE(task.blocked_on_queue == &q &&
           "unblock_sender_rollback: task not blocked on this queue");

    // Remove from singly-linked blocked_senders list under the queue lock.
    {
        SpinLockGuard<sync::SpinLock> guard(q.lock_);

        TaskControlBlock **pp = &q.blocked_senders_head;
        while (*pp && *pp != &task)
            pp = &(*pp)->blocked_next;
        if (*pp) {
            *pp = task.blocked_next;
            if (q.blocked_senders_tail == &task) {
                TaskControlBlock *walk = q.blocked_senders_head;
                while (walk && walk->blocked_next)
                    walk = walk->blocked_next;
                q.blocked_senders_tail = walk;
            }
        }

        task.blocked_next = nullptr;
        task.blocked_on_queue = nullptr;
    }

    // Restore pre-block state and re-add to ready queue.
    // Do NOT use set_task_ready() — it sets state=READY and alters
    // remaining_ticks.  The task never actually left the CPU, so restore
    // RUNNING directly and only fix ready-queue membership.
    task.state = TaskState::RUNNING;
    Scheduler::enqueue_ready(task);
}

} // namespace kernel
