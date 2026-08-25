/// @file task_queue.cpp
/// @brief TaskQueue intrusive doubly-linked list implementation.

#include <kernel/task/task_queue.hpp>
#include <kernel/task/task.hpp>

namespace kernel {

void TaskQueue::push_back(TaskControlBlock &tcb) noexcept {
    // Refuse to link a non-TCB (freed/overwritten) node — doing so would
    // corrupt the intrusive runq_next_/runq_prev_ links and later cause a
    // #GP when pop_front/remove walks them.
    if (!TaskControlBlock::is_valid(&tcb))
        return;
    if (tcb.in_ready_queue_)
        return;
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
}

TaskControlBlock *TaskQueue::pop_front() noexcept {
    // If the head pointer is non-null but the count is zero the list is
    // corrupted (e.g. a cycle produced by a stray double-enqueue).  Returning
    // early here prevents pop_front from spinning until count_ underflows and
    // hanging the scheduler inside reschedule()/next_task().
    if (!head_ || count_ == 0)
        return nullptr;
    // A freed/overwritten head (poisoned with 0xDD under CONFIG_DEBUG) must
    // not be dereferenced — M-2 (audit-task-sync-v0.4.2): drop ONLY the
    // offending head node and preserve the remaining queue instead of
    // zeroing the whole list (which orphans every other queued task and
    // suppresses the corruption rather than containing it).
    if (!TaskControlBlock::is_valid(head_)) {
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
    if (count_ > 0)
        --count_;
    return tcb;
}

void TaskQueue::remove(TaskControlBlock &tcb) noexcept {
    // Membership is AUTHORITATIVE via the intrusive links, NOT via
    // in_ready_queue_.  The flag can be cleared out-of-band (e.g. a ready_queue
    // reset()/snapshot restore_pod drop nulls head_/tail_ without clearing the
    // per-node flag), leaving a node physically linked with a stale inrq=false.
    // Gating the unlink on the flag would then make remove() a silent no-op, so
    // the still-linked node survives into MemPool::free -> use-after-free /
    // SIGILL on next dispatch.  So we splice the node out by walking the actual
    // list (links are the source of truth), regardless of the flag.  is_valid
    // guards break the walk if a freed/recycled node's runq_next_ is dangling.
    if (!TaskControlBlock::is_valid(&tcb))
        return;
    if (tcb.runq_prev_) {
        if (TaskControlBlock::is_valid(tcb.runq_prev_) &&
            tcb.runq_prev_->runq_next_ == &tcb)
            tcb.runq_prev_->runq_next_ = tcb.runq_next_;
    } else if (head_ == &tcb) {
        head_ = tcb.runq_next_;
    } else {
        // Not the head and has no prev: this node is not physically in THIS
        // queue.  Leave it untouched (the caller scans other queues).
        return;
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
    if (count_ > 0)
        --count_;
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

} // namespace kernel
