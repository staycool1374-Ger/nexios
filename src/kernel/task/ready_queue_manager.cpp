/// @file ready_queue_manager.cpp
/// @brief ReadyQueueManager implementation: O(1) enqueue/dequeue by priority.
///        Issue #582 INV-RQ-1/2: every mutation is expressed through
///        TaskQueue's IRQ-atomic typed API; refusals are typed, counted and
///        reported (never silent), and leave the TCB byte-identical.

#include <kernel/task/ready_queue_manager.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/task/scheduler_debug_internal.hpp>
#include <lib/logger.hpp>

extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);

namespace kernel {

using RunqEnqueueResult = ReadyQueueManager::RunqEnqueueResult;

// A restored queue endpoint is live when null (empty) or a still-valid TCB
// (issue #255 H5: the validity conjunction below is now named).
static bool endpoint_live(TaskControlBlock *e) {
    return e == nullptr || TaskControlBlock::is_valid(e);
}

namespace {
// One-shot latch for the forced-clear ERROR (issue #582 §5.5): the sweep
// re-enqueues the task, so only the first firing per boot is loud.
bool s_forced_clear_warned = false;
} // namespace

RunqEnqueueResult ReadyQueueManager::enqueue(TaskControlBlock &tcb,
                                             uint64_t priority) noexcept {
    // One IRQ-atomic unit at the RQM level: the try_link (flag+links) plus
    // the bitmap and rq_priority_ publish are unobservable mid-mutation.
    // Priority is clamped to the ceiling: an out-of-range level must never
    // index queues_[] (fail-closed refusal, TCB untouched).
    arch::IrqGuard irq_guard{};
    if (priority > CONFIG_PRIORITY_CEILING) [[unlikely]] {
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::RqmEnqueueRefuse,
            TaskControlBlock::is_valid(&tcb) ? tcb.id : 0, 0, 0, 0, 0, 0,
            priority);
        Scheduler::note_rqm_enqueue_refused_invalid();
        return RunqEnqueueResult::RefusedInvalid;
    }
    if (!TaskControlBlock::is_valid(&tcb)) [[unlikely]] {
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::RqmEnqueueRefuse, 0, 0, 0, 0, 0, 0,
            priority);
        Scheduler::note_rqm_enqueue_refused_invalid();
        return RunqEnqueueResult::RefusedInvalid;
    }
    if (!queues_[priority].try_link(tcb, priority)) {
        // Refused: already queued (flag set) — TCB byte-identical.
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::RqmEnqueueRefuse, tcb.id,
            tcb.in_ready_queue_ ? 1u : 0u,
            tcb.in_ready_queue_ ? 1u : 0u,
            reinterpret_cast<uint64_t>(tcb.runq_next_),
            reinterpret_cast<uint64_t>(tcb.runq_prev_), tcb.rq_priority_,
            priority);
        Scheduler::note_rqm_enqueue_refused_already();
        return RunqEnqueueResult::AlreadyQueued;
    }
    bitmap_.set(priority);
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::RqmEnqueueCommit, tcb.id, 0, 1, 0,
        reinterpret_cast<uint64_t>(tcb.runq_prev_), tcb.rq_priority_,
        priority);
    return RunqEnqueueResult::Linked;
}

TaskControlBlock *ReadyQueueManager::dequeue_highest() noexcept {
    uint64_t prio = bitmap_.get_highest_priority();
    if (prio == 0 && queues_[0].empty()) {
        return nullptr;
    }
    // Membership-flag discipline: see pop_and_detach (a dispatched task must
    // not look queued, or a later enqueue() silently drops a READY task the
    // scheduler can then never find — the `all`-suite wedge).
    return pop_and_detach(prio);
}

TaskControlBlock *ReadyQueueManager::dequeue_level(uint64_t prio) noexcept {
    if (prio == 0 || prio > CONFIG_PRIORITY_CEILING) {
        return nullptr;
    }
    // Level 0 is the idle band: the breaker never forces it (forcing idle
    // relieves no starved supervisor); the strict path still serves level 0
    // when it is the top level.
    if (queues_[prio].empty()) {
        bitmap_.clear(prio);
        return nullptr;
    }
    // Same membership-flag discipline as dequeue_highest (see pop_and_detach).
    return pop_and_detach(prio);
}

TaskControlBlock *ReadyQueueManager::pop_and_detach(uint64_t prio) noexcept {
    // Membership-flag discipline (pop_front clears flag+links+prio as one
    // unit): a dispatched task must not look queued, or a later enqueue()
    // (which refuses already-queued TCBs) silently drops a READY task the
    // scheduler can then never find — the `all`-suite wedge.
    auto *tcb = queues_[prio].pop_front();
    if (queues_[prio].empty()) {
        bitmap_.clear(prio);
    }
    return tcb;
}

TaskControlBlock *ReadyQueueManager::peek_highest() noexcept {
    uint64_t prio = bitmap_.get_highest_priority();
    if (prio == 0 && queues_[0].empty()) {
        return nullptr;
    }
    return queues_[prio].head();
}

void ReadyQueueManager::remove(TaskControlBlock &tcb,
                               uint64_t priority) noexcept {
    (void)priority;
    if (!TaskControlBlock::is_valid(&tcb))
        return;
    // rq_priority_ is the authoritative bucket hint: it is set by every
    // successful enqueue() and cleared by every detach path.  It is a hint,
    // not proof — a stale hint must never skip membership.
    const uint64_t hint = tcb.rq_priority_;
    if (hint <= CONFIG_PRIORITY_CEILING && queues_[hint].try_unlink(tcb)) {
        if (queues_[hint].empty())
            bitmap_.clear(hint);
        return;
    }
    // Hint miss (stale prio or cross-level link): bounded scan of all
    // levels once ([[unlikely]], counted).  The scan keeps the forced
    // writes off the hot path.
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::RqmRemoveHintMiss, tcb.id,
        tcb.in_ready_queue_ ? 1u : 0u, tcb.in_ready_queue_ ? 1u : 0u,
        reinterpret_cast<uint64_t>(tcb.runq_next_),
        reinterpret_cast<uint64_t>(tcb.runq_prev_), tcb.rq_priority_,
        hint);
    for (uint64_t q = 0; q <= CONFIG_PRIORITY_CEILING; ++q) {
        if (queues_[q].try_unlink(tcb)) {
            if (queues_[q].empty())
                bitmap_.clear(q);
            return;
        }
    }
    // Absent everywhere but flagged: provably unlinked (the scan above ran
    // link-authoritative try_unlink on every level).  Force-clear the stale
    // membership through the typed detach, count it, and report once with
    // an op-ring dump — the sweep re-enqueues the still-READY task.  Only
    // the target TCB is touched: its stale links are nulled, never
    // followed (following them could splice healthy nodes elsewhere).
    if (tcb.in_ready_queue_) {
        queues_[hint <= CONFIG_PRIORITY_CEILING ? hint : 0]
            .detach_out_of_band(tcb);
        Scheduler::note_runq_link_flag_desync();
        Scheduler::note_runq_desync_forced_clear();
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::RqmRemoveForcedClear, tcb.id, 1, 0,
            0, 0, 0, hint);
        if (!s_forced_clear_warned) {
            s_forced_clear_warned = true;
            kernel::Logger::error(
                "[RUNQ] hint-miss forced clear id=%lu (stale flag, "
                "nowhere linked)",
                tcb.id);
            sched_debug::sched_runq_trace_dump(tcb.id, 8);
        }
    }
}

uint64_t ReadyQueueManager::depth() const noexcept {
    uint64_t total = 0;
    for (uint64_t p = 0; p <= CONFIG_PRIORITY_CEILING; ++p)
        total += queues_[p].count();
    return total;
}

void ReadyQueueManager::detach(TaskControlBlock &tcb) noexcept {
    if (!TaskControlBlock::is_valid(&tcb))
        return;
    const uint64_t hint = tcb.rq_priority_;
    queues_[hint <= CONFIG_PRIORITY_CEILING ? hint : 0]
        .detach_out_of_band(tcb);
}

void ReadyQueueManager::move_priority(TaskControlBlock &tcb, uint64_t old_prio,
                                      uint64_t new_prio) noexcept {
    if (old_prio == new_prio)
        return;
    remove(tcb, old_prio);
    if (enqueue(tcb, new_prio) == RunqEnqueueResult::Linked) {
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::RqmPriorityCommit, tcb.id, 0, 1, 0,
            0, new_prio, new_prio);
    }
    // A refused re-link (already/invalid) is counted + traced by the
    // enqueue path itself; the task keeps its pre-move membership.
}

void ReadyQueueManager::capture_pod(ReadyQueuePOD &out) const noexcept {
    out.bitmap_hi = bitmap_.raw_hi();
    out.bitmap_lo = bitmap_.raw_lo();
    for (uint64_t i = 0; i <= CONFIG_PRIORITY_CEILING; ++i) {
        out.queue_heads[i] = reinterpret_cast<uint64_t>(queues_[i].head());
        out.queue_tails[i] = reinterpret_cast<uint64_t>(queues_[i].tail());
        out.queue_counts[i] = queues_[i].count();
    }
}

uint64_t ReadyQueueManager::restore_pod(const ReadyQueuePOD &src) noexcept {
    bitmap_.set_raw(src.bitmap_hi, src.bitmap_lo);
    uint64_t failed_count = 0;
    for (uint64_t i = 0; i <= CONFIG_PRIORITY_CEILING; ++i) {
        auto *h = reinterpret_cast<TaskControlBlock *>(src.queue_heads[i]);
        auto *t = reinterpret_cast<TaskControlBlock *>(src.queue_tails[i]);
        // The snapshot may reference TCBs that were later freed and recycled
        // onto other allocations (MemPool returns freed blocks to unrelated
        // callers).  Relinking a freed/reused pointer re-introduces a
        // corrupted node into the ready queue and produces the flaky GPF in
        // effective_priority()/pop_front().  Only restore queues whose
        // whole chain validates; otherwise drop the level to empty and let
        // the caller rebuild it from all_tasks_ (validated restore, §5.5).
        if (endpoint_live(h) && endpoint_live(t) &&
            queues_[i].restore_level(h, t, src.queue_counts[i])) {
            // Keep in_ready_queue_ consistent with the restored queues so the
            // double-enqueue guard in enqueue() stays correct after a snapshot
            // restore (otherwise a restored-queue TCB keeps a stale
            // in_ready_queue_=false and a later set_task_ready re-enqueues it,
            // corrupting the intrusive links).
            for (auto *n = queues_[i].head();
                 n && TaskControlBlock::is_valid(n); n = n->runq_next_) {
                n->in_ready_queue_ = true;
                n->rq_priority_ = i;
            }
            sched_debug::sched_runq_trace(
                sched_debug::RunqTraceTag::RqmRestoreLevel, h ? h->id : 0,
                0, 1, src.queue_heads[i], src.queue_tails[i],
                src.queue_counts[i], i);
        } else {
            // Queue dropped (head/tail invalid or chain failed validation):
            // clear the broken chain's own membership fields (flag+links+
            // prio) WITHOUT splicing: the links are untrusted, so a splice
            // could rewrite a healthy queue reached through a stale link.
            // Clearing own fields can never touch another queue.  Walk
            // bounded by the pod count and is_valid-guarded (a freed/reused
            // TCB stops the walk instead of dereferencing freed memory).
            // Later re-enqueues go through detach_out_of_band + enqueue.
            if (h) {
                auto *n = h;
                for (uint64_t guard = 0;
                     n != nullptr && guard <= src.queue_counts[i];
                     ++guard) {
                    if (!TaskControlBlock::is_valid(n))
                        break;
                    auto *next = n->runq_next_;
                    n->in_ready_queue_ = false;
                    n->runq_next_ = nullptr;
                    n->runq_prev_ = nullptr;
                    n->rq_priority_ = 0;
                    if (n == t)
                        break;
                    n = next;
                }
            }
            queues_[i].reset_level();
            bitmap_.clear(i);
            ++failed_count;
            sched_debug::sched_runq_trace(
                sched_debug::RunqTraceTag::RqmRestoreDrop, 0, 0, 0,
                src.queue_heads[i], src.queue_tails[i],
                src.queue_counts[i], i);
        }
    }
    return failed_count;
}

void ReadyQueueManager::clear_all() noexcept {
    sched_debug::sched_runq_trace(sched_debug::RunqTraceTag::RqmClearAll, 0,
                                  0, 0, 0, 0, 0, UINT64_MAX);
    for (uint64_t i = 0; i <= CONFIG_PRIORITY_CEILING; ++i) {
        while (!queues_[i].empty()) {
            auto *t = queues_[i].pop_front();
            if (t) {
                t->in_ready_queue_ = false;
                t->rq_priority_ = 0;
            }
        }
    }
}

void ReadyQueueManager::reset() noexcept {
    // Empty all per-priority queues AND clear each dropped node's
    // in_ready_queue_ flag.  TaskQueue::reset_level() only nulls
    // head/tail/count and leaves every node's in_ready_queue_=true stale —
    // which later causes enqueue()'s double-enqueue guard to REFUSE
    // re-linking a legitimately READY task (it thinks the node is still
    // queued), leaving the task READY + inrq=1 + not physically queued.
    // Walk + clear per node (validity-guarded so a dangling runq_next_
    // from a freed/reused TCB stops the walk instead of dereferencing
    // freed memory).
    for (uint64_t i = 0; i <= CONFIG_PRIORITY_CEILING; ++i) {
        for (auto *n = queues_[i].head(); n; n = n->runq_next_) {
            if (!TaskControlBlock::is_valid(n))
                break;
            n->in_ready_queue_ = false;
        }
        queues_[i].reset_level();
    }
    bitmap_.clear_all();
}
} // namespace kernel
