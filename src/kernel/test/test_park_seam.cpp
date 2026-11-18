/// @file kernel/test/test_park_seam.cpp
/// @brief #578 park-test seam implementation (harness infrastructure).
///
/// NOTE (audit): this TU defines NO JARVIS_TEST and registers nothing — it
/// is harness infrastructure (like test_isolate.cpp), not a main-branch
/// test edit. QE asserts live on testbed under TF_PARK_DEPTH0.
#include <kernel/test/park_seam.hpp>

#if defined(CONFIG_DEBUG)

#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace test {

bool park_seam_supported() noexcept {
    return true;
}

bool park_seam_active() noexcept {
    return Scheduler::is_test_active() && gs::get_park_test_ctx();
}

bool park_ctx_window_open() noexcept {
    // Order: cheapest checks first. All reads are per-CPU loads (no lock —
    // arming happens only in test context on this CPU, up-stack of any
    // waiver evaluation).
    if (!Scheduler::is_test_active())
        return false;
    if (!gs::get_park_test_ctx())
        return false;
    if (arch::irq_guard_depth_own() != 1)
        return false;
    if (arch::irq_guard_owner_own() == arch::irq_guard_owner_token())
        return false;
    // IF==1 is the hard requirement: an IrqGuard ctor cli()s, so a set IF
    // proves no live guard owns this CPU, which is what makes the park's
    // sti/hlt legal. IF==0 keeps the denial (fail-closed, covers the QE
    // manual-nesting test that holds IF=0 via IrqGuard).
    if (!arch::interrupts_enabled())
        return false;
    if (sync::spinlock_held_by_current_count() != 0)
        return false;
    if (arch::in_syscall_trap_level())
        return false;
    return true;
}

bool park_ctx_waivable(const TaskControlBlock &tcb) noexcept {
    if (!park_ctx_window_open())
        return false;
    if (!TaskControlBlock::is_valid(&tcb))
        return false;
    if (Scheduler::is_idle_task(&tcb))
        return false;
    if (tcb.state != TaskState::RUNNING)
        return false;
    if (!tcb.park_test_member)
        return false;
    return true;
}

ParkSeamScope::ParkSeamScope() noexcept : armed_(false) {
    if (!park_seam_supported())
        return;
    if (!Scheduler::is_test_active())
        return;
    auto *cur = Scheduler::current_task();
    if (cur == nullptr || cur->magic != TaskControlBlock::TCB_MAGIC)
        return;
    if (Scheduler::is_idle_task(cur))
        return;
    // The arming task must be the harness: only the suite driver runs test
    // bodies, so anything else is a misplaced scope (never arm silently).
    if (Scheduler::get_harness_task() == nullptr ||
        cur != Scheduler::get_harness_task())
        return;
    if (!arch::interrupts_enabled())
        return;
    if (sync::spinlock_held_by_current_count() != 0)
        return;
    cur->park_test_member = true;
    armed_ = gs::set_park_test_ctx(true);
    if (!armed_)
        cur->park_test_member = false;
}

ParkSeamScope::~ParkSeamScope() noexcept {
    auto *cur = Scheduler::current_task();
    if (cur != nullptr && cur->magic == TaskControlBlock::TCB_MAGIC)
        cur->park_test_member = false;
    gs::set_park_test_ctx(false);
    // Belt-and-braces: no marked task may cross the test boundary (snapshot
    // restore rewinds TCB memory anyway). Scheduler-owned walk.
    Scheduler::clear_park_test_members();
    armed_ = false;
}

} // namespace test
} // namespace kernel

#else // !defined(CONFIG_DEBUG)

namespace kernel {
namespace test {

bool park_seam_supported() noexcept {
    return false;
}

bool park_seam_active() noexcept {
    return false;
}

bool park_ctx_waivable(const TaskControlBlock &) noexcept {
    return false;
}

bool park_ctx_window_open() noexcept {
    return false;
}

ParkSeamScope::ParkSeamScope() noexcept : armed_(false) {
}

ParkSeamScope::~ParkSeamScope() noexcept {
}

} // namespace test
} // namespace kernel

#endif // defined(CONFIG_DEBUG)
