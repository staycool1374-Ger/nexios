#include <kernel/task/all_tasks_registry.hpp>
#include <kernel/task/task.hpp>
#include <logger.hpp>

namespace kernel {

// A task pointer is only safe to dereference when it lives in the higher-half
// kernel region AND still carries the TCB magic. The address-range check MUST
// come first: a corrupted list pointer (e.g. a freed/overwritten node whose
// pri_next_/pri_prev_ holds a low poison value such as 0x55) would otherwise
// fault with a General-Protection Fault the moment we read t->magic.
static inline bool safe_tcb(const TaskControlBlock *t) noexcept {
    if (reinterpret_cast<uint64_t>(t) < 0xFFFF800000000000ULL)
        return false;
    return t->magic == TaskControlBlock::TCB_MAGIC;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
/// @brief Validate pointer at a given offset/name (for invasive diagnostics).
static void diag_check_tails(uint64_t prio, const TaskControlBlock *ptr,
                             const char *caller) noexcept {
    if (!ptr)
        return;
    if (reinterpret_cast<uint64_t>(ptr) < 0xFFFF800000000000ULL) {
        Logger::raw_write("[TAILS] CORRUPT by ");
        Logger::raw_write(caller);
        Logger::raw_write(": tails_[");
        Logger::print_dec(prio);
        Logger::raw_write("] = 0x");
        Logger::print_hex(reinterpret_cast<uint64_t>(ptr));
        Logger::raw_write(" (below HHDM, likely 0x55/0xDD poison)\n");
        return;
    }
    if (ptr->magic != TaskControlBlock::TCB_MAGIC) {
        Logger::raw_write("[TAILS] CORRUPT by ");
        Logger::raw_write(caller);
        Logger::raw_write(": tails_[");
        Logger::print_dec(prio);
        Logger::raw_write("] -> magic=0x");
        Logger::print_hex(ptr->magic);
        Logger::raw_write(" (expected TCB_MAGIC)\n");
    }
}

void AllTasksRegistry::append(TaskControlBlock &t) noexcept {
    if (!safe_tcb(&t))
        return;
    uint64_t prio = t.priority;
    if (prio > CONFIG_PRIORITY_CEILING)
        prio = CONFIG_PRIORITY_CEILING;
    t.all_bucket_ = prio;

    t.pri_next_ = nullptr;
    t.pri_prev_ = tails_[prio];

    // INVASIVE DIAG: validate tails_ before the potentially-dangerous write
    diag_check_tails(prio, tails_[prio], "append");

    if (tails_[prio]) {
        tails_[prio]->pri_next_ = &t;
    } else {
        heads_[prio] = &t;
    }
    tails_[prio] = &t;
    bitmap_.set(prio);
    ++total_;
}

void AllTasksRegistry::remove(TaskControlBlock &t) noexcept {
    uint64_t p = t.all_bucket_;
    if (p >= NUM_PRIORITIES)
        return;
    if (!t.pri_prev_ && heads_[p] != &t)
        return;
    if (safe_tcb(t.pri_prev_)) {
        t.pri_prev_->pri_next_ = t.pri_next_;
    } else {
        heads_[p] = t.pri_next_;
    }
    if (safe_tcb(t.pri_next_)) {
        t.pri_next_->pri_prev_ = t.pri_prev_;
    } else {
        tails_[p] = t.pri_prev_;
        diag_check_tails(p, tails_[p], "remove");
    }
    t.pri_next_ = nullptr;
    t.pri_prev_ = nullptr;
    if (!heads_[p])
        bitmap_.clear(p);
    --total_;
}

void AllTasksRegistry::remove_unsafe(TaskControlBlock &t) noexcept {
    // Scan all priority buckets to find which one contains t.
    // MAX_WALK prevents infinite loops from corrupted pri_next_ chains.
    static constexpr uint64_t MAX_WALK = CONFIG_MAX_TASKS * 4;
    for (uint64_t p = 0; p < NUM_PRIORITIES; ++p) {
        if (!heads_[p])
            continue;
        TaskControlBlock *prev = nullptr;
        uint64_t walked = 0;
        for (auto *cur = heads_[p]; safe_tcb(cur) && walked < MAX_WALK;
             cur = cur->pri_next_, ++walked) {
            if (cur == &t) {
                if (prev) {
                    prev->pri_next_ = t.pri_next_;
                } else {
                    heads_[p] = t.pri_next_;
                }
                if (!t.pri_next_) {
                    tails_[p] = prev;
                    diag_check_tails(p, tails_[p], "remove_unsafe");
                }
                t.pri_next_ = nullptr;
                t.pri_prev_ = nullptr;
                if (!heads_[p])
                    bitmap_.clear(p);
                --total_;
                return;
            }
            prev = cur;
        }
    }
}

/// @brief Find the priority bucket a node currently lives in.
///        A task's stored t->priority may have been changed by priority
///        inheritance WITHOUT the node being re-indexed, so we must NOT trust
///        t->priority to locate the bucket. Instead climb pri_prev_ to the head
///        of the node's chain and match it against heads_[].
__attribute__((noinline)) static uint64_t
bucket_of(const TaskControlBlock *t, const TaskControlBlock *const heads_[],
          uint64_t num_priorities) noexcept {
    if (!safe_tcb(t))
        return 0;
    const TaskControlBlock *head = t;
    // Climb pri_prev_ to the true chain head, but stop the moment a link is
    // corrupted. Without safe_tcb guard a bad pri_prev_ would dereference an
    // invalid address and fault (this was the source of the GPF in next_ptr).
    while (head->pri_prev_ && safe_tcb(head->pri_prev_))
        head = head->pri_prev_;
    for (uint64_t p = 0; p < num_priorities; ++p) {
        if (safe_tcb(heads_[p]) && heads_[p] == head)
            return p;
    }
    return 0;
}

bool AllTasksRegistry::first(TaskControlBlock *&out) const noexcept {
    for (uint64_t prio = CONFIG_PRIORITY_CEILING;
         prio != static_cast<uint64_t>(-1); --prio) {
        if (safe_tcb(heads_[prio])) {
            out = heads_[prio];
            return true;
        }
    }
    return false;
}

bool AllTasksRegistry::next(TaskControlBlock *&t) const noexcept {
    if (!safe_tcb(t))
        return false;
    if (t->pri_next_ && safe_tcb(t->pri_next_)) {
        t = t->pri_next_;
        return true;
    }
    uint64_t b = bucket_of(t, heads_, NUM_PRIORITIES);
    if (b >= NUM_PRIORITIES)
        return false;
    for (uint64_t p = b; p != static_cast<uint64_t>(-1); --p) {
        if (p == b)
            continue;
        if (safe_tcb(heads_[p])) {
            t = heads_[p];
            return true;
        }
    }
    return false;
}

void AllTasksRegistry::capture(TaskControlBlock **out,
                               uint64_t max) const noexcept {
    uint64_t idx = 0;
    // Walk from highest priority down for deterministic order
    for (uint64_t prio = CONFIG_PRIORITY_CEILING;
         prio != static_cast<uint64_t>(-1) && idx < max; --prio) {
        for (auto *t = heads_[prio]; safe_tcb(t) && idx < max;
             t = t->pri_next_) {
            out[idx++] = t;
        }
    }
    // Zero remaining slots
    while (idx < max)
        out[idx++] = nullptr;
}

void AllTasksRegistry::restore(TaskControlBlock *const *in,
                                uint64_t count) noexcept {
    clear();
    for (uint64_t i = 0; i < count; ++i) {
        if (safe_tcb(in[i]))
            append(*in[i]);
    }
}

void AllTasksRegistry::clear() noexcept {
    for (uint64_t i = 0; i < NUM_PRIORITIES; ++i) {
        heads_[i] = nullptr;
        tails_[i] = nullptr;
    }
    bitmap_.clear_all();
    total_ = 0;
}

TaskControlBlock *AllTasksRegistry::first_ptr() const noexcept {
    for (uint64_t prio = CONFIG_PRIORITY_CEILING;
         prio != static_cast<uint64_t>(-1); --prio) {
        if (safe_tcb(heads_[prio]))
            return heads_[prio];
    }
    return nullptr;
}

__attribute__((noinline)) TaskControlBlock *
AllTasksRegistry::next_ptr(TaskControlBlock *t) const noexcept {
    if (!safe_tcb(t))
        return nullptr;
    // Only follow pri_next_ when it points at a valid TCB; a corrupted link
    // would otherwise hand a bad pointer to the caller (or fault on use).
    if (t->pri_next_ && safe_tcb(t->pri_next_))
        return t->pri_next_;

    uint64_t b = bucket_of(t, heads_, NUM_PRIORITIES);
    if (b >= NUM_PRIORITIES)
        return nullptr;
    for (uint64_t p = b; p != static_cast<uint64_t>(-1); --p) {
        if (p == b)
            continue;
        if (safe_tcb(heads_[p]))
            return heads_[p];
    }
    return nullptr;
}

void AllTasksRegistry::rebuild() noexcept {
    // Capture all task pointers walking the current (possibly stale-priority)
    // lists. Every task is visited exactly once.
    TaskControlBlock *buf[CONFIG_MAX_TASKS];
    uint64_t count = 0;
    for (uint64_t prio = CONFIG_PRIORITY_CEILING;
         prio != static_cast<uint64_t>(-1); --prio) {
        for (auto *t = heads_[prio]; safe_tcb(t) && count < CONFIG_MAX_TASKS;
             t = t->pri_next_) {
            buf[count++] = t;
        }
    }
    // Re-insert using each task's current (now-correct) priority.
    clear();
    for (uint64_t i = 0; i < count; ++i) {
        append(*buf[i]);
    }
}

} // namespace kernel
// NOLINTEND(bugprone-easily-swappable-parameters)
