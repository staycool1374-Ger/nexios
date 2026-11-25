/*
 * NexIOS RTOS — S582 lock-park latch tests (issue #608)
 */

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler_lock_guard.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>

using namespace kernel;
namespace gs = kernel::gs;

namespace {

inline void s582_wait_next_tick(uint64_t &last) {
    // Advance to next distinct tick value. Timer ISR advances ticks even
    // with scheduler_lock held; spin with pause until it changes.
    uint64_t cur = arch::Timer::ticks();
    // If last is 0 (first call), just wait for any change.
    uint64_t target = cur;
    while (arch::Timer::ticks() == target) {
        arch::pause();
    }
    last = arch::Timer::ticks();
}

TaskControlBlock *s582_make_blocked_target() {
    auto *t = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        20, 10);
    if (!t)
        return nullptr;
    // Fabricate a BLOCKED runner: make it findable via Scheduler::find_task
    // (id table) and ensure it is not on any ready queue. The latch exists
    // for runners the tick observed as BLOCKED; here the tick is replaced
    // by direct arm.
    {
        arch::IrqGuard g;
        Scheduler::add_task(*t);
    }
    t->state = TaskState::BLOCKED;
    Scheduler::dequeue_ready(*t);
    return t;
}

} // namespace

// Runmode: kernel
// Testidea: Tripwire must still fire — 8 lock-evidenced ticks with a
//        latched BLOCKED runner panics (fail-closed, SCHED+17).
// Input: Hold scheduler_lock_, fabricate SelfLock latch for a BLOCKED
//        target, drive 8 distinct ticks via lockpark_service_tick_pre.
// Expect: 8th covered tick panics "sched: scheduler_lock_ unreleased"
//        (expected panic, harness PASS). Covered ==8 at panic.
// Depends: gs::lockpark_* R4' predicate, last_cover_tick dedupe
JARVIS_TEST(s582_park_tripwire_still_fires, "PRE: none | POST: none") {
    Scheduler::lockpark_reset_for_test();
    auto *target = s582_make_blocked_target();
    JARVIS_ASSERT(target != nullptr);
    gs::lockpark_latch_arm(target->id, target->generation,
                           static_cast<uint8_t>(gs::LockParkClass::SelfLock));
    JARVIS_ASSERT(gs::lockpark_is_latched(target->id, target->generation));
    // Fabricate lock evidence via per-CPU depth slot after the target is
    // created and added (add_task asserts depth==0, so set depth only after).
    scheduler_lock_depth[arch::cpu_index()] = 1;

    uint64_t last = arch::Timer::ticks();
    for (int i = 0; i < 8; ++i) {
        s582_wait_next_tick(last);
        gs::lockpark_service_tick_pre();
        // On KVM/TCG the tick ISR also runs pre/post/miss; the harness
        // call here is the task-context re-entrancy that the plan counts
        // as the combined path. With E true, each distinct tick must
        // accrue.
    }
    // Must not reach here — the 8th should have panicked.
    JARVIS_ASSERT_FMT(false, "tripwire did not panic covered=%lu",
                      gs::lockpark_covered_ticks(arch::cpu_index()));
    // Unreachable, but keep cleanup for static analysis.
    scheduler_lock_depth[arch::cpu_index()] = 0;
    Scheduler::lockpark_reset_for_test();
    Scheduler::terminate(*target, 0);
    Scheduler::drain_zombie_list();
}

// Runmode: kernel
// Testidea: Free-lock non-delivery must NOT escalate — 16 ticks with the
//        lock free reset the clock, warn once, and retain the latch.
// Input: Arm ExternalDenial for a live BLOCKED target with lock free,
//        let the tick ISR drive distinct ticks.
// Expect: escalated==0, covered==0, free_nodeliver>=16, still latched;
//        after target READY, woke_cleared==1. 16 is the minimum — extra
//        ticks due to scheduling jitter are tolerated (still free, still
//        no escalation), so the bound is >=16, not exactly 16.
// Depends: R4' free-lock reset path, free_warned one-shot
JARVIS_TEST(s582_park_free_lock_nodeliver_no_escalation,
            "PRE: none | POST: none") {
    Scheduler::lockpark_reset_for_test();
    auto *target = s582_make_blocked_target();
    JARVIS_ASSERT(target != nullptr);
    gs::lockpark_latch_arm(target->id, target->generation,
                           static_cast<uint8_t>(gs::LockParkClass::ExternalDenial));
    JARVIS_ASSERT(gs::lockpark_is_latched(target->id, target->generation));

    // Let the tick ISR drive the free-lock services. Each distinct
    // tick with E==false resets covered and bumps free_nodeliver; the
    // latch is retained. Wait until at least 16 have been observed,
    // with a timeout to avoid hanging if the ISR never runs.
    uint64_t t0 = arch::Timer::ticks();
    while (gs::lockpark_free_nodeliver_count() < 16ULL) {
        if (arch::Timer::ticks() - t0 > 10000ULL) {
            break;
        }
        arch::pause();
    }

    uint64_t esc = gs::lockpark_escalated_count();
    uint64_t cov = gs::lockpark_covered_ticks(arch::cpu_index());
    uint64_t free = gs::lockpark_free_nodeliver_count();
    bool still = gs::lockpark_is_latched(target->id, target->generation);

    bool ok = (esc == 0ULL && cov == 0ULL && free >= 16ULL && still);
    if (!ok) {
        Logger::info("free-lock: esc=%lu cov=%lu free=%lu still=%d", esc, cov,
                     free, still ? 1 : 0);
    }

    // Wake the target — next service should clear as woke.
    target->state = TaskState::READY;
    Scheduler::set_task_ready(*target);
    // Drive one service to observe the woke clear. Use IRQs off so we do
    // not race the next tick's ISR before checking.
    {
        arch::IrqGuard g;
        gs::lockpark_service_tick_pre();
    }
    uint64_t woke = gs::lockpark_woke_cleared_count();

    Scheduler::lockpark_reset_for_test();
    Scheduler::terminate(*target, 0);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, esc);
    JARVIS_ASSERT_EQ(0ULL, cov);
    JARVIS_ASSERT(free >= 16ULL);
    JARVIS_ASSERT(still);
    JARVIS_ASSERT_EQ(1ULL, woke);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Cover unit is distinct ticks — miss+post on same tick
//        increments at most once (last_cover_tick dedupe).
// Input: Hold lock, arm SelfLock, drive one tick with miss then post.
// Expect: covered==1 after first distinct tick, still 1 after second
//        pair on same tick value, 2 after next distinct tick.
// Depends: last_cover_tick dedupe at R4'
JARVIS_TEST(s582_park_cover_unit_is_ticks, "PRE: none | POST: none") {
    Scheduler::lockpark_reset_for_test();
    struct DepthRAII {
        ~DepthRAII() { scheduler_lock_depth[arch::cpu_index()] = 0; }
    } depth_raii;
    auto *target = s582_make_blocked_target();
    if (!target) {
        JARVIS_ASSERT(target != nullptr);
        return;
    }
    gs::lockpark_latch_arm(target->id, target->generation,
                           static_cast<uint8_t>(gs::LockParkClass::SelfLock));
    // Set depth after target is created/added (add_task asserts depth==0).
    scheduler_lock_depth[arch::cpu_index()] = 1;

    // Use IRQs off for manual dedupe checks to avoid ISR race.
    uint64_t c0, c1, c2, c3;
    {
        arch::IrqGuard g;
        gs::lockpark_service_tick_miss();
        c0 = gs::lockpark_covered_ticks(arch::cpu_index());
        // Same tick value (still holding IRQs off, so now hasn't changed):
        // second miss+post must not increment (dedupe).
        gs::lockpark_service_tick_miss();
        gs::lockpark_service_tick_post();
        c1 = gs::lockpark_covered_ticks(arch::cpu_index());
        gs::lockpark_service_tick_miss();
        gs::lockpark_service_tick_post();
        c2 = gs::lockpark_covered_ticks(arch::cpu_index());
    }
    // Next distinct tick: let the ISR accrue exactly one more increment.
    // Keep depth 1 and latch armed, wait for one tick, then immediately
    // freeze ticks (IRQs off) and clear depth to prevent further ISR
    // increments from reaching the 8-panic threshold before we can reset.
    uint64_t last = arch::Timer::ticks();
    s582_wait_next_tick(last);
    {
        arch::IrqGuard g;
        scheduler_lock_depth[arch::cpu_index()] = 0;
        c3 = gs::lockpark_covered_ticks(arch::cpu_index());
        if (c3 == c2) {
            // ISR may not have run yet due to IRQs off timing; drive one.
            gs::lockpark_service_tick_pre();
            c3 = gs::lockpark_covered_ticks(arch::cpu_index());
        }
    }

    // Reset before asserts so failures do not leave depth==1 for the
    // next test (which would trip the ENSURE at scheduler_registry:700).
    Logger::info("cover c0=%lu c1=%lu c2=%lu c3=%lu", c0, c1, c2, c3);
    scheduler_lock_depth[arch::cpu_index()] = 0;
    Scheduler::lockpark_reset_for_test();
    Scheduler::terminate(*target, 0);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(c0 == 1ULL);
    JARVIS_ASSERT(c1 == 1ULL);
    JARVIS_ASSERT_EQ(c1, c2);
    JARVIS_ASSERT_EQ(2ULL, c3);
    JARVIS_TEST_PASS();
}

void register_s582_park_tests() {
    Logger::info("Registering s582 park tests");
    JARVIS_REGISTER_TEST(s582_park_free_lock_nodeliver_no_escalation);
    JARVIS_REGISTER_TEST(s582_park_cover_unit_is_ticks);
}

void register_s582_park_tripwire_tests() {
    Logger::info("Registering s582 park tripwire (expected panic)");
    JARVIS_REGISTER_TEST(s582_park_tripwire_still_fires);
}
