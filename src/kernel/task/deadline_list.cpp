#include <kernel/task/deadline_list.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>

namespace kernel {

void DeadlineList::insert(TaskControlBlock &t) noexcept {
    // Issue #297 (live-caught wedge): self-heal against double-insert.
    // Inserting an already-linked task — same head twice, a re-insert
    // without a matching remove from any path (task, tick, restore) —
    // links the task to itself (head self-loop) and every later insert
    // spins forever in the walk below with zero diagnostic output.
    // Unlink first (no-op when absent): the end state is then always a
    // singly-linked member with an exact size_ count, whatever path led
    // here. Cost is one bounded scan; membership is small.
    remove(t);
    t.dl_next_ = nullptr;
    t.dl_prev_ = nullptr;

    if (!head_) {
        head_ = &t;
        ++size_;
        return;
    }

    if (t.deadline_ticks < head_->deadline_ticks) {
        t.dl_next_ = head_;
        head_->dl_prev_ = &t;
        head_ = &t;
        ++size_;
        return;
    }

    TaskControlBlock *cur = head_;
    // Bounded walk (issue #297): a correct list visits at most size_
    // nodes, so exceeding it proves a cycle (stray writer, dangling
    // node, or a missed unlink elsewhere). Fail stop with location
    // instead of hanging the machine silently for a harness timeout.
    size_t trips = 0;
    while (cur->dl_next_ &&
           cur->dl_next_->deadline_ticks <= t.deadline_ticks) {
        cur = cur->dl_next_;
        if (++trips > size_) {
            ENSURE(false && "deadline list cycle detected");
            return;
        }
    }

    t.dl_next_ = cur->dl_next_;
    t.dl_prev_ = cur;
    if (cur->dl_next_)
        cur->dl_next_->dl_prev_ = &t;
    cur->dl_next_ = &t;
    ++size_;
}

void DeadlineList::remove(TaskControlBlock &t) noexcept {
    if (!head_)
        return;

    if (head_ == &t) {
        head_ = t.dl_next_;
        if (head_)
            head_->dl_prev_ = nullptr;
        t.dl_next_ = nullptr;
        t.dl_prev_ = nullptr;
        --size_;
        return;
    }

    TaskControlBlock *cur = head_;
    // Bounded walk (issue #297, mirrors insert): insert() routes every call
    // through this scan first, so an unbounded walk here would hang before
    // insert's own trip-count guard on a cyclic list. Fail stop instead.
    size_t trips = 0;
    while (cur && cur != &t) {
        cur = cur->dl_next_;
        if (++trips > size_) {
            ENSURE(false && "deadline list cycle detected");
            return;
        }
    }
    if (!cur)
        return;

    if (cur->dl_prev_)
        cur->dl_prev_->dl_next_ = cur->dl_next_;
    if (cur->dl_next_)
        cur->dl_next_->dl_prev_ = cur->dl_prev_;
    cur->dl_next_ = nullptr;
    cur->dl_prev_ = nullptr;
    --size_;
}

TaskControlBlock *DeadlineList::pop_earliest_if_expired() noexcept {
    if (!head_)
        return nullptr;

    uint64_t now = arch::Timer::ticks();
    if (now <= head_->deadline_ticks)
        return nullptr;

    // Earliest task has expired — pop it
    auto *t = head_;
    head_ = t->dl_next_;
    if (head_)
        head_->dl_prev_ = nullptr;
    t->dl_next_ = nullptr;
    t->dl_prev_ = nullptr;
    --size_;
    return t;
}

} // namespace kernel
