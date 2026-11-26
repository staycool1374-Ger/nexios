/// @file sync/irq_contract.hpp
/// @brief IRQ/preemption contract enforcement points (issue #546-A).
///        wait-context classification, block-transition notes, syscall-exit
///        epilogue, violation counters + lateness high-water, halt latch,
///        test-isolation reset. Lock-free, allocation-free, no scheduler
///        services (atomics only) — safe at any ISR nesting.
#pragma once

#include <types.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/hal/irq_guard.hpp>

namespace kernel {

namespace irq_contract {

/// @brief Execution context for wait-legality (INV-CTX-1/INV-CTX-2).
enum class ExecCtx : uint8_t {
    Boot = 0,        ///< no current task
    Task = 1,        ///< task context, outside any trap
    SyscallTrap = 2, ///< inside a syscall trap level
    Isr = 3,         ///< interrupt context, outside syscall traps
};

/// @brief Violation classes (§9 taxonomy). C6 is exempt from halting.
enum class ViolClass : uint8_t {
    C1 = 1, ///< wait/block/park in Isr or Boot context
    C2,     ///< sti-owning wait with live guard depth and no DeclaredWaitScope
    C3,     ///< DeclaredWaitScope misuse (nest/row/triple)
    C4,     ///< missed/duplicate block note (ledger-audit only, no runtime assert)
    C5,     ///< INV-C1 order-table lock held at reschedule (SCHED+15)
    C6,     ///< SelfLock latch at reschedule (SCHED+16, never halts)
};

/// @brief Classify the current execution context. Three existing reads
///        only; no lock, no allocation.
inline ExecCtx classify() noexcept {
    if (isr_nesting_own() != 0 && !arch::in_syscall_trap_level()) {
        return ExecCtx::Isr;
    }
    if (arch::in_syscall_trap_level()) {
        return ExecCtx::SyscallTrap;
    }
    if (current_cpu().current != nullptr) {
        return ExecCtx::Task;
    }
    return ExecCtx::Boot;
}

/// @brief Check a wait context (INV-CTX-1/INV-CTX-2). Atomics only.
/// @param site      Short caller tag for the evidence line.
/// @param self_owned True when the waiter owns IF outright (depth == 0).
/// @param declared  True when a DeclaredWaitScope is live for this frame.
void check_wait_ctx(const char *site, bool self_owned,
                    bool declared) noexcept;

/// @brief Check a BLOCKED-store transition (INV-BLK-1/INV-BLK-2).
///        Must be called in the same non-preemptible region as the store,
///        before any reschedule()/sti()/unlock. Atomics only.
/// @param site   Short caller tag.
/// @param target The task whose state was stored (for attribution).
/// @param self   True when target == current (SELF), else FOREIGN.
void check_block_store(const char *site, const TaskControlBlock *target,
                       bool self) noexcept;

/// @brief Syscall-exit epilogue (real exit assertion). Invoked at every
///        return path of handle/handle_fast. Atomics only; no lock, no
///        alloc, no scheduler service, no serial write.
/// @param tag Exit-site tag (syscall_exit:<handler>:<path>).
void syscall_exit_epilogue(const char *tag) noexcept;

/// @brief Record a tick arrival for the lateness bound (INV-IRQOFF-1).
///        Always-on: one raw_counter read + saturating high-water update.
void note_tick_arrival() noexcept;

/// @brief Raise the deferred halt latch (certification builds, policy 2).
///        Atomics only; the halt is flushed at the next safe point.
void request_halt(const char *site) noexcept;

/// @brief Flush a latched halt at a safe point (syscall exit epilogue
///        or reschedule entry with IF==1). No-op when nothing latched.
void flush_halt() noexcept;

/// @brief Violation counters for the ledger readout (per class).
uint64_t violation_count(ViolClass cls) noexcept;

/// @brief IRQ-off lateness high-water, in raw counter cycles.
uint64_t tick_late_max_cycles() noexcept;

/// @brief Tick arrivals observed with a late predecessor.
uint64_t tick_late_count() noexcept;

/// @brief Test-isolation reset: zeroes all counters, high-water, latch.
///        QE-owned wiring in test_isolate.cpp; defined here (production).
void reset_for_test() noexcept;

/// @brief Declared sti-owning wait scope (issue #546-A, A.3). RAII marker
///        that suppresses ONLY the INV-CTX-2 C2 report for its own frame;
///        never INV-CTX-1, INV-BLK-1, INV-C1, park denies, or lock checks.
///        Non-copyable, non-movable; must not outlive its function, span a
///        syscall return, or nest. Row must be a closed-list ledger id.
class [[nodiscard]] DeclaredWaitScope {
  public:
    explicit DeclaredWaitScope(const char *row) noexcept;
    ~DeclaredWaitScope() noexcept;
    DeclaredWaitScope(const DeclaredWaitScope &) = delete;
    DeclaredWaitScope &operator=(const DeclaredWaitScope &) = delete;
    DeclaredWaitScope(DeclaredWaitScope &&) = delete;
    DeclaredWaitScope &operator=(DeclaredWaitScope &&) = delete;

  private:
    const char *row_;
};

/// @brief Scope-slot primitives (used by DeclaredWaitScope only).
/// @return Enter/exit success; false means nested live scope (enter) or
///         no live scope / triple mismatch (exit) — both class C3.
bool declared_wait_enter(const char *row) noexcept;
bool declared_wait_exit_ok(const char *row) noexcept;
/// @brief True while a DeclaredWaitScope is live on this CPU.
bool declared_wait_live() noexcept;

} // namespace irq_contract
} // namespace kernel
