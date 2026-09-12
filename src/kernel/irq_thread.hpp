#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

#if CONFIG_THREADED_IRQS

#include <kernel/arch/idt.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spsc_ring.hpp>
#include <kernel/sync/notify.hpp>

namespace kernel {

/// @brief Per-vector threaded interrupt handler.
///
/// Splits interrupt handling into a minimal ISR (ack + data capture + Notify)
/// and a kernel task that runs the actual handler at a configurable priority.
/// The handler task blocks on Notify::wait() and is woken by the ISR.
///
/// Timer IRQ (vector 32 / vector 64) must never be threaded — it drives the
/// scheduler's on_tick() which is required for preemption and deadline
/// detection.
class IrqThread {
public:
    static constexpr size_t RING_CAPACITY = 64;  ///< ISR→task data ring capacity (bytes)

    /// @brief Create a threaded IRQ handler.
    /// @param vector     Interrupt vector number.
    /// @param priority   Task priority (0-127).
    /// @param handler    The handler function to run in task context.
    /// @param isr_ack    Optional ISR-level ack function (called before Notify).
    ///                   If null, the default ack (APIC EOI + optional mask) is used.
    /// @return true if the IrqThread was created successfully.
    static bool create(uint8_t vector, uint64_t priority,
                       arch::ISRHandler handler,
                       void (*isr_ack)(uint8_t vector) = nullptr);

    /// @brief ISR entry point — called from handle_interrupt_c.
    /// Captures data, calls isr_ack, notifies the handler task.
    static void isr_entry(uint8_t vector, uint64_t error_code, uint64_t rip);

    /// @brief Find the IrqThread for a given vector (or nullptr).
    static IrqThread *for_vector(uint8_t vector);

    /// @brief Tear down the IrqThread for a given vector.
    /// @param vector Interrupt vector number.
    /// @return true if a live instance was destroyed, false if none exists
    ///         for the vector (or it owns the calling task).
    /// @note Task context only: the handler task must be quiescent (BLOCKED
    ///       in its Notify wait, e.g. after masking the vector at the
    ///       interrupt controller).  The teardown is synchronous — the
    ///       handler task is terminated and reaped before returning, so no
    ///       ResourceTracker task delta survives and the test-boundary
    ///       cleanup never sees a half-torn instance.
    static bool destroy(uint8_t vector);

    /// @brief True if the given TCB belongs to a live IrqThread handler task.
    /// Used by test cleanup to avoid killing threaded-IRQ handler tasks.
    static bool is_irq_thread_task(const TaskControlBlock *t) noexcept;

    /// @brief Push data from the ISR to the handler task (lock-free).
    bool try_push_data(const uint8_t *data, size_t len);

private:
    uint8_t vector_;
    uint64_t priority_;
    arch::ISRHandler handler_;
    void (*isr_ack_)(uint8_t vector);

    TaskControlBlock *tcb_;
    sync::Notify *notify_;
    SPSCRing<uint8_t, RING_CAPACITY> ring_;

    bool valid_;

    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static IrqThread instances_[CONFIG_MAX_THREADED_IRQS];
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static size_t count_;

    /// @brief Kernel task entry point — loops: wait → handle → loop.
    static void task_entry() __attribute__((noreturn));
};

} // namespace kernel

#endif // CONFIG_THREADED_IRQS
