/// @file task_queue.cpp
/// @brief TaskQueue intrusive doubly-linked list implementation.
///        Issue #582 INV-RQ-1: every public mutation body is one
///        arch::IrqGuard unit (flag+links written atomically w.r.t. ISRs).

#include <kernel/task/task_queue.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/task/scheduler_debug_internal.hpp>

namespace kernel {

void TaskQueue::push_back(TaskControlBlock &tcb) noexcept {
    // Compatibility shim (test-only callers): production code uses
    // try_link and handles the typed refusal.  Passes the TCB's own
    // priority through (the old push_back never touched rq_priority_).
    (void)try_link(tcb, tcb.rq_priority_);
}

bool TaskQueue::try_link(TaskControlBlock &tcb, uint64_t level) noexcept {
    arch::IrqGuard irq_guard{};
    const uint64_t before = tcb.in_ready_queue_ ? 1u : 0u;
    // Refuse to link a non-TCB (freed/overwritten) node — doing so would
    // corrupt the intrusive runq_next_/runq_prev_ links and later cause a
    // #GP when pop_front/remove walks them.  TCB untouched (INV-RQ-2).
    if (!TaskControlBlock::is_valid(&tcb) ||
        level > CONFIG_PRIORITY_CEILING) [[unlikely]] {
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::LinkRefuseInvalid, 0, before, before,
            0, 0, 0, level);
        return false;
    }
    if (tcb.in_ready_queue_) [[unlikely]] {
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::LinkRefuseFlagged, tcb.id, before,
            before, reinterpret_cast<uint64_t>(tcb.runq_next_),
            reinterpret_cast<uint64_t>(tcb.runq_prev_), tcb.rq_priority_,
            level);
        return false;
    }
    tcb.in_ready_queue_ = true;
    tcb.runq_next_ = nullptr;
    tcb.runq_prev_ = tail_;
    if (tail_) {
        tail_->runq_next_ = &tcb;
    } else {
        head_ = &tcb;
    }
    tail_ = &tcb;
    ++count_;
    tcb.rq_priority_ = level;
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::LinkCommit, tcb.id, before, 1,
        0, reinterpret_cast<uint64_t>(tcb.runq_prev_), tcb.rq_priority_,
        level);
    return true;
}

TaskControlBlock *TaskQueue::pop_front() noexcept {
    arch::IrqGuard irq_guard{};
    // If the head pointer is non-null but the count is zero the list is
    // corrupted (e.g. a cycle produced by a stray double-enqueue).  Returning
    // early here prevents pop_front from spinning until count_ underflows and
    // hanging the scheduler inside reschedule()/next_task().
    if (!head_ || count_ == 0) [[unlikely]] {
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::PopRefuseEmpty, 0, 0, 0, 0, 0, 0,
            UINT64_MAX);
        return nullptr;
    }
    // A freed/overwritten head (poisoned with 0xDD under CONFIG_DEBUG) must
    // not be dereferenced — M-2 (audit-task-sync-v0.4.2): drop ONLY the
    // offending head node and preserve the remaining queue instead of
    // zeroing the whole list (which orphans every other queued task and
    // suppresses the corruption rather than containing it).
    if (!TaskControlBlock::is_valid(head_)) [[unlikely]] {
        auto *bad = head_;
        head_ = bad->runq_next_;
        if (head_) {
            if (TaskControlBlock::is_valid(head_))
                head_->runq_prev_ = nullptr;
            else
                head_ = nullptr;
        } else {
            tail_ = nullptr;
        }
        bad->runq_next_ = nullptr;
        bad->runq_prev_ = nullptr;
        if (count_ > 0)
            --count_;
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::PopDropInvalidHead, 0, 0, 0, 0, 0,
            0, UINT64_MAX);
        return nullptr;
    }
    auto *tcb = head_;
    tcb->in_ready_queue_ = false;
    head_ = tcb->runq_next_;
    if (head_) {
        if (TaskControlBlock::is_valid(head_))
            head_->runq_prev_ = nullptr;
        else
            head_ = nullptr;
    } else {
        tail_ = nullptr;
    }
    tcb->runq_next_ = nullptr;
    tcb->runq_prev_ = nullptr;
    tcb->rq_priority_ = 0;
    if (count_ > 0)
        --count_;
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::PopCommit, tcb->id, 1, 0, 0, 0,
        0, UINT64_MAX);
    return tcb;
}

void TaskQueue::remove(TaskControlBlock &tcb) noexcept {
    // Compatibility shim (test-only callers): production code uses
    // try_unlink and handles the typed outcome.
    (void)try_unlink(tcb);
}

bool TaskQueue::try_unlink(TaskControlBlock &tcb) noexcept {
    arch::IrqGuard irq_guard{};
    // Membership is AUTHORITATIVE via the intrusive links, NOT via
    // in_ready_queue_.  The flag can be cleared out-of-band, leaving a node
    // physically linked with a stale inrq=false.  Gating the unlink on the
    // flag would then make try_unlink a silent no-op, so the still-linked
    // node survives into MemPool::free -> use-after-free / SIGILL on next
    // dispatch.  So we splice the node out by walking the actual list
    // (links are the source of truth), regardless of the flag.  is_valid
    // guards break the walk if a freed/recycled node's runq_next_ is dangling.
    // Unlike the pre-#582 remove, a refused unlink leaves the TCB
    // byte-identical (no flag/pointer writes on the false path).
    if (!TaskControlBlock::is_valid(&tcb)) [[unlikely]] {
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::UnlinkInvalid, 0, 0, 0, 0, 0, 0,
            UINT64_MAX);
        return false;
    }
    const uint64_t before = tcb.in_ready_queue_ ? 1u : 0u;
    if (tcb.runq_prev_) {
        if (TaskControlBlock::is_valid(tcb.runq_prev_) &&
            tcb.runq_prev_->runq_next_ == &tcb)
            tcb.runq_prev_->runq_next_ = tcb.runq_next_;
        else {
            // Prev-link inconsistent: the node is not in THIS queue.
            sched_debug::sched_runq_trace(
                sched_debug::RunqTraceTag::UnlinkNotInQueue, tcb.id, before,
                before, reinterpret_cast<uint64_t>(tcb.runq_next_),
                reinterpret_cast<uint64_t>(tcb.runq_prev_),
                tcb.rq_priority_, UINT64_MAX);
            return false;
        }
    } else if (head_ == &tcb) {
        head_ = tcb.runq_next_;
    } else {
        // Not the head and has no prev: this node is not physically in THIS
        // queue.  Leave it untouched (the caller scans other queues).
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::UnlinkNotInQueue, tcb.id, before,
            before, reinterpret_cast<uint64_t>(tcb.runq_next_),
            reinterpret_cast<uint64_t>(tcb.runq_prev_), tcb.rq_priority_,
            UINT64_MAX);
        return false;
    }
    if (tcb.runq_next_) {
        if (TaskControlBlock::is_valid(tcb.runq_next_) &&
            tcb.runq_next_->runq_prev_ == &tcb)
            tcb.runq_next_->runq_prev_ = tcb.runq_prev_;
    } else if (tail_ == &tcb) {
        tail_ = tcb.runq_prev_;
    }
    tcb.in_ready_queue_ = false;
    tcb.runq_next_ = nullptr;
    tcb.runq_prev_ = nullptr;
    tcb.rq_priority_ = 0;
    if (count_ > 0)
        --count_;
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::UnlinkSplice, tcb.id, before, 0, 0, 0,
        0, UINT64_MAX);
    return true;
}

void TaskQueue::detach_out_of_band(TaskControlBlock &tcb) noexcept {
    arch::IrqGuard irq_guard{};
    const uint64_t before = tcb.in_ready_queue_ ? 1u : 0u;
    // Splice when contained (no-op + traced otherwise), then clear the
    // whole membership (flag+links+rq_priority_) as one unit.
    (void)try_unlink(tcb);
    if (TaskControlBlock::is_valid(&tcb)) {
        tcb.in_ready_queue_ = false;
        tcb.runq_next_ = nullptr;
        tcb.runq_prev_ = nullptr;
        tcb.rq_priority_ = 0;
    }
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::OutOfBandDetach,
        TaskControlBlock::is_valid(&tcb) ? tcb.id : 0, before, 0, 0, 0,
        0, UINT64_MAX);
}

bool TaskQueue::contains(const TaskControlBlock &tcb) const noexcept {
    for (TaskControlBlock *p = head_; p != nullptr; p = p->runq_next_) {
        if (!TaskControlBlock::is_valid(p))
            break;
        if (p == &tcb)
            return true;
    }
    return false;
}

void TaskQueue::reset_level() noexcept {
    arch::IrqGuard irq_guard{};
    // Nulls head/tail/count without touching nodes: node flags stay as-is
    // for the caller to repair (RQM::reset walks + clears per node).
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::ResetLevel, 0, 0, 0, 0, 0, 0,
        UINT64_MAX);
    head_ = nullptr;
    tail_ = nullptr;
    count_ = 0;
}

bool TaskQueue::restore_level(TaskControlBlock *h, TaskControlBlock *t,
                              uint64_t c) noexcept {
    arch::IrqGuard irq_guard{};
    // Empty restore must be fully null.
    if (c == 0) {
        if (h != nullptr || t != nullptr)
            return false;
        head_ = nullptr;
        tail_ = nullptr;
        count_ = 0;
        sched_debug::sched_runq_trace(
            sched_debug::RunqTraceTag::SetRawLevel, 0, 0, 0, 0, 0, 0,
            UINT64_MAX);
        return true;
    }
    if (h == nullptr || t == nullptr)
        return false;
    // Bounded walk (never past c): every node valid, prev-links
    // consistent, exact length, terminating exactly at t.
    TaskControlBlock *prev = nullptr;
    TaskControlBlock *p = h;
    uint64_t n = 0;
    while (p != nullptr && n < c) {
        if (!TaskControlBlock::is_valid(p))
            return false;
        if (p->runq_prev_ != prev)
            return false;
        prev = p;
        p = p->runq_next_;
        ++n;
    }
    if (n != c || p != nullptr || prev != t)
        return false;
    head_ = h;
    tail_ = t;
    count_ = c;
    sched_debug::sched_runq_trace(
        sched_debug::RunqTraceTag::SetRawLevel, h->id, 0, 0,
        reinterpret_cast<uint64_t>(h->runq_next_), 0, h->rq_priority_,
        UINT64_MAX);
    return true;
}

} // namespace kernel
