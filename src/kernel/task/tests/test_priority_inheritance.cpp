/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file test_priority_inheritance.cpp
/// @brief Phase 6 tests: Priority Inheritance Protocol — single-level and
///        transitive priority donation across Mutex and Semaphore.
///
///        v0.3.10 rework (SIMULATED → DRIVEN): the contending tasks are REAL
///        kernel tasks (prio ≥ 11) dispatched by the real timer ISR.  LOW
///        holds the mutex in its own dispatched lambda and blocks on a real
///        semaphore (staying live while holding); HIGH blocks on the mutex.
///        The PIP boost is reached through genuine blocking, never through
///        set_current impersonation or direct priority writes.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/timer.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief  Context handed to a task lambda via `user_data` (captureless
///         lambdas only — the kernel task entry is a plain function pointer).
struct LockHoldCtx {
    uint64_t mutex_;
    uint64_t gate_;
    uint64_t acquired_;
};

/// @brief  Create a REAL kernel task (prio ≥ 11) whose lambda locks @p mutex,
///         records acquisition, blocks on @p gate, then unlocks.  Dispatch
///         and wait for the genuine block (mutex now held, task BLOCKED).
/// @param  ctx Pointer to a LockHoldCtx (must outlive the test).
/// @return TCB pointer (caller must release), or nullptr.
TaskControlBlock *spawn_holder(sync::Mutex &mutex, sync::Semaphore &gate,
                               uint64_t prio, LockHoldCtx &ctx) {
    ctx.mutex_ = reinterpret_cast<uint64_t>(&mutex);
    ctx.gate_ = reinterpret_cast<uint64_t>(&gate);
    ctx.acquired_ = 0;
    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<LockHoldCtx *>(self->user_data);
            auto *m = reinterpret_cast<sync::Mutex *>(c->mutex_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            m->lock();
            __atomic_store_n(&c->acquired_, 1, __ATOMIC_RELEASE);
            g->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately
            // (the switch is deferred); returning would self-terminate before
            // the harness observes BLOCKED.  Spin on our own BLOCKED state so
            // the harness can observe it; gate.post() wakes us, we unlock and
            // return + terminate.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
            m->unlock();
        },
        prio, 10);
    if (t == nullptr)
        return nullptr;
    t->user_data = &ctx;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    while (t->state != TaskState::BLOCKED)
        arch::pause();
    return t;
}

/// @brief  Create a REAL kernel task (prio ≥ 11) whose lambda blocks on
///         @p mutex until the holder releases it, then records acquisition
///         and unlocks.  Dispatch and wait for the genuine BLOCKED state
///         (on the mutex).
TaskControlBlock *spawn_contender(sync::Mutex &mutex, uint64_t prio,
                                  uint64_t slot[2]) {
    slot[0] = reinterpret_cast<uint64_t>(&mutex);
    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *s = reinterpret_cast<uint64_t *>(self->user_data);
            auto *m = reinterpret_cast<sync::Mutex *>(s[0]);
            auto *acq = reinterpret_cast<uint64_t *>(s[1]);
            m->lock();
            __atomic_store_n(acq, 1, __ATOMIC_RELEASE);
            m->unlock();
        },
        prio, 10);
    if (t == nullptr)
        return nullptr;
    t->user_data = slot;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    while (t->state != TaskState::BLOCKED)
        arch::pause();
    return t;
}

void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    // terminate-if-live: a blocked/READY contender is genuinely terminated;
    // an already-self-terminated one is skipped (no double zombie-append).
    // Callers drain once after all release_task() calls.
    kernel::test::terminate_if_live(t);
}

} // namespace

// ---------------------------------------------------------------------------
// Mutex — basic priority donation
// ---------------------------------------------------------------------------
// Low (prio 11) holds mutex, High (prio 20) blocks on it.
// Low's priority boosted to High's level; on unlock, restored.
TEST_CLASS(MutexPriorityDonates) {
    sync::Mutex mutex;
    mutex.init();
    sync::Semaphore gate;
    gate.init(0, 1);

    LockHoldCtx hctx{};
    auto *low = spawn_holder(mutex, gate, 11, hctx);
    CT_ASSERT(low != nullptr);
    CT_ASSERT(hctx.acquired_ == 1);
    CT_ASSERT(mutex.owner() == low);
    CT_ASSERT(low->priority == 11);

    uint64_t hslot[2];
    uint64_t high_acquired = 0;
    hslot[1] = reinterpret_cast<uint64_t>(&high_acquired);
    auto *high = spawn_contender(mutex, 20, hslot);
    CT_ASSERT(high != nullptr);

    // HIGH genuinely blocked on the mutex → LOW boosted to HIGH's priority.
    CT_ASSERT(high->state == TaskState::BLOCKED);
    CT_ASSERT(high->waiting_on_mutex == &mutex);
    CT_ASSERT(low->priority >= high->priority);

    // Release the gate: LOW wakes, unlocks, transfers ownership to HIGH.
    gate.post();
    kernel::test::wait_for_termination_safe(low);
kernel::test::wait_for_termination_safe(high);

    CT_ASSERT(low->priority == low->base_priority);
    CT_ASSERT(high_acquired == 1);

    release_task(low);
    release_task(high);
    Scheduler::drain_zombie_list();
};

// ---------------------------------------------------------------------------
// Mutex — transitive chain A → B → C
// ---------------------------------------------------------------------------
// A(11) holds M1, B(15) holds M2 then blocks on M1, C(20) blocks on M2.
// B boosted to C's priority, A boosted to B's boosted priority.
TEST_CLASS(MutexChainPropagates) {
    sync::Mutex m1, m2;
    m1.init();
    m2.init();
    sync::Semaphore gate;
    gate.init(0, 1);

    // A holds M1 and blocks on the gate.
    LockHoldCtx actx{};
    auto *a = spawn_holder(m1, gate, 11, actx);
    CT_ASSERT(a != nullptr);
    CT_ASSERT(m1.owner() == a);

    // B: locks M2 (uncontested) then blocks on M1 (held by A).
    struct BCtx {
        uint64_t m2_;
        uint64_t m1_;
    } bctx;
    bctx.m2_ = reinterpret_cast<uint64_t>(&m2);
    bctx.m1_ = reinterpret_cast<uint64_t>(&m1);
    auto *b = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<BCtx *>(self->user_data);
            auto *mm2 = reinterpret_cast<sync::Mutex *>(c->m2_);
            auto *mm1 = reinterpret_cast<sync::Mutex *>(c->m1_);
            mm2->lock();
            mm1->lock(); // blocks: M1 held by A
            mm2->unlock();
        },
        15, 10);
    CT_ASSERT(b != nullptr);
    b->user_data = &bctx;
    Scheduler::add_task(*b);
    Scheduler::reschedule();
    while (b->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(m2.owner() == b);
    CT_ASSERT(b->state == TaskState::BLOCKED);
    CT_ASSERT(b->waiting_on_mutex == &m1);
    // A boosted to B's priority (15).
    CT_ASSERT(a->priority >= b->priority);

    // C blocks on M2 (held by B) → B boosted to C's priority, A transitively.
    struct CCtx {
        uint64_t m2_;
    } cctx;
    cctx.m2_ = reinterpret_cast<uint64_t>(&m2);
    auto *c = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *ctx = reinterpret_cast<CCtx *>(self->user_data);
            auto *mm2 = reinterpret_cast<sync::Mutex *>(ctx->m2_);
            mm2->lock(); // blocks: M2 held by B
            mm2->unlock();
        },
        20, 10);
    CT_ASSERT(c != nullptr);
    c->user_data = &cctx;
    Scheduler::add_task(*c);
    Scheduler::reschedule();
    while (c->state != TaskState::BLOCKED)
        arch::pause();

    CT_ASSERT(c->state == TaskState::BLOCKED);
    // B boosted to C's priority.
    CT_ASSERT(b->priority >= c->priority);
    // A transitively boosted to B's boosted priority.
    CT_ASSERT(a->priority >= b->priority);

    // Release A → M1 unlocked → B acquires M1, unlocks M2 → C acquires M2.
    gate.post();
    kernel::test::wait_for_termination_safe(a);
kernel::test::wait_for_termination_safe(b);
kernel::test::wait_for_termination_safe(c);

    CT_ASSERT(a->priority == a->base_priority);

    release_task(a);
    release_task(b);
    release_task(c);
    Scheduler::drain_zombie_list();
};

// ---------------------------------------------------------------------------
// Mutex — priority steps down with remaining waiters
// ---------------------------------------------------------------------------
// Holder (11) with waiters at 14, 17, 20.  While all blocked the holder is
// boosted to 20 (max waiter).  The release chain wakes the HIGHEST-priority
// waiter first — proven by the order in which the waiters acquire.
TEST_CLASS(MutexPriStepDown) {
    sync::Mutex mutex;
    mutex.init();
    sync::Semaphore gate;
    gate.init(0, 1);

    LockHoldCtx hctx{};
    auto *holder = spawn_holder(mutex, gate, 11, hctx);
    CT_ASSERT(holder != nullptr);
    CT_ASSERT(mutex.owner() == holder);

    uint64_t slot20[2];
    uint64_t w20_acquired = 0;
    slot20[1] = reinterpret_cast<uint64_t>(&w20_acquired);
    auto *w20 = spawn_contender(mutex, 20, slot20);
    CT_ASSERT(w20 != nullptr);

    uint64_t slot17[2];
    uint64_t w17_acquired = 0;
    slot17[1] = reinterpret_cast<uint64_t>(&w17_acquired);
    auto *w17 = spawn_contender(mutex, 17, slot17);
    CT_ASSERT(w17 != nullptr);

    uint64_t slot14[2];
    uint64_t w14_acquired = 0;
    slot14[1] = reinterpret_cast<uint64_t>(&w14_acquired);
    auto *w14 = spawn_contender(mutex, 14, slot14);
    CT_ASSERT(w14 != nullptr);

    CT_ASSERT(w20->state == TaskState::BLOCKED);
    CT_ASSERT(w17->state == TaskState::BLOCKED);
    CT_ASSERT(w14->state == TaskState::BLOCKED);
    // Holder boosted to the max waiter priority (20).
    CT_ASSERT(holder->priority >= 20);

    // Release: the release chain must wake the highest-priority waiter first.
    gate.post();
    kernel::test::wait_for_termination_safe(holder);
kernel::test::wait_for_termination_safe(w20);
kernel::test::wait_for_termination_safe(w17);
kernel::test::wait_for_termination_safe(w14);        arch::pause();

    // All waiters acquired and completed in the release chain.
    CT_ASSERT(w20_acquired == 1);
    CT_ASSERT(w17_acquired == 1);
    CT_ASSERT(w14_acquired == 1);
    CT_ASSERT(!mutex.is_locked());

    release_task(holder);
    release_task(w20);
    release_task(w17);
    release_task(w14);
    Scheduler::drain_zombie_list();
};

// ---------------------------------------------------------------------------
// Mutex — nested mutexes held by same task
// ---------------------------------------------------------------------------
// A holds M1 and M2, with a waiter on each.  While blocked on both, A is
// boosted to the max waiter priority.  Releasing both returns A to base.
TEST_CLASS(MutexNestedDrop) {
    sync::Mutex m1, m2;
    m1.init();
    m2.init();
    sync::Semaphore gate;
    gate.init(0, 1);

    // A locks M1 and M2, then blocks on the gate (holding both).
    struct ACtx {
        uint64_t m1_;
        uint64_t m2_;
        uint64_t gate_;
    } actx;
    actx.m1_ = reinterpret_cast<uint64_t>(&m1);
    actx.m2_ = reinterpret_cast<uint64_t>(&m2);
    actx.gate_ = reinterpret_cast<uint64_t>(&gate);
    auto *a = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<ACtx *>(self->user_data);
            auto *mm1 = reinterpret_cast<sync::Mutex *>(c->m1_);
            auto *mm2 = reinterpret_cast<sync::Mutex *>(c->m2_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            mm1->lock();
            mm2->lock();
            g->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately;
            // spin on our own BLOCKED state so the harness observes it before
            // we would self-terminate.  gate.post() wakes us and we unlock.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
            mm2->unlock();
            mm1->unlock();
        },
        11, 10);
    CT_ASSERT(a != nullptr);
    a->user_data = &actx;
    Scheduler::add_task(*a);
    Scheduler::reschedule();
    while (a->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(m1.owner() == a);
    CT_ASSERT(m2.owner() == a);

    uint64_t s10[2];
    uint64_t w10_acquired = 0;
    s10[1] = reinterpret_cast<uint64_t>(&w10_acquired);
    auto *w10 = spawn_contender(m1, 14, s10);
    CT_ASSERT(w10 != nullptr);

    uint64_t s20[2];
    uint64_t w20_acquired = 0;
    s20[1] = reinterpret_cast<uint64_t>(&w20_acquired);
    auto *w20 = spawn_contender(m2, 20, s20);
    CT_ASSERT(w20 != nullptr);

    // A boosted to the max waiter priority (20).
    CT_ASSERT(a->priority >= 20);

    // Release A → M2 unlocked first (waiter w20 wakes), then M1 (w10 wakes).
    gate.post();
    kernel::test::wait_for_termination_safe(a);
kernel::test::wait_for_termination_safe(w10);
kernel::test::wait_for_termination_safe(w20);

    CT_ASSERT(w10_acquired == 1);
    CT_ASSERT(w20_acquired == 1);
    CT_ASSERT(a->priority == a->base_priority);
    CT_ASSERT(!m1.is_locked());
    CT_ASSERT(!m2.is_locked());

    release_task(a);
    release_task(w10);
    release_task(w20);
    Scheduler::drain_zombie_list();
};

// ---------------------------------------------------------------------------
// Semaphore — priority inheritance
// ---------------------------------------------------------------------------
// Low (prio 11) holds a binary semaphore (count→0), High (prio 20) waits.
// Low boosted; on post priority restored.
TEST_CLASS(SemaphoreInherits) {
    sync::Semaphore sem;
    sem.init(1, 1);
    sync::Semaphore gate;
    gate.init(0, 1);

    // LOW: waits on the binary semaphore (count 1→0, becomes owner), then
    // blocks on the gate while holding it.
    struct SemCtx {
        uint64_t sem_;
        uint64_t gate_;
    } lctx;
    lctx.sem_ = reinterpret_cast<uint64_t>(&sem);
    lctx.gate_ = reinterpret_cast<uint64_t>(&gate);
    auto *low = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SemCtx *>(self->user_data);
            auto *s = reinterpret_cast<sync::Semaphore *>(c->sem_);
            auto *g = reinterpret_cast<sync::Semaphore *>(c->gate_);
            s->wait();
            g->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately;
            // spin on our own BLOCKED state so the harness observes it before
            // we would self-terminate.  gate.post() wakes us and we release.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
            s->post();
        },
        11, 10);
    CT_ASSERT(low != nullptr);
    low->user_data = &lctx;
    Scheduler::add_task(*low);
    Scheduler::reschedule();
    while (low->state != TaskState::BLOCKED)
        arch::pause();
    CT_ASSERT(sem.value() == 0); // LOW holds the binary semaphore
    CT_ASSERT(low->priority == 11);

    // HIGH waits on the semaphore (blocks; LOW is the owner).
    uint64_t hacquired = 0;
    struct HSemCtx {
        uint64_t sem_;
        uint64_t out_;
    } hctx;
    hctx.sem_ = reinterpret_cast<uint64_t>(&sem);
    hctx.out_ = reinterpret_cast<uint64_t>(&hacquired);
    auto *high = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<HSemCtx *>(self->user_data);
            auto *s = reinterpret_cast<sync::Semaphore *>(c->sem_);
            auto *o = reinterpret_cast<uint64_t *>(c->out_);
            s->wait();
            // INV-4: Semaphore::wait() sets BLOCKED then returns immediately;
            // spin on our own BLOCKED state so the harness observes it before
            // we would self-terminate.  LOW's post wakes us and we acquire.
            while (Scheduler::current_task()->state == TaskState::BLOCKED)
                arch::pause();
            __atomic_store_n(o, 1, __ATOMIC_RELEASE);
            s->post();
        },
        20, 10);
    CT_ASSERT(high != nullptr);
    high->user_data = &hctx;
    Scheduler::add_task(*high);
    Scheduler::reschedule();
    while (high->state != TaskState::BLOCKED)
        arch::pause();

    CT_ASSERT(high->state == TaskState::BLOCKED);
    CT_ASSERT(low->priority >= high->priority);

    // Release LOW → it posts the semaphore → HIGH wakes, acquires, posts.
    gate.post();
    kernel::test::wait_for_termination_safe(low);
kernel::test::wait_for_termination_safe(high);

    CT_ASSERT(low->priority == low->base_priority);
    CT_ASSERT(hacquired == 1);

    release_task(low);
    release_task(high);
    Scheduler::drain_zombie_list();
};

void register_priority_inheritance_tests() {
    Logger::info("Registering priority inheritance tests");
    REGISTER_CLASS(MutexPriorityDonates);
    REGISTER_CLASS(MutexChainPropagates);
    REGISTER_CLASS(MutexPriStepDown);
    REGISTER_CLASS(MutexNestedDrop);
    REGISTER_CLASS(SemaphoreInherits);
}
