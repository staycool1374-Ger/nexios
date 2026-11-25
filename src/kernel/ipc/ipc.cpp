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
#include <kernel/time/timer_wheel.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/sched_trigger.hpp>
#include <kernel/cap/endpoint.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/debug/ipc_sched_trace.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/park_lock.hpp>
#include <assert.hpp>

namespace kernel {

namespace {
// M2 follow-up evidence (#545 SILENT, CONFIG_DEBUG only): lost-wakeup
// window probes.  Stores only; no behaviour change.
#if defined(CONFIG_DEBUG)
uint64_t g_ipc_wake_gate_missed = 0;
uint64_t g_ipc_block_with_pending_inbox = 0;
// F1 (#545, INV-4b): in-guard try-once probe outcomes (diagnostic only).
uint64_t g_ipc_probe_hit = 0;
uint64_t g_ipc_probe_contended = 0;
#endif
uint64_t g_ipc_reply_dropped = 0;
} // namespace

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
        // H-3: a cleaned-up task is REAPED, not TERMINATED.  Never feed a
        // freed TCB to set_task_ready (ready-queue corruption / UAF).
        if (task->state != TaskState::TERMINATED &&
            task->state != TaskState::REAPED)
            Scheduler::set_task_ready(*task);
        task = next;
    }
    blocked_senders_head = nullptr;
    blocked_senders_tail = nullptr;

    // M-4 (audit-ipc-cap-syscalls-v0.4.2): undo the priority-inheritance
    // boost applied to the owner by block_sender() — with no blocked senders
    // left the owner must return to its base priority.  Without this the
    // owner kept its elevated priority permanently (priority-inversion
    // residue across repeated block/wake cycles).
    if (owner) {
        TaskControlBlock &owner_task = *owner;
        arch::IrqGuard irq_guard{};
        uint64_t old_prio = Scheduler::effective_priority(&owner_task);
        owner_task.priority = owner_task.base_priority;
        uint64_t new_prio = Scheduler::effective_priority(&owner_task);
        if (old_prio != new_prio && owner_task.in_ready_queue_)
            Scheduler::move_priority(owner_task, old_prio, new_prio);
    }
}

/// @brief Initialise or reset the message queue to empty.
void MessageQueue::init() {
    head = 0;
    tail = 0;
    count = 0;
    prio_bitmap = 0;
    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i)
        reply_tok[i] = 0;
    blocked_senders_head = nullptr;
    blocked_senders_tail = nullptr;
    lock_.set_rank(sync::LockRank::Queue); // M2 §6.2: queue rank.
    lock_.unlock(); // Ensure SpinLock is unlocked (constructor not called on
                    // MemPool alloc).  Unpaired by design: note_release()
                    // treats an init-time unlock as a silent no-op.
}

/// @brief Locked fullness probe (M2: ParkLockGuard so a contended probe
///        parks instead of spinning behind a preempted holder).
bool MessageQueue::is_full_locked() {
    sync::ParkLockGuard guard(lock_, "mq:is_full_locked");
    return count >= IPC_MAX_QUEUE_MSG;
}

/// @brief Insert a message at the tail of the queue.
/// @return true on success, false if the queue is full.
bool MessageQueue::push(const Message &msg) {
    sync::ParkLockGuard guard(lock_, "mq:push");
    return push_locked(msg);
}

/// @brief push() body with the lock already held (see header contract).
bool MessageQueue::push_locked(const Message &msg, uint32_t tok) {
    if (is_full())
        return false;

    msgs[tail] = msg;
    reply_tok[tail] = tok;
    prio_bitmap |= (1ULL << msg.priority);
    tail = (tail + 1) % IPC_MAX_QUEUE_MSG;
    count = count + 1;
    return true;
}

/// @brief Index of the highest-priority message (FIFO within a priority).
/// @return IPC_MAX_QUEUE_MSG when empty.  Caller MUST hold lock_.
size_t MessageQueue::find_best_index() const {
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
    return best_idx;
}

/// @brief Remove the message at @p best_idx (compaction + prio_bitmap
///        rebuild).  Caller MUST hold lock_.
void MessageQueue::remove_at(size_t best_idx) {
    if (best_idx == head) {
        reply_tok[head] = 0;
        head = (head + 1) % IPC_MAX_QUEUE_MSG;
    } else {
        size_t pos = best_idx;
        size_t iter = 0;
        for (iter = 0; iter < IPC_MAX_QUEUE_MSG; ++iter) {
            size_t next = (pos + 1) % IPC_MAX_QUEUE_MSG;
            if (next == tail)
                break;
            msgs[pos] = msgs[next];
            reply_tok[pos] = reply_tok[next];
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
}

/// @brief Remove the highest-priority message (FIFO within same priority).
/// @return true if a message was dequeued.
bool MessageQueue::pop(Message &out) {
    sync::ParkLockGuard guard(lock_, "mq:pop");
    if (is_empty())
        return false;

    size_t best_idx = find_best_index();
    if (best_idx >= IPC_MAX_QUEUE_MSG)
        return false;

    out = msgs[best_idx];
    remove_at(best_idx);
    return true;
}

/// @brief Remove the highest-priority message only if it fits @p max_size.
/// @return true if a fitting message was dequeued; false if the queue is
///         empty OR the best match is oversized (NOT removed — fail-closed,
///         issue #11 paper §3.9).  Callers disambiguate with is_empty().
bool MessageQueue::pop_clamped(Message &msg, uint32_t max_size) {
    sync::ParkLockGuard guard(lock_, "mq:pop_clamped");
    if (is_empty())
        return false;

    size_t best_idx = find_best_index();
    if (best_idx >= IPC_MAX_QUEUE_MSG)
        return false;
    if (msgs[best_idx].data_size > max_size)
        return false;

    msg = msgs[best_idx];
    remove_at(best_idx);
    return true;
}

/// @brief Index of the highest-priority message from @p sender_id (issue
///        #296).  Priority-min with first-index-wins ties — the same order
///        as find_best_index, so selective removal preserves INV-P.
/// @return IPC_MAX_QUEUE_MSG when no message from @p sender_id is queued.
///         Caller MUST hold lock_.
size_t MessageQueue::find_best_from_sender(uint64_t sender_id) const {
    size_t best_prio = IPC_PRIORITY_LEVELS + 1;
    size_t best_idx = IPC_MAX_QUEUE_MSG;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (head + i) % IPC_MAX_QUEUE_MSG;
        if (msgs[idx].sender_id != sender_id)
            continue;
        if (msgs[idx].priority < best_prio) {
            best_prio = msgs[idx].priority;
            best_idx = idx;
            if (best_prio == 0)
                break;
        }
    }
    return best_idx;
}

/// @brief Remove the highest-priority message from @p sender_id (issue #296).
///        Non-matching messages are never touched.
/// @return true with @p out filled; false when none is queued.
bool MessageQueue::pop_from_sender(Message &out, uint64_t sender_id) {
    sync::ParkLockGuard guard(lock_, "mq:pop_from_sender");
    size_t best_idx = find_best_from_sender(sender_id);
    if (best_idx >= IPC_MAX_QUEUE_MSG)
        return false;

    out = msgs[best_idx];
    remove_at(best_idx);
    return true;
}

/// @brief pop_from_sender with the issue #11 clamp (issue #296): an
///        oversized best matching message stays queued (returns false);
///        non-matching messages never count as oversized and are never
///        consumed.
bool MessageQueue::pop_from_sender_clamped(Message &out, uint32_t max_size,
                                            uint64_t sender_id) {
    sync::ParkLockGuard guard(lock_, "mq:pop_from_sender_clamped");
    size_t best_idx = find_best_from_sender(sender_id);
    if (best_idx >= IPC_MAX_QUEUE_MSG)
        return false;
    if (msgs[best_idx].data_size > max_size)
        return false;

    out = msgs[best_idx];
    remove_at(best_idx);
    return true;
}

/// @brief M3 one-shot reply selectors (F3/Q2): best match additionally
///        requires the one-shot token (sidecar reply_tok == token).
///        Token-less or wrong-token messages from the same sender are
///        never touched — they stay queued for their real consumer.
size_t MessageQueue::find_best_reply_from(uint64_t sender_id,
                                          uint32_t token) const {
    size_t best_prio = IPC_PRIORITY_LEVELS + 1;
    size_t best_idx = IPC_MAX_QUEUE_MSG;
    for (size_t i = 0; i < count; ++i) {
        const size_t idx = (head + i) % IPC_MAX_QUEUE_MSG;
        if (msgs[idx].sender_id != sender_id)
            continue;
        if (reply_tok[idx] != token)
            continue;
        if (msgs[idx].priority < best_prio) {
            best_prio = msgs[idx].priority;
            best_idx = idx;
            if (best_prio == 0)
                break;
        }
    }
    return best_idx;
}

bool MessageQueue::pop_reply_from(Message &out, uint64_t sender_id,
                                  uint32_t token) {
    sync::ParkLockGuard guard(lock_, "mq:pop_reply_from");
    size_t best_idx = find_best_reply_from(sender_id, token);
    if (best_idx >= IPC_MAX_QUEUE_MSG)
        return false;

    out = msgs[best_idx];
    remove_at(best_idx);
    return true;
}

bool MessageQueue::pop_reply_from_clamped(Message &out, uint32_t max_size,
                                          uint64_t sender_id,
                                          uint32_t token) {
    sync::ParkLockGuard guard(lock_, "mq:pop_reply_from_clamped");
    size_t best_idx = find_best_reply_from(sender_id, token);
    if (best_idx >= IPC_MAX_QUEUE_MSG)
        return false;
    if (msgs[best_idx].data_size > max_size)
        return false;

    out = msgs[best_idx];
    remove_at(best_idx);
    return true;
}

bool MessageQueue::has_reply_from(uint64_t sender_id, uint32_t token) {
    sync::ParkLockGuard guard(lock_, "mq:has_reply_from");
    return find_best_reply_from(sender_id, token) < IPC_MAX_QUEUE_MSG;
}

/// @brief Stamp sweep body (M3): every queued message from @p sender_id
///        gets @p token, so a reply that arrived before the waiter armed
///        its ticket still matches pop_reply_from (legacy-equivalent:
///        the old pop_from_sender consumed the same messages as replies).
///        Non-blocking try: Contended leaves everything untouched.
bool MessageQueue::stamp_from_try(uint64_t sender_id, uint32_t token) {
    if (!lock_.try_lock())
        return false;
    sync::spinlock_annotate_site(&lock_, "mq:stamp_from_try");
    for (size_t i = 0; i < count; ++i) {
        const size_t idx = (head + i) % IPC_MAX_QUEUE_MSG;
        if (msgs[idx].sender_id == sender_id)
            reply_tok[idx] = token;
    }
    lock_.unlock();
    return true;
}

/// @brief Try-once probe bodies (F1 #545, INV-4b contract in task.hpp).
///        Single try_lock; every path unlocks exactly once.  Selection
///        reuses find_best_*/remove_at (single source with the blocking
///        pops — no INV-P/#11 semantic drift).
MessageQueue::QueueProbe MessageQueue::probe_pop(Message &out) {
    if (!lock_.try_lock())
        return QueueProbe::Contended;
    sync::spinlock_annotate_site(&lock_, "mq:probe_pop");
    QueueProbe result = QueueProbe::Empty;
    if (!is_empty()) {
        const size_t best_idx = find_best_index();
        if (best_idx < IPC_MAX_QUEUE_MSG) {
            out = msgs[best_idx];
            remove_at(best_idx);
            result = QueueProbe::Consumed;
        }
    }
    lock_.unlock();
    return result;
}

MessageQueue::QueueProbe MessageQueue::probe_pop_clamped(Message &out,
                                                         uint32_t max_size) {
    if (!lock_.try_lock())
        return QueueProbe::Contended;
    sync::spinlock_annotate_site(&lock_, "mq:probe_pop_clamped");
    QueueProbe result = QueueProbe::Empty;
    if (!is_empty()) {
        const size_t best_idx = find_best_index();
        if (best_idx < IPC_MAX_QUEUE_MSG) {
            if (msgs[best_idx].data_size > max_size) {
                result = QueueProbe::PresentOversized;
            } else {
                out = msgs[best_idx];
                remove_at(best_idx);
                result = QueueProbe::Consumed;
            }
        }
    }
    lock_.unlock();
    return result;
}

MessageQueue::QueueProbe
MessageQueue::probe_pop_from_sender(Message &out, uint64_t sender_id) {
    if (!lock_.try_lock())
        return QueueProbe::Contended;
    sync::spinlock_annotate_site(&lock_, "mq:probe_pop_from_sender");
    QueueProbe result = QueueProbe::Empty;
    const size_t best_idx = find_best_from_sender(sender_id);
    if (best_idx < IPC_MAX_QUEUE_MSG) {
        out = msgs[best_idx];
        remove_at(best_idx);
        result = QueueProbe::Consumed;
    }
    lock_.unlock();
    return result;
}

MessageQueue::QueueProbe MessageQueue::probe_pop_from_sender_clamped(
    Message &out, uint32_t max_size, uint64_t sender_id) {
    if (!lock_.try_lock())
        return QueueProbe::Contended;
    sync::spinlock_annotate_site(&lock_, "mq:probe_pop_from_sender_clamped");
    QueueProbe result = QueueProbe::Empty;
    const size_t best_idx = find_best_from_sender(sender_id);
    if (best_idx < IPC_MAX_QUEUE_MSG) {
        if (msgs[best_idx].data_size > max_size) {
            result = QueueProbe::PresentOversized;
        } else {
            out = msgs[best_idx];
            remove_at(best_idx);
            result = QueueProbe::Consumed;
        }
    }
    lock_.unlock();
    return result;
}

/// @brief M3 one-shot reply probes (F3/Q2): probe_pop_from_sender[clamped]
///        additionally restricted to @p token.  Single try_lock; every path
///        unlocks exactly once.
MessageQueue::QueueProbe MessageQueue::probe_pop_reply_from(
    Message &out, uint64_t sender_id, uint32_t token) {
    if (!lock_.try_lock())
        return QueueProbe::Contended;
    sync::spinlock_annotate_site(&lock_, "mq:probe_pop_reply_from");
    QueueProbe result = QueueProbe::Empty;
    const size_t best_idx = find_best_reply_from(sender_id, token);
    if (best_idx < IPC_MAX_QUEUE_MSG) {
        out = msgs[best_idx];
        remove_at(best_idx);
        result = QueueProbe::Consumed;
    }
    lock_.unlock();
    return result;
}

MessageQueue::QueueProbe MessageQueue::probe_pop_reply_from_clamped(
    Message &out, uint32_t max_size, uint64_t sender_id, uint32_t token) {
    if (!lock_.try_lock())
        return QueueProbe::Contended;
    sync::spinlock_annotate_site(&lock_, "mq:probe_pop_reply_from_clamped");
    QueueProbe result = QueueProbe::Empty;
    const size_t best_idx = find_best_reply_from(sender_id, token);
    if (best_idx < IPC_MAX_QUEUE_MSG) {
        if (msgs[best_idx].data_size > max_size) {
            result = QueueProbe::PresentOversized;
        } else {
            out = msgs[best_idx];
            remove_at(best_idx);
            result = QueueProbe::Consumed;
        }
    }
    lock_.unlock();
    return result;
}


/// @brief Non-destructive probe for a queued message from @p sender_id
///        (issue #296, send_sync dest-death path).
bool MessageQueue::has_from(uint64_t sender_id) {
    sync::ParkLockGuard guard(lock_, "mq:has_from");
    return find_best_from_sender(sender_id) < IPC_MAX_QUEUE_MSG;
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
/// IpcFlags::NONBLOCK).
// ---------------------------------------------------------------------------
// M3 (F3/Q3): cap on direct-switch attempts per wait.  A refused or
// lost trigger falls through to the tick path; the cap bounds trap
// traffic (no unbounded self-IPI loop if the receiver never becomes
// dispatchable).
inline constexpr uint64_t kDirectMaxAttempts = 3;

// M3 one-shot reply tokens (F3/Q2)
// ---------------------------------------------------------------------------

namespace {

// M3 (F3/Q2): token sequence, seeded at 1 (0 = no token).  u32: tokens
// are single-outstanding per waiter (a second Call mints only after the
// first resolves), so 32 bits never alias a live token.  Functional in
// all builds (not CONFIG_DEBUG-gated): release builds enforce tokens too.
uint32_t g_reply_token_seq = 1;

uint32_t mint_reply_token() noexcept {
    uint32_t token =
        __atomic_fetch_add(&g_reply_token_seq, 1, __ATOMIC_RELAXED) &
        0x7FFFFFFFu;
    if (token == 0) {
        // 0 is reserved for "no token"; skip it.
        token = (__atomic_fetch_add(&g_reply_token_seq, 1,
                                    __ATOMIC_RELAXED) &
                 0x7FFFFFFFu);
        if (token == 0)
            token = 1;
    }
    return token;
}

// M3 ticket word: high bit = live, low 31 = token.  u32 keeps the TCB
// inside pool8 (see the static_assert); the single atomic word still
// gives one-shot consume semantics via exchange(0).
inline constexpr uint32_t kReplyTicketLive = 0x80000000u;

/// @brief Outcome of the M3 reply-token rule applied to one delivery.
enum class DeliverRule : uint8_t {
    Pushed,      ///< No live ticket naming this sender: pushed untokened.
    PushedReply, ///< Live ticket names this sender: consumed, stamped, pushed.
    Full,        ///< Queue full under the lock: nothing pushed.
};

/// @brief Apply the M3 reply-token rule and push atomically (M3/F3/Q2).
///        Caller must hold NO locks; the rule + push run under the
///        destination queue lock.  The sender id must already be stamped
///        by the caller (kernel current task id — unforgeable).
///        Rule: a live ticket naming this sender — kernel-stamped
///        sender_id plus the sender TCB's generation still equal to the
///        recorded wait tag — is consumed and the token stamped into the
///        parallel reply_tok slot (the reply path); anything else pushes
///        untokened (legacy traffic, grants, pulses, new requests — the
///        token lives only in tickets and stamps, never on the wire, so
///        there is nothing to forge).  Identity is proven on
///        find_task(m.sender_id) — never on dest: dest is the reply
///        recipient (the waiter) while the wait tag was captured from the
///        Call destination (the server) — see INV-MK-3, issue #581.
/// @param dest  Recipient TCB (owns the queue and the ticket).
/// @param m     Message to deliver (never mutated).
/// @return Pushed (pushed untokened), PushedReply (pushed + stamped) or
///         Full (queue full — nothing pushed).
DeliverRule deliver_with_reply_rule(TaskControlBlock &dest,
                                     Message &m,
                                     uint32_t expected_ticket = 0) noexcept {
    sync::ParkLockGuard guard(dest.msg_queue.lock_, "mq:deliver");
    const uint32_t seen =
        __atomic_load_n(&dest.reply_ticket, __ATOMIC_ACQUIRE);
    if (seen != 0) {
        if (expected_ticket != 0 && seen != expected_ticket) {
#if defined(CONFIG_DEBUG)
            __atomic_fetch_add(&g_ipc_reply_dropped, 1, __ATOMIC_RELAXED);
#endif
            return DeliverRule::Full;
        }
        const uint64_t cur_id = m.sender_id;
        const uint64_t want_id =
            __atomic_load_n(&dest.reply_wait_dest_id, __ATOMIC_ACQUIRE);
        if (cur_id != 0 && cur_id == want_id) {
            const uint32_t want_gen =
                __atomic_load_n(&dest.reply_wait_dest_gen, __ATOMIC_ACQUIRE);
            // #581: the waiter (dest) and the server are distinct tasks,
            // so dest.generation can never equal the server-side wait
            // tag — resolve the sender and compare ITS generation.
            // find_task is a lock-free id-table read (no new lock edge
            // under the queue lock).  Null sender (server reaped) or a
            // generation mismatch falls through to legacy untokened
            // delivery below; the ticket stays live for the sweep-stamp
            // recovery, never a forged stamp.
            auto *sender = Scheduler::find_task(cur_id);
            if (sender != nullptr &&
                __atomic_load_n(&sender->generation, __ATOMIC_ACQUIRE) ==
                    want_gen) {
                const uint32_t old =
                    __atomic_exchange_n(&dest.reply_ticket, 0,
                                        __ATOMIC_ACQ_REL);
                if (old == seen) {
                    const uint32_t tok = seen & ~kReplyTicketLive;
                    if (!dest.msg_queue.push_locked(m, tok))
                        return DeliverRule::Full;
                    return DeliverRule::PushedReply;
                }
            }
        }
    }
    if (!dest.msg_queue.push_locked(m, 0))
        return DeliverRule::Full;
    return DeliverRule::Pushed;
}

} // namespace

bool IPC::send(uint64_t dest_id, const Message &msg, IpcFlags flags) noexcept {
    auto *tcb = Scheduler::find_task(dest_id);
    if (!tcb || tcb->state == TaskState::TERMINATED)
        return false;

    // If the queue is already full, either return immediately (NONBLOCK)
    // or block the sender until space becomes available.
    if (tcb->msg_queue.is_full_locked()) {
        if ((flags & IpcFlags::NONBLOCK) != IpcFlags::NONE)
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
    // M3 (F3/Q2): the sender id is kernel-stamped — unforgeable by a local
    // peer — so token + stamped identity is a sound reply authenticator.
    {
        auto *stamp_cur = Scheduler::current_task();
        m.sender_id = stamp_cur ? stamp_cur->id : 0;
    }
    bool transferred = false;
    if (msg.buf_handle != 0) {
        auto *cur = Scheduler::current_task();
        if (cur) {
            transferred = BufferPool::transfer(msg.buf_handle, *cur, *tcb);
            if (!transferred)
                m.buf_handle = 0; // drop buffer association; keep message
        }
    }

    // M3 (F3/Q2): reply-token rule + push atomically under the queue
    // lock.  A full queue fails as before.
    {
        const DeliverRule rule = deliver_with_reply_rule(*tcb, m);
        if (rule == DeliverRule::Full) {
            if (transferred) {
                auto *cur = Scheduler::current_task();
                // Best-effort rollback (issue #348-A): on failure the buffer
                // stays with tcb and is reclaimed at task teardown
                // (BufferPool::unmap_all) — never leaked, never double-owned.
                if (cur)
                    (void)BufferPool::transfer(msg.buf_handle, *tcb, *cur);
            }
            return false;
        }
    }
    const bool ok = true;

    IPC_SCHED_TRACE(
        "[SEND]", "to=", dest_id, "from=",
        (Scheduler::current_task() ? Scheduler::current_task()->id : 0),
        "ty=", m.type, "q=", tcb->msg_queue.count);

    // Wake a task blocked in send_sync() waiting for a reply on its own queue.
    // send_sync sets reply_wait and blocks; without this, the reply would sit
    // in the queue and the sender would hang forever (only rescued by a
    // spurious re-dispatch).  This is the reply-delivery wakeup path.
    // M1/F7 (§11.6): ACQUIRE load pairs with the waiter's RELEASE publish.
    if (__atomic_load_n(&tcb->reply_wait, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&tcb->reply_wait, false, __ATOMIC_RELEASE);
        if (tcb->state == TaskState::BLOCKED) {
            IPC_SCHED_TRACE("[WAKE]", "dest=", dest_id,
                            "st=", static_cast<uint64_t>(tcb->state),
                            "inrq=", tcb->in_ready_queue_ ? 1u : 0u,
                            "q=", tcb->msg_queue.count);
            Scheduler::set_task_ready(*tcb);
            tcb->remaining_ticks = tcb->period_ticks;
        }
    }

    // Issue #208: wake ONLY tasks waiting on an IPC channel.  A BLOCKED
    // task may wait in a non-IPC channel (waitpid, pager fault, IRQ,
    // mutex/semaphore/eventgroup/queue) — requeuing it on an unrelated
    // message arrival resumes it with stale wait state (e.g. a waitpid
    // waiter resumes with the -1 block sentinel instead of the child's
    // pid because the waitpid wake protocol never ran).  reply_wait
    // (send_sync) is already handled above; queue-full senders
    // (blocked_on_queue) self-correct on the fullness re-check; receive
    // waiters (blocked_in_recv) consume the arrival.
    if (tcb->state == TaskState::BLOCKED &&
        (__atomic_load_n(&tcb->reply_wait, __ATOMIC_ACQUIRE) ||
         tcb->blocked_on_queue != nullptr || tcb->blocked_in_recv)) {
        Scheduler::set_task_ready(*tcb);
        tcb->remaining_ticks = tcb->period_ticks;
    }
#if defined(CONFIG_DEBUG)
    // M2 follow-up evidence (#545 SILENT): a pushed message that leaves
    // the destination BLOCKED with no wait flag set is a lost wakeup.
    if (ok && tcb->state == TaskState::BLOCKED &&
        !__atomic_load_n(&tcb->reply_wait, __ATOMIC_ACQUIRE) &&
        tcb->blocked_on_queue == nullptr && !tcb->blocked_in_recv) {
        __atomic_fetch_add(&g_ipc_wake_gate_missed, 1, __ATOMIC_RELAXED);
    }
#endif
    return ok;
}

/// @brief Receive a message from the calling task's own queue.
bool IPC::recv(Message &msg) noexcept {
    auto *cur = Scheduler::current_task();
    if (!cur)
        return false;

    bool ok = cur->msg_queue.pop(msg);
    if (ok) {
        // M3 (F3/Q2): record the delivered request's caller identity +
        // token for REPLY_RECV (implicit dest + token, kernel-held).
        note_delivered_request(*cur, msg);
        if (cur->msg_queue.blocked_senders_head) {
            wake_sender(cur->msg_queue, *cur);
        }
    }
    return ok;
}

/// @brief Record a delivered request's caller identity (M3, F3/Q2).
///        No token travels in messages (tokens live only in tickets and
///        sidecar stamps); ticket-liveness plus generation discriminate
///        at REPLY_RECV time.
void IPC::note_delivered_request(TaskControlBlock &receiver,
                                 const Message &msg) noexcept {
    auto *sender = Scheduler::find_task(msg.sender_id);
    receiver.last_caller_id = msg.sender_id;
    receiver.last_caller_gen =
        (sender != nullptr) ? sender->generation : 0;
}

/// @brief Send through a capability-gated endpoint.  The endpoint's message
///        queue is the transport (no task-ID ambient authority).  Blocks on a
///        full queue unless IpcFlags::NONBLOCK; wakes the bound receiver.
bool IPC::send_via_cap(cap::Endpoint *ep, const Message &msg, IpcFlags flags) noexcept {
    if (!ep)
        return false;
    if (ep->revoked())
        return false; // revoked endpoint refuses sends (CSpace revocation)
    if (ep->disposed_)
        return false; // freed mid-flight — never touch ep->q on a freed block

    auto *cur = Scheduler::current_task();

    // M-2 (audit-ipc-cap-syscalls-v0.4.2): a task sending to its OWN bound
    // endpoint would block forever on a full queue it can never drain.  The
    // task-ID self-send guard exists in send() but was missing here.
    if (cur && ep->bound_receiver == cur)
        return false;

    // M2 follow-up (#545 FIRE, INV-L4): the is_full_locked probe parks on
    // a contended foreign-owned depth instead of spinning with IF=0, so
    // the denied fallback below only triggers for genuine own-guard
    // scopes (fail-closed) — unreachable-by-design on the syscall trap
    // path, which holds no IrqGuard.
    if (ep->q.is_full_locked()) {
        if ((flags & IpcFlags::NONBLOCK) != IpcFlags::NONE)
            return false;
        if (!cur)
            return false;
        block_sender(ep->q, *cur);
        Scheduler::reschedule();
        if (arch::interrupts_enabled()) {
            while (cur->state == TaskState::BLOCKED) {
                arch::pause();
            }
        } else {
            unblock_sender_rollback(ep->q, *cur);
            return false;
        }
        // H-3: dispose() may have run while we were blocked — the endpoint is
        // freed, so never dereference ep->q here.  The disposed_ flag was
        // published under q.lock_ before the wake.
        if (ep->disposed_)
            return false;
        if (ep->q.is_full())
            return false;
    }

    Message m = msg;
    m.sender_id = cur ? cur->id : 0;
    {
        // M3 (F3/Q2): same token rule as IPC::send, applied against the
        // bound receiver (null receiver: no ticket can exist — strip and
        // push).  Drop is not a sender failure.
        TaskControlBlock *rule_recv = nullptr;
        {
            sync::ParkLockGuard guard(ep->lock_, "ep:bound_receiver_rule");
            rule_recv = ep->bound_receiver;
        }
        bool pushed = false;
        if (rule_recv != nullptr) {
            const DeliverRule rule = deliver_with_reply_rule(*rule_recv, m);
            pushed = (rule != DeliverRule::Full);
        } else {
            pushed = ep->q.push(m);
        }
        if (!pushed)
            return false;
    }

    TaskControlBlock *receiver = nullptr;
    {
        sync::ParkLockGuard guard(ep->lock_, "ep:bound_receiver");
        receiver = ep->bound_receiver;
    }
    // Issue #208: same gate as IPC::send — do not requeue a receiver
    // blocked in a non-IPC channel (waitpid, pager, IRQ, ...).  Endpoint
    // receive never blocks, so a BLOCKED bound_receiver waits elsewhere;
    // only IPC-channel waiters (reply_wait, queue-full senders,
    // receive waiters) may wake on message arrival.
    if (receiver && receiver->state == TaskState::BLOCKED &&
        (__atomic_load_n(&receiver->reply_wait, __ATOMIC_ACQUIRE) ||
         receiver->blocked_on_queue != nullptr ||
         receiver->blocked_in_recv)) {
        Scheduler::set_task_ready(*receiver);
        receiver->remaining_ticks = receiver->period_ticks;
    }
    return true;
}

/// @brief Receive from a capability-gated endpoint into the calling task's
///        own queue.  Non-blocking: returns false on an empty endpoint queue.
bool IPC::recv_via_cap(cap::Endpoint *ep, Message &msg) noexcept {
    if (!ep)
        return false;
    if (!ep->q.pop(msg))
        return false;
    // M3 (F3/Q2): endpoint delivery still records the caller for
    // REPLY_RECV (the reply itself goes by task id).
    if (auto *cur = Scheduler::current_task())
        note_delivered_request(*cur, msg);
    if (ep->q.blocked_senders_head) {
        auto *cur = Scheduler::current_task();
        if (cur)
            wake_sender(ep->q, *cur);
    }
    return true;
}

/// @brief Send and block until a reply arrives (client-side synchronous IPC).
///        Reply matching (issue #296): only a message whose sender_id is
///        @p dest_id is ever consumed as the reply.  Non-matching messages
///        (grants, pulses, other senders' traffic) are never touched — they
///        stay queued for their real consumer.  All other dispositions are
///        unchanged from the v1 contract (dest-death reply preservation,
///        #11 oversized-stays-queued clamp, arrival-triggered wake).
errors::IpcError IPC::send_sync_err(uint64_t dest_id, const Message &msg,
                                     Message &reply,
                                     uint32_t reply_max_size) noexcept {
    // M3 (F3/Q2): mint the one-shot reply token.  It is armed before the
    // request publish so a server dequeueing the request never observes
    // tag+flag with ticket==0 (transient STALE window); the early ticket
    // is live for the deliver's expected-ticket check.
    const uint32_t token = mint_reply_token();
    // Early ticket arm: publish tag/flag/ticket order before send so the
    // request arrival always sees a live ticket (see step 5 publish block
    // for the second store that re-publishes after tag).
    {
        auto *early_cur = Scheduler::current_task();
        if (early_cur) {
            __atomic_store_n(&early_cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_ticket, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_ticket, token | kReplyTicketLive, __ATOMIC_RELEASE);
        }
    }
    // Send the request message
    if (!send(dest_id, msg)) {
        auto *fail_cur = Scheduler::current_task();
        if (fail_cur) {
            __atomic_store_n(&fail_cur->reply_ticket, 0, __ATOMIC_RELEASE);
        }
        return errors::IPC_ERR_NO_REPLY;
    }

    // Block waiting for a reply on our own queue
    auto *cur = Scheduler::current_task();
    if (!cur)
        return errors::IPC_ERR_NO_REPLY;

    bool was_blocked = false;
    IPC_SCHED_TRACE("[SYNC]", "cur=", cur->id, "dest=", dest_id,
                    "ty=", msg.type, "q=", cur->msg_queue.count);
    // M3 audit (S2): the direct-switch cap is per wait — reset the
    // attempt counter on wait entry.  Nothing else clears it, so
    // without this the fast path dies after three lifetime waits.
    cur->ipc_direct_attempts = 0;
    for (;;) {
        if (cur->state == TaskState::BLOCKED) {
            // INV-4b: a BLOCKED task takes no locks (F1 gate-denied
            // zombie window).  Go straight back to reschedule +
            // hlt-wait instead of taking.
            Scheduler::reschedule();
            arch::sti();
            arch::hlt();
            arch::cli();
            continue;
        }
        // Matching reply already queued (covers the reply-arrived-first
        // and peer-died-after-reply cases): consume it without blocking.
        // M3 (F3/Q2): only the one-shot token match counts — other
        // senders' traffic (grants, pulses, stale replies) is never
        // touched.  A clamped oversized match stays queued per #11.
        // M3 sweep: stamp same-sender messages first — a reply pushed
        // between send() and the ticket publish below would otherwise
        // land unstamped and never match (tick-spin until peer death).
        // Try-only: a skip self-heals on the next tick recheck.
        (void)cur->msg_queue.stamp_from_try(dest_id, token);
        bool got = (reply_max_size != 0)
                       ? cur->msg_queue.pop_reply_from_clamped(
                             reply, reply_max_size, dest_id, token)
                       : cur->msg_queue.pop_reply_from(reply, dest_id,
                                                       token);
        if (got) {
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            if (was_blocked) {
                cur->remaining_ticks = cur->period_ticks;
                // Issue #513: the wait loop below ends each sleep with
                // cli(); a task that blocked at least once would otherwise
                // return to task context with SIE=0 — every later hlt
                // (including the trampoline park after entry-return) then
                // sleeps through all wakeups.  Restore the task-context
                // norm (IRQ on) on every slow-path exit.  Fast paths
                // (was_blocked == false) never touched SIE: preserve it.
                arch::sti();
            }
            return errors::IPC_ERR_OK;
        }
        // Clamped with a matching-but-oversized reply queued: fail closed
        // at once (no block) so the fastpath caller falls back to a full
        // RECEIVE — the pre-#296 immediate-false contract, preserved.
        // M3: the oversized test is token-scoped (a non-token oversized
        // message is somebody else's, never a fail-closed trigger).
        if (reply_max_size != 0 && cur->msg_queue.has_reply_from(dest_id,
                                                                 token)) {
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            if (was_blocked) {
                cur->remaining_ticks = cur->period_ticks;
                arch::sti(); // Issue #513: restore IRQ-on (see above).
            }
            return errors::IPC_ERR_NO_REPLY;
        }
        // If destination died while we were waiting for a reply, bail out.
        // BUT: a reply may already be queued (the peer delivered its reply
        // and then terminated normally).  In that case the IPC contract is
        // satisfied — pop it above on the next iteration instead of
        // discarding it; only a queue with genuinely nothing from the peer
        // means the peer died before replying.  Non-matching messages are
        // never consumed here, so the queue is left intact on failure.
        auto *dest = Scheduler::find_task(dest_id);
        // M1/F7 (§12.2): a live id whose generation no longer matches the
        // wait tag is a recycled slot — the waited-on task is gone.  The
        // generation check only applies once a wait was armed (was_blocked);
        // the unpublished tag (0) never matches a live generation.
        bool dest_gone =
            !dest || dest->state == TaskState::TERMINATED ||
            dest->state == TaskState::REAPED ||
            (was_blocked &&
             dest->generation !=
                 __atomic_load_n(&cur->reply_wait_dest_gen, __ATOMIC_ACQUIRE));
        if (dest_gone) {
            if (reply_max_size == 0 &&
                cur->msg_queue.has_reply_from(dest_id, token)) {
                continue; // unclamped match present: consume next iteration
            }
            IPC_SCHED_TRACE("[SYNC-FAIL]", "dest-gone cur=", cur->id,
                            "dest=", dest_id, "q=", cur->msg_queue.count,
                            "x=", 0u);
            // Wait is over: never return with a stale BLOCKED marking or a
            // latched reply channel — the caller resumes running now, and a
            // leftover BLOCKED/reply_wait would mislead the next scheduler
            // decision (issue #513 follow-up: stale wait-state livelock).
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            // M1/F7 park-wake bookkeeping (SMP): a cross-CPU
            // wake_reply_waiters_of can READY-mark + enqueue us between
            // the wake observation in reschedule() and this arm — writing
            // RUNNING while still queued would strand a RUNNING task in
            // the runq (briefing §1).  Dequeue first if queued.
            if (was_blocked && cur->in_ready_queue_) {
                Scheduler::dequeue_ready(*cur);
            }
            cur->state = TaskState::RUNNING;
            if (was_blocked) {
                arch::sti(); // Issue #513: restore IRQ-on (see above).
            }
            return errors::IPC_ERR_PEER_DIED;
        }

        // M1/F7 (§11.6): publish the wait tag BEFORE the reply_wait flag —
        // RELEASE order lets the death drain observe (tag, flag) atomically.
#if defined(CONFIG_DEBUG)
        // M2 follow-up evidence (#545 SILENT): publishing a reply wait
        // while a matching reply is already queued is the
        // publish-vs-arrival window (the loop-top fast path should have
        // consumed it first).  M3: token-scoped (a non-token message from
        // the peer is somebody else's traffic, not our reply).
        if (cur->msg_queue.has_reply_from(dest_id, token)) {
            __atomic_fetch_add(&g_ipc_block_with_pending_inbox, 1,
                               __ATOMIC_RELAXED);
        }
#endif
        // F1 (#545 SILENT, INV-4b): the post-publish Dekker check as a
        // try-once probe INSIDE the publish guard, AFTER the publish
        // stores (Dekker order: publish then recheck — a probe placed
        // before the publish misses a push+read landing between the two
        // on SMP, losing the wake with no post-publish drain left to
        // catch it).  A reply that arrived between the loop-top empty
        // check and the publish is consumed instead of
        // armed-and-slept-through — and because the probe runs with IF=0
        // while still RUNNING, a tick cannot preempt it mid-take (a
        // preempted take while BLOCKED strands the lock with no INV-C1
        // warning).  A Contended probe means a live holder whose
        // subsequent wake-gate read observes our publish above, so no
        // retry is needed.  A hit unpublishes (never ARMED, never
        // BLOCKED — no waker can be owed one).
        bool late_hit = false;
        bool late_oversized = false;
        {
            arch::IrqGuard publish_guard{};
            __atomic_store_n(&cur->reply_wait_dest_id, dest_id,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen,
                             dest->generation, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait, true, __ATOMIC_RELEASE);
            // M3 (F3/Q2): arm the one-shot ticket AFTER the tag publish
            // (same RELEASE-ordered window; a delivering reply observes
            // tag + flag + ticket atomically w.r.t. the tick), then sweep
            // (covers the loop-top-check to publish gap).
            __atomic_store_n(&cur->reply_ticket,
                             token | kReplyTicketLive,
                             __ATOMIC_RELEASE);
            (void)cur->msg_queue.stamp_from_try(dest_id, token);
            using QP = MessageQueue::QueueProbe;
            // M3: the Dekker probe is token-scoped — only the one-shot
            // reply counts as a late hit.
            const QP probe =
                (reply_max_size != 0)
                    ? cur->msg_queue.probe_pop_reply_from_clamped(
                          reply, reply_max_size, dest_id, token)
                    : cur->msg_queue.probe_pop_reply_from(reply, dest_id,
                                                          token);
            if (probe == QP::Consumed)
                IPC::ipc_probe_hit_mark();
            else if (probe == QP::Contended)
                IPC::ipc_probe_contended_mark();
            if (probe == QP::Consumed) {
                __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_id, 0,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_gen, 0,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
                    late_hit = true;
            } else if (probe == QP::PresentOversized) {
                __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_id, 0,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_gen, 0,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
                    late_oversized = true;
            } else {
                cur->state = TaskState::BLOCKED;
                Scheduler::dequeue_ready(*cur);
            }
        }
        if (late_hit) {
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            cur->remaining_ticks = cur->period_ticks;
            arch::sti();
            cur->state = TaskState::RUNNING;
            return errors::IPC_ERR_OK;
        }
        if (late_oversized) {
            // Clamped reply present but oversized (same as the loop-top
            // has_reply_from path): fail closed at once, never block.
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            cur->remaining_ticks = cur->period_ticks;
            arch::sti();
            cur->state = TaskState::RUNNING;
            return errors::IPC_ERR_NO_REPLY;
        }
        was_blocked = true;
        // M3 (F3/Q3): direct-switch fast path.  The caller is BLOCKED +
        // dequeued with no locks held (INV-C1/INV-4b: the publish guard
        // closed above).  On success the trap applies the arm (no tick
        // wait); on refusal (or a lost trigger) fall through to the
        // classic tick-applied park below.  Bounded attempts, then the
        // tick path unconditionally (no spin, no unbounded trap loop).
        if (cur->ipc_direct_attempts < kDirectMaxAttempts) {
            ++cur->ipc_direct_attempts;
            if (Scheduler::try_direct_switch(*cur)) {
                arch::sti();
                arch::sched_apply_trigger();
                arch::pause();
            }
        }
        if (cur->state != TaskState::BLOCKED) {
            continue; // Direct switch applied (or a wake won the race).
        }
        Scheduler::reschedule();
        // Issue #513 (face A/C hang): the wait yields UNCONDITIONALLY.
        // A kernel task reaching this loop with SIE=0 (inherited IRQ-off
        // window) would sleep through every wakeup source with a bare
        // hlt: no timer IRQ fires, the TimerWheel deadline can never
        // expire, and the bounded wait below degrades to an unexitable
        // wedge.  Trap/IPC entry owns the IRQ state (SIE=0, no locks
        // held), so sti is safe for both flavors (idempotent when SIE
        // is already 1).  Mirrors the sys_receive/sys_recv_fast fix.
        arch::sti();
        arch::hlt();
        arch::cli();
        // Resume (reply arrival or spurious non-matching wake): re-check
        // for the matching reply at the top; otherwise re-block.
    }
}

/// @brief Send and block until a reply arrives (client-side synchronous IPC).
bool IPC::send_sync(uint64_t dest_id, const Message &msg, Message &reply,
                    uint32_t reply_max_size) noexcept {
    return send_sync_err(dest_id, msg, reply, reply_max_size) ==
           errors::IPC_ERR_OK;
}

/// @brief Bounded send_sync for server proxies (issues #241, #48).
errors::IpcError IPC::send_sync_bounded_err(uint64_t dest_id,
                                            const Message &msg,
                                            Message &reply,
                                            uint64_t timeout_ticks) noexcept {
    // M3 (F3/Q2): mint the token here so the publish below always has
    // one, even when the send itself fails closed early.  The request
    // carries no token (the token lives only in tickets and stamps).
    const uint32_t token = mint_reply_token();
    {
        auto *early_cur = Scheduler::current_task();
        if (early_cur) {
            __atomic_store_n(&early_cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_ticket, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&early_cur->reply_ticket, token | kReplyTicketLive, __ATOMIC_RELEASE);
        }
    }
    if (!send(dest_id, msg)) {
        auto *fail_cur = Scheduler::current_task();
        if (fail_cur) {
            __atomic_store_n(&fail_cur->reply_ticket, 0, __ATOMIC_RELEASE);
        }
        return errors::IPC_ERR_NO_REPLY;
    }
    auto *cur = Scheduler::current_task();
    if (!cur) {
        return errors::IPC_ERR_NO_REPLY;
    }
    if (timeout_ticks != 0) {
        if (!recv_wait_arm(*cur, timeout_ticks)) {
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            return errors::IPC_ERR_NO_REPLY; // fail-closed: no arm, no block
        }
    }
    bool was_blocked = false;
    bool armed_here = (timeout_ticks != 0);
    // M3 audit (S2): per-wait attempt budget — reset on wait entry
    // (see send_sync_err; same lifetime-saturation hazard here).
    cur->ipc_direct_attempts = 0;
    for (;;) {
        if (cur->state == TaskState::BLOCKED) {
            // INV-4b: a BLOCKED task takes no locks (F1 gate-denied
            // zombie window).  Go straight back to reschedule +
            // hlt-wait instead of taking.
            Scheduler::reschedule();
            arch::sti();
            arch::hlt();
            arch::cli();
            continue;
        }
        // M3 sweep (same send-to-publish race as send_sync).
        (void)cur->msg_queue.stamp_from_try(dest_id, token);
        bool got = cur->msg_queue.pop_reply_from(reply, dest_id, token);
        if (got) {
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            if (armed_here) {
                recv_wait_cancel(*cur);
            }
            if (was_blocked) {
                cur->remaining_ticks = cur->period_ticks;
                arch::sti(); // Issue #513: restore IRQ-on (see send_sync).
            }
            return errors::IPC_ERR_OK;
        }
        if (cur->recv_timed_out) {
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            cur->recv_timed_out = false;
            recv_wait_cancel(*cur);
            // Wait over: caller resumes running (see above).
            // M1/F7 park-wake bookkeeping (SMP): dequeue first if a
            // cross-CPU death-drain wake queued us after our own wake
            // observation (same RUNNING-in-runq hazard as send_sync).
            if (was_blocked && cur->in_ready_queue_) {
                Scheduler::dequeue_ready(*cur);
            }
            cur->state = TaskState::RUNNING;
            if (was_blocked) {
                arch::sti(); // Issue #513: restore IRQ-on (see send_sync).
            }
            return errors::IPC_ERR_TIMEOUT; // bounded expiry: fail-closed
        }
        auto *dest = Scheduler::find_task(dest_id);
        // M1/F7 (§12.2): generation check gated on armed wait (see send_sync).
        bool dest_gone =
            !dest || dest->state == TaskState::TERMINATED ||
            dest->state == TaskState::REAPED ||
            (was_blocked &&
             dest->generation !=
                 __atomic_load_n(&cur->reply_wait_dest_gen, __ATOMIC_ACQUIRE));
        if (dest_gone) {
            if (cur->msg_queue.has_reply_from(dest_id, token)) {
                continue; // unclamped match present: consume next iteration
            }
            if (armed_here) {
                recv_wait_cancel(*cur);
            }
            // Wait is over (see above): clear the channel, restore RUNNING.
            // M1/F7 park-wake bookkeeping (SMP): dequeue first if queued
            // (same RUNNING-in-runq hazard as the send_sync dest-gone arm).
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            if (was_blocked && cur->in_ready_queue_) {
                Scheduler::dequeue_ready(*cur);
            }
            cur->state = TaskState::RUNNING;
            if (was_blocked) {
                arch::sti(); // Issue #513: restore IRQ-on (see send_sync).
            }
            return errors::IPC_ERR_PEER_DIED;
        }
        // M1/F7 (§11.6): publish the wait tag BEFORE the reply_wait flag.
        // F1 (#545, INV-4b): the post-publish Dekker check as a try-once
        // probe INSIDE the publish guard, AFTER the publish stores
        // (Dekker order: publish then recheck — a probe placed before
        // the publish misses a push+read landing between the two on
        // SMP).  Contended = live holder whose wake-gate read observes
        // our publish above, so no retry is needed.  A hit unpublishes
        // (never ARMED — no waker can be owed one).
        bool late_hit = false;
        {
            arch::IrqGuard publish_guard{};
            __atomic_store_n(&cur->reply_wait_dest_id, dest_id,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen,
                             dest->generation, __ATOMIC_RELAXED);
            __atomic_store_n(&cur->reply_wait, true, __ATOMIC_RELEASE);
            // M3 (F3/Q2): arm the one-shot ticket with the publish, then
            // sweep (covers the loop-top-check to publish gap).
            __atomic_store_n(&cur->reply_ticket,
                             token | kReplyTicketLive,
                             __ATOMIC_RELEASE);
            (void)cur->msg_queue.stamp_from_try(dest_id, token);
            using QP = MessageQueue::QueueProbe;
            // M3: token-scoped probe (only the one-shot reply counts).
            const QP probe =
                cur->msg_queue.probe_pop_reply_from(reply, dest_id, token);
            if (probe == QP::Consumed)
                IPC::ipc_probe_hit_mark();
            else if (probe == QP::Contended)
                IPC::ipc_probe_contended_mark();
            if (probe == QP::Consumed) {
                __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_id, 0,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_gen, 0,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
                    late_hit = true;
            } else {
                cur->state = TaskState::BLOCKED;
                Scheduler::dequeue_ready(*cur);
            }
        }
        if (late_hit) {
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_ticket, 0, __ATOMIC_RELEASE);
            if (armed_here) {
                recv_wait_cancel(*cur);
            }
            cur->remaining_ticks = cur->period_ticks;
            arch::sti();
            cur->state = TaskState::RUNNING;
            return errors::IPC_ERR_OK;
        }
        was_blocked = true;
        // M3 (F3/Q3): direct-switch fast path (same shape as send_sync;
        // the wheel timeout armed above is NOT re-armed here, so a failed
        // direct attempt can never extend the bounded wait).
        if (cur->ipc_direct_attempts < kDirectMaxAttempts) {
            ++cur->ipc_direct_attempts;
            if (Scheduler::try_direct_switch(*cur)) {
                arch::sti();
                arch::sched_apply_trigger();
                arch::pause();
            }
        }
        if (cur->state != TaskState::BLOCKED) {
            continue; // Direct switch applied (or a wake won the race).
        }
        Scheduler::reschedule();
        // Issue #513 (face A/C hang): unconditional yield (see send_sync
        // above — same SIE=0 wedge through the bounded wait below).
        arch::sti();
        arch::hlt();
        arch::cli();
    }
}

/// @brief Bounded send_sync for server proxies (issues #241, #48).
bool IPC::send_sync_bounded(uint64_t dest_id, const Message &msg,
                            Message &reply,
                            uint64_t timeout_ticks) noexcept {
    return send_sync_bounded_err(dest_id, msg, reply, timeout_ticks) ==
           errors::IPC_ERR_OK;
}

/// @brief Reply to the recorded caller and wait for the next request
///        (M3, N5: REPLY_RECV fast path body).
///        The reply is validated against the kernel-held triple, stamped
///        with the one-shot token, and delivered through the same
///        token rule as a plain send (atomic consume + stamp + push).
///        The caller triple is NOT cleared on success: a second reply
///        without an intervening new request finds the consumed token
///        and fails REPLY_STALE (one-shot).  A new request overwrites
///        the triple via note_delivered_request.
errors::IpcError IPC::reply_recv_err(Message &reply, uint32_t reply_type,
                                     uint32_t reply_len, Message &msg,
                                     uint64_t next_cap) noexcept {
    (void)next_cap; // Reserved for the cap-gated variant (always 0 today).
    auto *cur = Scheduler::current_task();
    if (!cur)
        return errors::IPC_ERR_REPLY_BAD_ARGS;
    if (reply_len > IPC_MAX_MSG_SIZE)
        return errors::IPC_ERR_REPLY_BAD_ARGS;
    // Validate the kernel-held triple (implicit dest: the server cannot
    // name a foreign caller; the live ticket is the token).
    if (cur->last_caller_id == 0)
        return errors::IPC_ERR_REPLY_NO_TOKEN;
    auto *caller = Scheduler::find_task(cur->last_caller_id);
    if (caller == nullptr || caller->generation != cur->last_caller_gen ||
        __atomic_load_n(&caller->reply_ticket, __ATOMIC_ACQUIRE) == 0) {
        return errors::IPC_ERR_REPLY_STALE;
    }
    // Defensive: reply path never leaves blocked_in_recv true on a RUNNING return
    cur->blocked_in_recv = false;
    // Build the outbound reply and deliver through the token rule (which
    // atomically consumes the ticket, stamps, and pushes; the wake gates
    // run inside deliver's caller path as for a plain send).
    Message out{};
    out.type = reply_type;
    out.data_size = reply_len;
    for (uint32_t i = 0; i < reply_len; ++i)
        out.data[i] = reply.data[i];
    out.sender_id = cur->id;
    {
        const uint32_t expected = __atomic_load_n(&caller->reply_ticket, __ATOMIC_ACQUIRE);
        const DeliverRule rule = deliver_with_reply_rule(*caller, out, expected);
        if (rule != DeliverRule::PushedReply)
            return errors::IPC_ERR_REPLY_STALE;
    }
    // Wake the caller exactly like a plain-send arrival (reply-wait flag
    // + #208 channel rule).  deliver_with_reply_rule only queued; the
    // wake gates live in send() — mirror them here (no buffer involved).
    if (__atomic_load_n(&caller->reply_wait, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&caller->reply_wait, false, __ATOMIC_RELEASE);
        if (caller->state == TaskState::BLOCKED) {
            Scheduler::set_task_ready(*caller);
            caller->remaining_ticks = caller->period_ticks;
        }
    }
    if (caller->state == TaskState::BLOCKED &&
        (__atomic_load_n(&caller->reply_wait, __ATOMIC_ACQUIRE) ||
         caller->blocked_on_queue != nullptr ||
         caller->blocked_in_recv)) {
        Scheduler::set_task_ready(*caller);
        caller->remaining_ticks = caller->period_ticks;
    }
    // Wait for the next request: the sys_receive forever-leg shape
    // (bounded hlt-wait, INV-4b gated takes, direct-switch entry).
    bool was_blocked = false;
    // M3 audit (S2): per-wait attempt budget — reset on wait entry.
    // A REPLY_RECV server loop would otherwise lose the fast path
    // after three served requests.
    cur->ipc_direct_attempts = 0;
    for (;;) {
        if (cur->state == TaskState::BLOCKED) {
            // INV-4b: a BLOCKED task takes no locks (F1 gate-denied
            // zombie window).  Go straight back to reschedule +
            // hlt-wait instead of taking.
            Scheduler::reschedule();
            arch::sti();
            arch::hlt();
            arch::cli();
            continue;
        }
        {
            bool got = cur->msg_queue.pop(msg);
            if (got) {
                note_delivered_request(*cur, msg);
                if (cur->msg_queue.blocked_senders_head)
                    wake_sender(cur->msg_queue, *cur);
                if (was_blocked) {
                    cur->remaining_ticks = cur->period_ticks;
                    arch::sti();
                }
                cur->blocked_in_recv = false;
                cur->state = TaskState::RUNNING;
                return errors::IPC_ERR_OK;
            }
        }
        bool probe_hit = false;
        {
            arch::IrqGuard publish_guard{};
            cur->blocked_in_recv = true;
            using QP = MessageQueue::QueueProbe;
            const QP probe = cur->msg_queue.probe_pop(msg);
            if (probe == QP::Consumed) {
                IPC::ipc_probe_hit_mark();
                cur->blocked_in_recv = false;
                probe_hit = true;
            } else {
                if (probe == QP::Contended)
                    IPC::ipc_probe_contended_mark();
                cur->state = TaskState::BLOCKED;
                Scheduler::dequeue_ready(*cur);
            }
        }
        // wake_sender half runs AFTER the guard (it takes the queue
        // lock itself): mirrors IPC::recv's pop+wake pairing.
        if (probe_hit) {
            note_delivered_request(*cur, msg);
            if (cur->msg_queue.blocked_senders_head)
                wake_sender(cur->msg_queue, *cur);
            if (was_blocked) {
                cur->remaining_ticks = cur->period_ticks;
                arch::sti();
            }
            cur->blocked_in_recv = false;
            cur->state = TaskState::RUNNING;
            return errors::IPC_ERR_OK;
        }
        was_blocked = true;
        // M3 (F3/Q3): direct-switch fast path (same shape as send_sync;
        // the waiter owns no timeout arm here — the wait is forever, so
        // no re-arm discipline applies).
        if (cur->ipc_direct_attempts < kDirectMaxAttempts) {
            ++cur->ipc_direct_attempts;
            if (Scheduler::try_direct_switch(*cur)) {
                arch::sti();
                arch::sched_apply_trigger();
                arch::pause();
            }
        }
        if (cur->state != TaskState::BLOCKED) {
            continue; // Direct switch applied (or a wake won the race).
        }
        Scheduler::reschedule();
        arch::sti();
        arch::hlt();
        arch::cli();
        // Resume: recheck the inbox first, gated on non-BLOCKED (INV-4b).
        if (cur->state != TaskState::BLOCKED) {
            bool got2 = cur->msg_queue.pop(msg);
            if (got2) {
                note_delivered_request(*cur, msg);
                if (cur->msg_queue.blocked_senders_head)
                    wake_sender(cur->msg_queue, *cur);
                cur->remaining_ticks = cur->period_ticks;
                arch::sti();
                cur->blocked_in_recv = false;
                cur->state = TaskState::RUNNING;
                return errors::IPC_ERR_OK;
            }
        }
    }
}

/// @brief Return a reference to a task's message queue (asserts existence).
MessageQueue &IPC::queue(uint64_t task_id) {
    auto *tcb = Scheduler::find_task(task_id);
    ENSURE(tcb != nullptr);
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
///     request a deferred context switch (INV-4).  A BLOCKED caller is
///     parked by reschedule() until woken (INV-4a) instead of running on
///     while BLOCKED.  The spin-wait loop in
///     `IPC::send()` handles gate-denied contexts.  Calling `reschedule()` without
///     having dequeued the task first would leave it BLOCKED+inrq and
///     trigger the [WEDGE] invariant violation on the very next tick.
///   - When IPC::send's spin-wait is active, the task's state is temporarily
///     BLOCKED on-CPU — this is the one legitimate case of a BLOCKED task
///     having CPU ownership (INV-4 deferred switch).  The ready-queue
///     invariant is what matters; CPU ownership is separate.
///
/// # WEDGE-avoidance summary
///
/// Every transition to BLOCKED must be atomic with dequeue_ready, and
/// every waiter-list link must precede the BLOCKED transition:
///   | code                                              | invariant |
///   |---------------------------------------------------|-----------|
///   | link; state = BLOCKED; + dequeue_ready();         | OK        |
///   | state = BLOCKED; (no dequeue)                      | WEDGE     |
///   | state = BLOCKED; + dequeue, link after (racy)      | WEDGE     |
///
/// This function implements the correct pattern (link + BLOCKED +
/// dequeue under q.lock_ + IrqGuard).  Callers that set
/// `state = BLOCKED` independently (e.g., `monitor_task_entry`,
/// `Scheduler::terminate`) must also dequeue first.
bool IPC::block_sender(MessageQueue &q, TaskControlBlock &task) {
    // INV-C2 (M2 follow-up #545 SILENT): queue lock first, then the
    // IRQ-atomic publish inside it.  Linking before the BLOCKED transition
    // closes the pop-vs-link lost-wakeup window: dequeue_ready is lock-free
    // (ledger S5), so no scheduler lock nests inside q.lock_.
    sync::ParkLockGuard guard(q.lock_, "mq:block_sender");
    {
        arch::IrqGuard irq_guard{};
        task.blocked_on_queue = &q;
        TaskControlBlock **pp = &q.blocked_senders_head;
        while (*pp && (*pp)->priority >= task.priority)
            pp = &(*pp)->blocked_next;
        task.blocked_next = *pp;
        *pp = &task;
        if (!task.blocked_next)
            q.blocked_senders_tail = &task;

        task.state = TaskState::BLOCKED;
        kernel::Scheduler::dequeue_ready(task);

        // Priority inheritance — boost owner when higher-priority sender
        // blocks. FIX(sched-race): the read-modify-write of q.owner->priority
        // and the move_priority() re-index stay atomic against the timer
        // ISR's deadline demote / sporadic priority changes (same IrqGuard
        // as the publish above).  q.lock_ is a plain spinlock (does not
        // mask IRQs); IrqGuard excludes the ISR so old/new
        // effective_priority snapshots stay consistent.
        if (q.owner && task.priority > q.owner->priority) {
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
        // F1 (#545, INV-4b): release the queue lock while still under the
        // caller's IrqGuard, so BLOCKED-marking and unlock are one atomic,
        // IF=0 step — a tick cannot preempt between them and strand the
        // lock with the task descheduled (no INV-C1 check on that path).
        guard.release();
    }

    return true;
}

/// @brief Unblock the oldest blocked sender and restore receiver priority.
void IPC::wake_sender(MessageQueue &q, TaskControlBlock &receiver) {
    TaskControlBlock *task;

    // Pop sender from head under the queue lock.  set_task_ready and priority
    // restore run outside the lock (they take scheduler_lock_ internally).
    {
        sync::ParkLockGuard guard(q.lock_, "mq:wake_sender_pop");

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
        sync::ParkLockGuard guard(q.lock_, "mq:wake_sender_pi");
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

/// @brief Wake every reply waiter of a dying task (M1/F7, INV-MK-11).
void IPC::wake_reply_waiters_of(TaskControlBlock &dead) noexcept {
    // No scheduler lock held (set_task_ready takes it on the not-own-CPU
    // branch).  IrqGuard pins the all_tasks_ walk against a concurrent
    // recycle on this CPU; cross-CPU teardown is serialized by the
    // waiter's own loop-top re-check (tag + find_task + state).
    arch::IrqGuard irq_guard{};
    const uint64_t dead_id = dead.id;
    const uint32_t dead_gen = dead.generation;
    Scheduler::TaskIter it{};
    for (TaskControlBlock *w = it.next(&dead); w != nullptr;
         w = it.next(&dead)) {
        // §12.1 dead-state filter: never touch TERMINATED/REAPED TCBs.
        if (w->state == TaskState::TERMINATED || w->state == TaskState::REAPED)
            continue;
        // M1/F7 (§11.6): ACQUIRE the flag first; the tag is only meaningful
        // when the flag was observably set.
        if (!__atomic_load_n(&w->reply_wait, __ATOMIC_ACQUIRE))
            continue;
        if (__atomic_load_n(&w->reply_wait_dest_id, __ATOMIC_ACQUIRE) !=
            dead_id)
            continue;
        if (__atomic_load_n(&w->reply_wait_dest_gen, __ATOMIC_ACQUIRE) !=
            dead_gen)
            continue;
        // Wake-only (never arms a switch, never clears the waiter's flag):
        // the waiter owns its reply_wait clear and re-checks the queued
        // reply first at loop top, so a pre-death reply is never lost (#296).
        Scheduler::set_task_ready(*w);
        w->remaining_ticks = w->period_ticks;
    }
}

uint64_t IPC::ipc_wake_gate_missed_count() noexcept {
#if defined(CONFIG_DEBUG)
    return __atomic_load_n(&g_ipc_wake_gate_missed, __ATOMIC_RELAXED);
#else
    return 0;
#endif
}

uint64_t IPC::ipc_block_with_pending_inbox_count() noexcept {
#if defined(CONFIG_DEBUG)
    return __atomic_load_n(&g_ipc_block_with_pending_inbox, __ATOMIC_RELAXED);
#else
    return 0;
#endif
}

uint64_t IPC::ipc_probe_hit_count() noexcept {
#if defined(CONFIG_DEBUG)
    return __atomic_load_n(&g_ipc_probe_hit, __ATOMIC_RELAXED);
#else
    return 0;
#endif
}

uint64_t IPC::ipc_probe_contended_count() noexcept {
#if defined(CONFIG_DEBUG)
    return __atomic_load_n(&g_ipc_probe_contended, __ATOMIC_RELAXED);
#else
    return 0;
#endif
}

uint64_t IPC::ipc_reply_dropped_count() noexcept {
#if defined(CONFIG_DEBUG)
    return __atomic_load_n(&g_ipc_reply_dropped, __ATOMIC_RELAXED);
#else
    return 0;
#endif
}

#if defined(CONFIG_DEBUG)
void IPC::ipc_probe_hit_mark() noexcept {
    __atomic_fetch_add(&g_ipc_probe_hit, 1, __ATOMIC_RELAXED);
}

void IPC::ipc_probe_contended_mark() noexcept {
    __atomic_fetch_add(&g_ipc_probe_contended, 1, __ATOMIC_RELAXED);
}
#endif

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

void IPC::recv_timeout_fire(void *context) noexcept {
    auto *task =
        static_cast<TaskControlBlock *>(context);
    if (!TaskControlBlock::is_valid(task)) {
        return;
    }
    if (task->generation != task->recv_timeout_gen) {
        return;
    }
    if (!__atomic_load_n(&task->recv_timeout_armed, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&task->recv_timed_out, true, __ATOMIC_RELEASE);
}

bool IPC::recv_wait_arm(TaskControlBlock &task,
                        uint64_t timeout_ticks) noexcept {
    // Consume any stale expiry from a previous wait first: the resume
    // path below only sets this flag, so a stale true would fake a
    // timeout on entry (e.g. a later forever-wait after a timed one).
    __atomic_store_n(&task.recv_timed_out, false, __ATOMIC_RELEASE);
    if (timeout_ticks == 0) {
        return true;
    }
    if (CONFIG_TICK_HZ == 0) {
        return false;
    }
    // Ticks to ns without 64-bit overflow (decompose around 1e9).
    const uint64_t whole = timeout_ticks / CONFIG_TICK_HZ;
    const uint64_t part = timeout_ticks % CONFIG_TICK_HZ;
    uint64_t budget_ns = 0;
    if (whole >= 0xFFFFFFFFFFFFFFFFULL / 1000000000ULL) {
        budget_ns = 0xFFFFFFFFFFFFFFFFULL;
    } else {
        budget_ns =
            whole * 1000000000ULL + (part * 1000000000ULL) / CONFIG_TICK_HZ;
    }
    if (budget_ns == 0) {
        budget_ns = 1;
    }
    const uint64_t now_ns = arch::Timer::ns_monotonic();
    // Saturating add: absurd budgets (>= 584M years) clamp to never-expire
    // instead of wrapping into an immediate deadline.
    uint64_t expiry_ns = now_ns + budget_ns;
    if (expiry_ns < now_ns) {
        expiry_ns = 0xFFFFFFFFFFFFFFFFULL;
    }
    task.recv_timeout_gen = task.generation;
    // Defensive cancel-first: never overwrite a live arm (unreachable
    // under today's single-arm discipline, closed by construction).
    if (__atomic_load_n(&task.recv_timeout_armed, __ATOMIC_ACQUIRE)) {
        recv_wait_cancel(task);
    }
    if (!time::TimerWheel::arm(0, expiry_ns, recv_timeout_fire, &task,
                               &task.recv_timeout_handle)) {
        __atomic_store_n(&task.recv_timeout_armed, false, __ATOMIC_RELEASE);
        return false;
    }
    __atomic_store_n(&task.recv_timeout_armed, true, __ATOMIC_RELEASE);
    return true;
}

void IPC::recv_wait_cancel(TaskControlBlock &task) noexcept {
    if (!__atomic_load_n(&task.recv_timeout_armed, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&task.recv_timeout_armed, false, __ATOMIC_RELEASE);
    time::TimerWheel::cancel(task.recv_timeout_handle);
}

} // namespace kernel
