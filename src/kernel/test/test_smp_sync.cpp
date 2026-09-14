/*
 * NexIOS RTOS — SMP bring-up (Phase 5)
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

/// @file test_smp_sync.cpp
/// @brief SMP synchronization tests (issue #85, module 11): IRQ
///        save/restore guard contracts live here (real); reader-writer
///        locks, ticket locks, cross-CPU races and holder migration
///        have no kernel API — documented stubs.  Plain SpinLock
///        contention is covered by synchronization_spinlock.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

// Runmode: kernel
// Testidea: IrqSpinLockGuard disables interrupts on entry and restores
//           the entry state on exit: IF set → cleared inside → set
//           after; the spinlock is held inside, free after.
// Input: IF query + guard scope around a SpinLock.
// Expect: Inside: IF==0 and try_lock fails (held); after: IF==1 and
//         try_lock succeeds (released).
// Depends: sync::IrqSpinLockGuard, arch::interrupts_enabled
JARVIS_TEST(smp_sync_irq_guard_if_contract, "PRE: none | POST: none") {
    sync::SpinLock lock;
    JARVIS_ASSERT(arch::interrupts_enabled());
    {
        sync::IrqSpinLockGuard guard(lock);
        JARVIS_ASSERT(!arch::interrupts_enabled());
        JARVIS_ASSERT(!lock.try_lock());
    }
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_ASSERT(lock.try_lock());
    lock.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Explicit unlock()/relock() on the guard is balanced: after
//           unlock the lock is free and IF is restored; relock takes
//           both again; scope exit does not double-unlock.
// Input: Guard + unlock() + lock() + scope exit, IF sampled throughout.
// Expect: After unlock: IF==1, try_lock ok; after relock: IF==0,
//         try_lock fails; after scope: IF==1, try_lock ok.
// Depends: sync::IrqSpinLockGuard::unlock/lock
JARVIS_TEST(smp_sync_irq_guard_unlock_relock, "PRE: none | POST: none") {
    sync::SpinLock lock;
    {
        sync::IrqSpinLockGuard guard(lock);
        guard.unlock();
        JARVIS_ASSERT(arch::interrupts_enabled());
        JARVIS_ASSERT(lock.try_lock());
        lock.unlock();
        guard.lock();
        JARVIS_ASSERT(!arch::interrupts_enabled());
        JARVIS_ASSERT(!lock.try_lock());
    }
    JARVIS_ASSERT(arch::interrupts_enabled());
    JARVIS_ASSERT(lock.try_lock());
    lock.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SMP spinlock under true two-CPU contention: AP and BSP hammer
//           the same lock, the protected counter never loses an update.
// Input: Shared lock + counter, workers on both CPUs, N increments each.
// Expect: Final counter == 2N (no torn update, no deadlock).
// Depends: Cross-CPU worker spawn + join (not yet implemented)
JARVIS_TEST(smp_sync_two_cpu_race, "PRE: none | POST: none | PENDING: cross-CPU spawn") {
    /* Pseudocode:
     *   spawn_on_cpu(1, hammer); hammer(); join();
     *   JARVIS_ASSERT(counter == 2 * N);
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Reader-writer lock: concurrent readers share, writer is
//           exclusive against readers and writers.
// Input: N readers + 1 writer racing on an RwLock.
// Expect: No reader observes a torn write; writer starves neither way.
// Depends: sync::RwLock (not yet implemented)
JARVIS_TEST(smp_sync_rwlock, "PRE: none | POST: none | PENDING: RwLock") {
    /* Pseudocode:
     *   RwLock rw; readers hold read_lock while writer wants write_lock;
     *   JARVIS_ASSERT(exclusive phases never overlap);
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Migrating a lock holder to another CPU never corrupts the
//           lock: ownership transfers, waiter queue stays consistent.
// Input: Holder blocked on a lock, set_affinity moves it mid-wait.
// Expect: Lock still acquirable/releasable; no orphan waiter.
// Depends: Lock holder migration support (not yet implemented)
JARVIS_TEST(smp_sync_holder_migration, "PRE: none | POST: none | PENDING: migration") {
    /* Pseudocode:
     *   holder waits on lock on CPU0; move to CPU1; complete cycle;
     *   JARVIS_ASSERT(lock free && waiter list empty);
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Ticket lock grants in FIFO order under contention.
// Input: N contenders take tickets concurrently, record grant order.
// Expect: Grant order equals ticket order (no barging, no starvation).
// Depends: sync::TicketLock (not yet implemented)
JARVIS_TEST(smp_sync_ticket_fifo, "PRE: none | POST: none | PENDING: TicketLock") {
    /* Pseudocode:
     *   N tasks take_number() concurrently; JARVIS_ASSERT(grant order sorted);
     */
    JARVIS_TEST_PASS();
}

void register_smp_sync_tests() {
    Logger::info("Registering smp sync tests");
    JARVIS_REGISTER_TEST(smp_sync_irq_guard_if_contract);
    JARVIS_REGISTER_TEST(smp_sync_irq_guard_unlock_relock);
    JARVIS_REGISTER_TEST(smp_sync_two_cpu_race);
    JARVIS_REGISTER_TEST(smp_sync_rwlock);
    JARVIS_REGISTER_TEST(smp_sync_holder_migration);
    JARVIS_REGISTER_TEST(smp_sync_ticket_fifo);
}
#endif  // CONFIG_ARCH_X86_64
