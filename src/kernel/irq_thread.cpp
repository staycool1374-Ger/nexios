#include <kernel/irq_thread.hpp>

#if CONFIG_THREADED_IRQS

#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/apic.hpp>
#include <kernel/arch/io.hpp>
#include <constants.hpp>
#include <kernel/task/scheduler.hpp>
#include <logger.hpp>

namespace kernel {

// ─── Static storage ──────────────────────────────────────────────────────
IrqThread IrqThread::instances_[CONFIG_MAX_THREADED_IRQS];
size_t    IrqThread::count_ = 0;

// ─── Create ──────────────────────────────────────────────────────────────

bool IrqThread::create(uint8_t vector, uint64_t priority,
                       arch::ISRHandler handler,
                       void (*isr_ack)(uint8_t vector)) {
    // Idempotent per vector: a live instance already handles this vector
    // (e.g. the keyboard IrqThread re-created after reboot_from_table).
    for (size_t i = 0; i < count_; ++i) {
        if (instances_[i].valid_ && instances_[i].vector_ == vector) {
            Logger::info("IrqThread: vector %u already active, reusing", vector);
            return true;
        }
    }
    if (count_ >= CONFIG_MAX_THREADED_IRQS) {
        Logger::error("IrqThread: max instances (%u) reached", CONFIG_MAX_THREADED_IRQS);
        return false;
    }
    if (!handler) {
        Logger::error("IrqThread: null handler for vector %u", vector);
        return false;
    }

    // Create the handler task (kernel task, no period)
    auto *tcb = TaskControlBlock::create(task_entry, priority, 0);
    if (!tcb) {
        Logger::error("IrqThread: failed to create task for vector %u", vector);
        return false;
    }

    // Name the handler task after its IRQ vector (e.g. "irq33" for the
    // keyboard), replacing the generic auto-generated "task_<id>" name.
    {
        char buf[CONFIG_TASK_NAME_LEN];
        size_t pos = 0;
        buf[pos++] = 'i';
        buf[pos++] = 'r';
        buf[pos++] = 'q';
        char rev[4];
        size_t rp = 0;
        uint32_t n = vector;
        do {
            rev[rp++] = static_cast<char>('0' + (n % 10));
            n /= 10;
        } while (n);
        while (rp > 0 && pos < CONFIG_TASK_NAME_LEN - 1)
            buf[pos++] = rev[--rp];
        buf[pos] = '\0';
        __builtin_memcpy(tcb->name, buf, pos + 1);
    }

    auto &inst = instances_[count_];
    inst.vector_   = vector;
    inst.priority_ = priority;
    inst.handler_  = handler;
    inst.isr_ack_  = isr_ack;
    inst.tcb_      = tcb;
    inst.notify_   = &tcb->notify;
    inst.ring_.reset();
    inst.valid_ = true;

    count_++;

    // Register the handler task with the scheduler so it can run task_entry()
    // and reach notify_->wait().  Without this the task is created but never
    // scheduled — ISR notifies go nowhere and threaded IRQs never fire their
    // handler (dead keyboard input in the interactive shell).
    Scheduler::add_task(*tcb);

    Logger::info("IrqThread: vector %u created (prio=%lu, tcb=%x)",
                 vector, priority, tcb->id);
    return true;
}

// ─── ISR entry ───────────────────────────────────────────────────────────

// ─── Teardown ─────────────────────────────────────────────────────────────

bool IrqThread::destroy(uint8_t vector) {
    size_t slot = count_;
    for (size_t scan = 0; scan < count_; ++scan) {
        if (instances_[scan].valid_ && instances_[scan].vector_ == vector) {
            slot = scan;
            break;
        }
    }
    if (slot >= count_) {
        return false;
    }
    auto &target = instances_[slot];
    if (!target.tcb_ || target.tcb_ == Scheduler::current_task()) {
        return false;
    }

    // Invalidate first: for_vector()/is_irq_thread_task() stop matching, and
    // an in-flight ISR observes null notify_ (isr_entry null-checks it)
    // instead of a half-torn instance.  Task context only by contract.
    target.valid_ = false;
    TaskControlBlock *handler_task = target.tcb_;
    target.tcb_ = nullptr;
    target.notify_ = nullptr;

    // Compact the table: move the last slot into the hole field by field.
    // The SPSC ring is not copyable, so it is reset instead of moved — its
    // bytes belonged to the destroyed instance.  Keeps create() append-only.
    size_t last = count_ - 1;
    if (slot != last) {
        auto &source = instances_[last];
        target.vector_ = source.vector_;
        target.priority_ = source.priority_;
        target.handler_ = source.handler_;
        target.isr_ack_ = source.isr_ack_;
        target.tcb_ = source.tcb_;
        target.notify_ = source.notify_;
        target.ring_.reset();
        target.valid_ = true;
        source.valid_ = false;
        source.tcb_ = nullptr;
        source.notify_ = nullptr;
    }
    count_ = last;

    // Synchronously terminate and reap: a TERMINATED-but-unreaped zombie
    // would be re-terminated by the test-boundary cleanup (double
    // zombie-append), and the task count must be back at baseline before
    // returning.  The quiescent handler is BLOCKED in its Notify wait; its
    // waiter slot dies with the TCB, and Notify::notify on a TERMINATED
    // waiter is a guarded no-op, so no use-after-free is possible.
    if (handler_task->state != TaskState::TERMINATED) {
        Scheduler::terminate(*handler_task, 0);
    }
    Scheduler::drain_zombie_list();
    return true;
}

void IrqThread::isr_entry(uint8_t vector, uint64_t error_code, uint64_t rip) {
    (void)error_code;
    (void)rip;

    auto *irqt = for_vector(vector);
    if (!irqt)
        return;

    // 1. Default ack: call the arch-generic interrupt controller EOI.
    //    On x86_64 this sends PIC EOI; APIC EOI is handled separately
    //    via a custom isr_ack in the keyboard (and future) registration.
    //    On AArch64 this calls GIC EOI; on RISC-V this calls PLIC complete.
    //    Issue #26: threaded handlers inherit the interrupted context's
    //    TPR shadow and must not raise except via TprGuard (RAII).
    if (irqt->isr_ack_) {
        irqt->isr_ack_(vector);
    } else {
        arch::ArchInterruptController::eoi(vector);
    }

    // 2. Wake the handler task
    //    notify() is ISR-safe: it does an atomic hand-off and calls
    //    Scheduler::set_task_ready() which enqueues the task in the
    //    ready queue for the next context switch.
    if (irqt->notify_) {
        irqt->notify_->notify(1);
    }
}

// ─── Find ────────────────────────────────────────────────────────────────

IrqThread *IrqThread::for_vector(uint8_t vector) {
    for (size_t i = 0; i < count_; ++i) {
        if (instances_[i].valid_ && instances_[i].vector_ == vector)
            return &instances_[i];
    }
    return nullptr;
}

bool IrqThread::is_irq_thread_task(const TaskControlBlock *t) noexcept {
    if (!t)
        return false;
    for (size_t i = 0; i < count_; ++i) {
        if (instances_[i].valid_ && instances_[i].tcb_ == t)
            return true;
    }
    return false;
}

// ─── Push data from ISR ─────────────────────────────────────────────────

bool IrqThread::try_push_data(const uint8_t *data, size_t len) {
    // Push byte by byte into the SPSCRing.
    // SPSCRing::try_push is lock-free and ISR-safe.
    for (size_t i = 0; i < len; ++i) {
        if (!ring_.try_push(data[i])) {
            // Ring full — discard remaining data
            return false;
        }
    }
    return true;
}

// ─── Task entry ─────────────────────────────────────────────────────────

void IrqThread::task_entry() {
    // Find the IrqThread instance that owns this task.
    // We match by current_task()->notify pointer.
    auto *self = Scheduler::current_task();
    IrqThread *irqt = nullptr;
    for (size_t i = 0; i < count_; ++i) {
        if (instances_[i].valid_ && instances_[i].tcb_ == self) {
            irqt = &instances_[i];
            break;
        }
    }

    if (!irqt) {
        Logger::error("IrqThread::task_entry: orphan task %x", self->id);
        Scheduler::terminate(*self, 1);
        __builtin_unreachable();
    }

    Logger::debug("IrqThread: task %x running handler for vector %u",
                  self->id, irqt->vector_);

    for (;;) {
        // Wait for the ISR to notify us
        uint64_t val = irqt->notify_->wait();
        (void)val;

        // Call the registered handler (runs in task context — can block)
        irqt->handler_(irqt->vector_, 0, 0);
    }
}

} // namespace kernel

#endif // CONFIG_THREADED_IRQS
