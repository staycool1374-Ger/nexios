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

/// @file test_mk_death.cpp
/// @brief M1 QE (issues #545/#516): death-funnel + daemon-cell tests
///        (servers class).  Death-record visibility/boundedness, funnel
///        cell clear, stale-cell heal, reply-waiter wake on peer death
///        (INV-MK-11), waiter wake storm.  No production code touched.
///
///        Scratch daemons ride the proven qe_probe pattern (vfsd initrd
///        path, isolation drops the registration at test end).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/ipc/death_notify.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace mk_death {
// (globals below used by the storm + wake tests)

// --- pair-B rendezvous: server replies, then exits; client must still
// consume the reply (reply-before-death preservation) ---
TaskControlBlock *g_pb_server = nullptr;
volatile bool g_pb_client_ok = false;
volatile uint64_t g_pb_client_type = 0;

void pb_server_entry() {
    auto *self = Scheduler::current_task();
    (void)self;
    Message msg;
    bool ok = false;
    for (int i = 0; i < 100000 && !ok; ++i) {
        ok = IPC::recv(msg);
        if (!ok) {
            Scheduler::reschedule();
            arch::hlt();
        }
    }
    if (!ok)
        return;
    Message reply;
    reply.sender_id = self->id;
    reply.type = 78;
    reply.priority = 0;
    reply.data_size = 0;
    (void)IPC::send(msg.sender_id, reply);
}

void pb_client_entry() {
    auto *self = Scheduler::current_task();
    Message msg;
    msg.sender_id = self->id;
    msg.type = 46;
    msg.priority = 0;
    msg.data_size = 0;
    Message reply;
    const errors::IpcError err =
        IPC::send_sync_err(g_pb_server->id, msg, reply);
    if (err == errors::IPC_ERR_OK && reply.type == 78) {
        g_pb_client_ok = true;
        g_pb_client_type = reply.type;
    }
}

// Scratch daemon PID cell (mirrors the entry pid via set/get fns).
volatile uint64_t g_cell_pid = 0;
void scratch_set_pid(uint64_t pid) {
    g_cell_pid = pid;
}
uint64_t scratch_get_pid() {
    return g_cell_pid;
}

const char *vfsd_path() {
    for (uint64_t i = 0; i < daemon::MAX_DAEMONS; ++i) {
        const auto &entry = daemon::get_entry(i);
        if (entry.name != nullptr &&
            __builtin_strcmp(entry.name, "vfsd") == 0) {
            return entry.initrd_path;
        }
    }
    return nullptr;
}

uint64_t scratch_pid_of(const char *name) {
    for (uint64_t i = 0; i < daemon::MAX_DAEMONS; ++i) {
        const auto &entry = daemon::get_entry(i);
        if (entry.name != nullptr &&
            __builtin_strcmp(entry.name, name) == 0) {
            return entry.pid;
        }
    }
    return 0;
}

uint64_t scratch_restarts_of(const char *name) {
    for (uint64_t i = 0; i < daemon::MAX_DAEMONS; ++i) {
        const auto &entry = daemon::get_entry(i);
        if (entry.name != nullptr &&
            __builtin_strcmp(entry.name, name) == 0) {
            return entry.restart_count;
        }
    }
    return 0;
}

// Count dmesg records matching (subsystem, code) — deltas only, the
// ring is global and never cleared by isolation.
uint64_t count_dmesg(log::ErrorSubsystem subsys, uint64_t code) {
    uint64_t n = 0;
    log::DmesgService::instance().for_each(
        [&](const log::LogEntry &e) {
            if (e.subsystem == subsys && e.error_code == code)
                ++n;
        });
    return n;
}

// --- mk_reply_waiter_wake rendezvous ---
TaskControlBlock *g_wake_server = nullptr;
volatile bool g_wake_client_done = false;
volatile uint64_t g_wake_client_err = 0;
volatile bool g_wake_client_reply_wait = true;
volatile uint64_t g_wake_client_tag = 1;
volatile uint64_t g_wake_client_ticks = 0;

void wake_server_entry() {
    auto *self = Scheduler::current_task();
    (void)self;
    for (;;) {
        arch::sti();
        arch::hlt();
        arch::cli();
    }
}

void wake_client_entry() {
    auto *self = Scheduler::current_task();
    Message msg;
    msg.sender_id = self->id;
    msg.type = 42;
    msg.priority = 0;
    msg.data_size = 0;
    Message reply;
    const uint64_t t0 = arch::Timer::ticks();
    const errors::IpcError err =
        IPC::send_sync_err(g_wake_server->id, msg, reply);
    const uint64_t t1 = arch::Timer::ticks();
    g_wake_client_err = static_cast<uint64_t>(err);
    g_wake_client_reply_wait = self->reply_wait;
    g_wake_client_tag = self->reply_wait_dest_id;
    g_wake_client_ticks = (t1 >= t0) ? (t1 - t0) : 0;
    g_wake_client_done = true;
}

void wake_client_bounded_entry() {
    auto *self = Scheduler::current_task();
    Message msg;
    msg.sender_id = self->id;
    msg.type = 43;
    msg.priority = 0;
    msg.data_size = 0;
    Message reply;
    const errors::IpcError err = IPC::send_sync_bounded_err(
        g_wake_server->id, msg, reply, 1000);
    g_wake_client_err = static_cast<uint64_t>(err);
    g_wake_client_reply_wait = self->reply_wait;
    g_wake_client_tag = self->reply_wait_dest_id;
    g_wake_client_done = true;
}

// --- wake storm rendezvous (8 waiters + 8 watchers) ---
TaskControlBlock *g_storm_server = nullptr;
volatile uint64_t g_storm_wakes = 0;
volatile uint64_t g_storm_records = 0;

void storm_client_entry() {
    auto *self = Scheduler::current_task();
    Message msg;
    msg.sender_id = self->id;
    msg.type = 44;
    msg.priority = 0;
    msg.data_size = 0;
    Message reply;
    const errors::IpcError err =
        IPC::send_sync_err(g_storm_server->id, msg, reply);
    if (err == errors::IPC_ERR_PEER_DIED && !self->reply_wait &&
        self->reply_wait_dest_id == 0) {
        __atomic_fetch_add(&g_storm_wakes, 1, __ATOMIC_RELAXED);
    }
}

void storm_watcher_entry() {
    auto *self = Scheduler::current_task();
    if (!ipc::DeathNotify::watch(*self, g_storm_server->id, 0))
        return;
    for (uint64_t i = 0; i < 200000ULL; ++i) {
        ipc::DeathRecord rec;
        if (ipc::DeathNotify::recv(*self, rec) == 1) {
            if (rec.dead_id == g_storm_server->id &&
                rec.cause ==
                    static_cast<uint64_t>(ipc::DeathCause::Kill)) {
                __atomic_fetch_add(&g_storm_records, 1,
                                   __ATOMIC_RELAXED);
            }
            break;
        }
        Scheduler::reschedule();
        arch::hlt();
    }
}

} // namespace mk_death

// Runmode: kernel
// Testidea: M1 QE (INV-MK-6): a supervisor watching a prio-0 child under
// load observes exactly one fault death record within 2 ticks.
// Input: 2 prio-5 spinners (load) + prio-0 child; supervisor=harness
// watches child; fault-kill child via terminate_err(Fault).
// Expect: Exactly 1 record; dead_id==child, dead_gen==generation,
// cause==Fault(2), die_tick in [kill_tick, kill_tick+1]; second recv==0;
// recv within 2 ticks of the kill; watch slot released; tracker delta 0.
// Depends: DeathNotify watch/recv, publish_task_death funnel, Timer
JARVIS_TEST(mk_death_visible_bounded, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    auto *spinner1 = TaskControlBlock::create(test::forever_entry, 5, 10);
    JARVIS_ASSERT(spinner1 != nullptr);
    auto *spinner2 = TaskControlBlock::create(test::forever_entry, 5, 10);
    JARVIS_ASSERT(spinner2 != nullptr);
    auto *child = TaskControlBlock::create(test::forever_entry, 0, 10);
    JARVIS_ASSERT(child != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*child, SchedPolicy::FIXED));
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*spinner1);
        Scheduler::add_task(*spinner2);
        Scheduler::add_task(*child);
    }
    const uint64_t live0 = ipc::DeathNotify::live_count();
    JARVIS_ASSERT(ipc::DeathNotify::watch(*self, child->id, 0));
    JARVIS_ASSERT(ipc::DeathNotify::is_watching(*self, child->id));
    const uint64_t child_gen = child->generation;
    const uint64_t kill_tick = arch::Timer::ticks();
    JARVIS_ASSERT(Scheduler::terminate_err(
                      *child, 0, ipc::DeathCause::Fault) ==
                  errors::SCHED_ERR_OK);
    ipc::DeathRecord rec;
    bool got = false;
    uint64_t recv_tick = kill_tick;
    for (uint64_t i = 0; i < 200000ULL; ++i) {
        if (ipc::DeathNotify::recv(*self, rec) == 1) {
            got = true;
            recv_tick = arch::Timer::ticks();
            break;
        }
        Scheduler::reschedule();
        arch::hlt();
    }
    Logger::info("[MK-DEATH] visible got=%lu id=%lu gen=%lu cause=%lu "
                 "die_tick=%lu kill_tick=%lu recv_tick=%lu",
                 static_cast<uint64_t>(got), rec.dead_id, rec.dead_gen,
                 rec.cause, rec.die_tick, kill_tick, recv_tick);
    JARVIS_ASSERT(got);
    JARVIS_ASSERT_EQ(child->id, rec.dead_id);
    JARVIS_ASSERT_EQ(child_gen, static_cast<uint64_t>(rec.dead_gen));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(ipc::DeathCause::Fault),
                     rec.cause);
    JARVIS_ASSERT(rec.die_tick >= kill_tick);
    JARVIS_ASSERT(rec.die_tick <= kill_tick + 1);
    JARVIS_ASSERT(recv_tick - kill_tick <= 2);
    JARVIS_ASSERT_EQ(0, ipc::DeathNotify::recv(*self, rec));
    JARVIS_ASSERT(!ipc::DeathNotify::is_watching(*self, child->id));
    test::wait_for_termination_safe(child);
    test::terminate_and_drain2(spinner1, spinner2);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(live0, ipc::DeathNotify::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: M1 QE (#516): fault-killing a daemon task clears its PID cell
// synchronously (same tick) via the funnel, then the restart pass
// respawns it.
// Input: Scratch daemon qe_mk1 on vfsd's initrd path; ensure_running;
// fault-kill the daemon task.
// Expect: get_pid_fn()==0 and entry.pid==0 immediately (tick delta<=1);
// exactly one DAEMON+1 record; restart_stale_daemons() respawns (pid!=0,
// restart_count==1); teardown drains both tasks; tracker delta 0.
// Depends: daemon funnel clear, notify_death, restart_stale_daemons
JARVIS_TEST(fault_kill_clears_daemon_cell, "PRE: none | POST: none") {
    const char *path = mk_death::vfsd_path();
    JARVIS_ASSERT(path != nullptr);
    mk_death::g_cell_pid = 0;
    JARVIS_ASSERT(daemon::register_daemon("qe_mk1", path,
                                          mk_death::scratch_set_pid,
                                          mk_death::scratch_get_pid));
    daemon::ensure_running("qe_mk1");
    const uint64_t pid = mk_death::scratch_pid_of("qe_mk1");
    JARVIS_ASSERT(pid != 0);
    JARVIS_ASSERT_EQ(pid, mk_death::g_cell_pid);
    auto *daemon_task = Scheduler::find_task(pid);
    JARVIS_ASSERT(daemon_task != nullptr);
    const uint64_t d1_0 =
        mk_death::count_dmesg(log::ErrorSubsystem::DAEMON,
                              log::kDmesgBase_DAEMON + 1);
    const uint64_t kill_tick = arch::Timer::ticks();
    JARVIS_ASSERT(Scheduler::terminate_err(
                      *daemon_task, 0, ipc::DeathCause::Fault) ==
                  errors::SCHED_ERR_OK);
    const uint64_t clear_tick = arch::Timer::ticks();
    Logger::info("[MK-DEATH] cell pid=%lu cell_fn=%lu entry=%lu d1=%lu",
                 pid, mk_death::g_cell_pid,
                 mk_death::scratch_pid_of("qe_mk1"),
                 mk_death::count_dmesg(log::ErrorSubsystem::DAEMON,
                                       log::kDmesgBase_DAEMON + 1) - d1_0);
    JARVIS_ASSERT_EQ(0ULL, mk_death::g_cell_pid);
    JARVIS_ASSERT_EQ(0ULL, mk_death::scratch_pid_of("qe_mk1"));
    JARVIS_ASSERT(clear_tick - kill_tick <= 1);
    JARVIS_ASSERT_EQ(d1_0 + 1,
                     mk_death::count_dmesg(log::ErrorSubsystem::DAEMON,
                                           log::kDmesgBase_DAEMON + 1));
    daemon::restart_stale_daemons();
    const uint64_t pid2 = mk_death::scratch_pid_of("qe_mk1");
    JARVIS_ASSERT(pid2 != 0);
    JARVIS_ASSERT_EQ(1ULL, mk_death::scratch_restarts_of("qe_mk1"));
    auto *task2 = Scheduler::find_task(pid2);
    JARVIS_ASSERT(task2 != nullptr);
    Scheduler::terminate(*daemon_task, 0);
    test::terminate_if_live(task2);
    Scheduler::drain_zombie_list();
    mk_death::g_cell_pid = 0;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: M1 QE (#516): a stale cell (TERMINATED task, pre-M1 corpse
// shape) is healed by the next restart pass with exactly one respawn.
// Input: Scratch daemon qe_mk2 ensured; task marked TERMINATED directly
// (no funnel — simulates the pre-M1 corpse); restart_stale_daemons().
// Expect: DAEMON+12 delta==1; cell healed (entry.pid live, != stale);
// restart_count==1; second pass changes nothing (exactly once).
// Depends: daemon_task_live, DAEMON+12 heal, restart body
JARVIS_TEST(daemon_cell_stale_heal, "PRE: none | POST: none") {
    const char *path = mk_death::vfsd_path();
    JARVIS_ASSERT(path != nullptr);
    mk_death::g_cell_pid = 0;
    JARVIS_ASSERT(daemon::register_daemon("qe_mk2", path,
                                          mk_death::scratch_set_pid,
                                          mk_death::scratch_get_pid));
    daemon::ensure_running("qe_mk2");
    const uint64_t pid = mk_death::scratch_pid_of("qe_mk2");
    JARVIS_ASSERT(pid != 0);
    auto *daemon_task = Scheduler::find_task(pid);
    JARVIS_ASSERT(daemon_task != nullptr);
    // Pre-M1 corpse: TERMINATED without any funnel/cleanup.
    daemon_task->state = TaskState::TERMINATED;
    JARVIS_ASSERT_EQ(pid, mk_death::scratch_pid_of("qe_mk2"));
    const uint64_t d12_0 =
        mk_death::count_dmesg(log::ErrorSubsystem::DAEMON,
                              log::kDmesgBase_DAEMON + 12);
    daemon::restart_stale_daemons();
    const uint64_t pid2 = mk_death::scratch_pid_of("qe_mk2");
    Logger::info("[MK-DEATH] heal stale=%lu new=%lu restarts=%lu d12=%lu",
                 pid, pid2, mk_death::scratch_restarts_of("qe_mk2"),
                 mk_death::count_dmesg(log::ErrorSubsystem::DAEMON,
                                       log::kDmesgBase_DAEMON + 12) - d12_0);
    JARVIS_ASSERT_EQ(d12_0 + 1,
                     mk_death::count_dmesg(log::ErrorSubsystem::DAEMON,
                                           log::kDmesgBase_DAEMON + 12));
    JARVIS_ASSERT(pid2 != 0);
    JARVIS_ASSERT(pid2 != pid);
    JARVIS_ASSERT_EQ(1ULL, mk_death::scratch_restarts_of("qe_mk2"));
    auto *task2 = Scheduler::find_task(pid2);
    JARVIS_ASSERT(task2 != nullptr);
    JARVIS_ASSERT(task2->state != TaskState::TERMINATED);
    // Exactly once: a second pass with a live cell changes nothing.
    daemon::restart_stale_daemons();
    JARVIS_ASSERT_EQ(pid2, mk_death::scratch_pid_of("qe_mk2"));
    JARVIS_ASSERT_EQ(1ULL, mk_death::scratch_restarts_of("qe_mk2"));
    Scheduler::terminate(*daemon_task, 0);
    test::terminate_if_live(task2);
    Scheduler::drain_zombie_list();
    mk_death::g_cell_pid = 0;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: M1 QE (INV-MK-11): terminating a server wakes its plain
// send_sync reply waiter within a small bounded tick window with
// PEER_DIED and clean state.
// Input: Client C (prio 12) send_sync_err to silent server S (prio 11);
// harness kills S once C is BLOCKED.
// Expect: C returns PEER_DIED(10); reply_wait==false, dest tag cleared,
// RUNNING, not queued; wake within 8 ticks of the kill (typically 1-2;
// rv64 TCG needs up to ~5 — the waiter is READY throughout, a hang
// would never terminate the client); both drain.
// Depends: wake_reply_waiters_of, send_sync dest-gone leg
JARVIS_TEST(mk_reply_waiter_wake, "PRE: none | POST: none") {
    mk_death::g_wake_server = nullptr;
    mk_death::g_wake_client_done = false;
    mk_death::g_wake_client_err = 0;
    mk_death::g_wake_client_ticks = 0;
    auto *server = TaskControlBlock::create(mk_death::wake_server_entry,
                                            11, 10);
    JARVIS_ASSERT(server != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*server, SchedPolicy::FIXED));
    auto *client = TaskControlBlock::create(mk_death::wake_client_entry,
                                            12, 10);
    JARVIS_ASSERT(client != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*client, SchedPolicy::FIXED));
    mk_death::g_wake_server = server;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*server);
        Scheduler::add_task(*client);
    }
    Scheduler::reschedule();
    // Wait until the client is parked in the send_sync wait.
    bool blocked = false;
    for (uint64_t i = 0; i < 200000ULL; ++i) {
        if (client->state == TaskState::BLOCKED) {
            blocked = true;
            break;
        }
        Scheduler::reschedule();
        arch::hlt();
    }
    JARVIS_ASSERT(blocked);
    const uint64_t kill_tick = arch::Timer::ticks();
    JARVIS_ASSERT(Scheduler::terminate_err(*server, 0) ==
                  errors::SCHED_ERR_OK);
    test::wait_for_termination_safe(client);
    test::wait_for_termination_safe(server);
    Logger::info("[MK-DEATH] wake err=%lu tag=%lu ticks=%lu",
                 mk_death::g_wake_client_err, mk_death::g_wake_client_tag,
                 mk_death::g_wake_client_ticks);
    JARVIS_ASSERT(mk_death::g_wake_client_done);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::IPC_ERR_PEER_DIED),
                     mk_death::g_wake_client_err);
    JARVIS_ASSERT(!mk_death::g_wake_client_reply_wait);
    JARVIS_ASSERT_EQ(0ULL, mk_death::g_wake_client_tag);
    // Wake latency: the kill→wake→dispatch→consume chain.  Typically 1-2
    // ticks; riscv64 TCG needs up to ~5 (slow dispatch, not a kernel
    // wait — the waiter is READY throughout).  Bound at 8 with the
    // measured value logged; a hang would never terminate the client.
    JARVIS_ASSERT(mk_death::g_wake_client_ticks <= 8);
    (void)kill_tick;
    test::terminate_and_drain2(server, client);
    mk_death::g_wake_server = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: M1 QE (INV-MK-11): a bounded reply waiter whose server dies
// returns PEER_DIED, never TIMEOUT.
// Input: As mk_reply_waiter_wake with send_sync_bounded_err (bound 1000).
// Expect: PEER_DIED; reply_wait==false, tag cleared; both drain.
// Depends: send_sync_bounded dest-gone leg
JARVIS_TEST(mk_reply_waiter_wake_bounded, "PRE: none | POST: none") {
    mk_death::g_wake_server = nullptr;
    mk_death::g_wake_client_done = false;
    mk_death::g_wake_client_err = 0;
    auto *server = TaskControlBlock::create(mk_death::wake_server_entry,
                                            11, 10);
    JARVIS_ASSERT(server != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*server, SchedPolicy::FIXED));
    auto *client = TaskControlBlock::create(
        mk_death::wake_client_bounded_entry, 12, 10);
    JARVIS_ASSERT(client != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*client, SchedPolicy::FIXED));
    mk_death::g_wake_server = server;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*server);
        Scheduler::add_task(*client);
    }
    Scheduler::reschedule();
    bool blocked = false;
    for (uint64_t i = 0; i < 200000ULL; ++i) {
        if (client->state == TaskState::BLOCKED) {
            blocked = true;
            break;
        }
        Scheduler::reschedule();
        arch::hlt();
    }
    JARVIS_ASSERT(blocked);
    JARVIS_ASSERT(Scheduler::terminate_err(*server, 0) ==
                  errors::SCHED_ERR_OK);
    test::wait_for_termination_safe(client);
    test::wait_for_termination_safe(server);
    JARVIS_ASSERT(mk_death::g_wake_client_done);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::IPC_ERR_PEER_DIED),
                     mk_death::g_wake_client_err);
    JARVIS_ASSERT(!mk_death::g_wake_client_reply_wait);
    JARVIS_ASSERT_EQ(0ULL, mk_death::g_wake_client_tag);
    test::terminate_and_drain2(server, client);
    mk_death::g_wake_server = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: M1 QE: a send_sync against an already-dead server fails fast
// (no sleep past the next tick); a reply queued before the server's
// death still wins over PEER_DIED.
// Input: (a) Server S dead before the call; harness
// send_sync_bounded_err(S, bound 50) synchronously (send-refusal bail
// runs before any publish/block, so the harness never blocks).
// (b) Live rendezvous: server recvs, replies, exits; client Calls.
// Expect: (a) NO_REPLY fail-fast, tick delta<=1 (never blocked).  The
// send-ok-then-die race → PEER_DIED is covered by
// mk_reply_waiter_wake[_bounded] via real dispatch.  (b) OK with the
// reply type (dest-gone continue leg consumes the queued reply).
// Depends: send refusal fail-fast, reply-before-death consume
JARVIS_TEST(plain_send_sync_peer_died_bound, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    // Pair A: fail fast, no sleep.  Server created, added, and terminated
    // with no dispatch in between (never ran).
    auto *server_a = TaskControlBlock::create(test::forever_entry, 5, 10);
    JARVIS_ASSERT(server_a != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*server_a);
    }
    JARVIS_ASSERT(Scheduler::terminate_err(*server_a, 0) ==
                  errors::SCHED_ERR_OK);
    const uint64_t server_a_id = server_a->id;
    Message msg;
    msg.sender_id = self->id;
    msg.type = 45;
    msg.priority = 0;
    msg.data_size = 0;
    Message reply;
    const uint64_t t0 = arch::Timer::ticks();
    const errors::IpcError err_a = IPC::send_sync_bounded_err(
        server_a_id, msg, reply, 50);
    const uint64_t t1 = arch::Timer::ticks();
    Logger::info("[MK-DEATH] failfast err=%lu ticks=%lu",
                 static_cast<uint64_t>(err_a), t1 - t0);
    // Dead before send: the send itself refuses (NO_REPLY), synchronously
    // — never blocks.  (The send-ok-then-die race → PEER_DIED is covered
    // by mk_reply_waiter_wake[_bounded], driven via real dispatch.)
    JARVIS_ASSERT(err_a == errors::IPC_ERR_NO_REPLY);
    JARVIS_ASSERT(t1 - t0 <= 1);
    // Pair B: reply queued before the server's death still wins.  Server
    // recvs, replies, and exits; the client consumes the reply whether
    // or not it observes the death first (dest-gone continue leg).
    mk_death::g_pb_server = nullptr;
    mk_death::g_pb_client_ok = false;
    mk_death::g_pb_client_type = 0;
    auto *server_b = TaskControlBlock::create(mk_death::pb_server_entry,
                                              11, 10);
    JARVIS_ASSERT(server_b != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*server_b, SchedPolicy::FIXED));
    auto *client_b = TaskControlBlock::create(mk_death::pb_client_entry,
                                              12, 10);
    JARVIS_ASSERT(client_b != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*client_b, SchedPolicy::FIXED));
    mk_death::g_pb_server = server_b;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*server_b);
        Scheduler::add_task(*client_b);
    }
    Scheduler::reschedule();
    test::wait_for_termination_safe(client_b);
    test::wait_for_termination_safe(server_b);
    Logger::info("[MK-DEATH] reply-wins ok=%lu type=%lu",
                 static_cast<uint64_t>(mk_death::g_pb_client_ok),
                 mk_death::g_pb_client_type);
    JARVIS_ASSERT(mk_death::g_pb_client_ok);
    JARVIS_ASSERT_EQ(78ULL, mk_death::g_pb_client_type);
    Scheduler::terminate(*server_a, 0);
    test::terminate_and_drain2(server_b, client_b);
    Scheduler::drain_zombie_list();
    mk_death::g_pb_server = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: M1 QE (INV-MK-6/11 storm): one kill wakes 8 reply waiters
// (PEER_DIED each) and delivers 8 death records; no lock held across the
// funnel.
// Input: Silent server S + 8 clients send_sync_err(S) + 8 watchers
// DeathNotify::watch(S); harness kills S once all clients BLOCKED.
// Expect: g_storm_wakes==8, g_storm_records==8;
// spinlock_contract_check_clean; all 17 tasks drain; tracker delta 0.
// Depends: wake_reply_waiters_of fan-out, death record fan-out
JARVIS_TEST(mk_death_waiter_wake_storm, "PRE: none | POST: none") {
    mk_death::g_storm_server = nullptr;
    mk_death::g_storm_wakes = 0;
    mk_death::g_storm_records = 0;
    const uint64_t live0 = ipc::DeathNotify::live_count();
    auto *server = TaskControlBlock::create(mk_death::wake_server_entry,
                                            11, 10);
    JARVIS_ASSERT(server != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*server, SchedPolicy::FIXED));
    mk_death::g_storm_server = server;
    TaskControlBlock *clients[8];
    TaskControlBlock *watchers[8];
    for (int i = 0; i < 8; ++i) {
        clients[i] = TaskControlBlock::create(mk_death::storm_client_entry,
                                               12, 10);
        JARVIS_ASSERT(clients[i] != nullptr);
        JARVIS_ASSERT(Scheduler::set_sched_policy(*clients[i],
                                                  SchedPolicy::FIXED));
        watchers[i] = TaskControlBlock::create(
            mk_death::storm_watcher_entry, 10, 10);
        JARVIS_ASSERT(watchers[i] != nullptr);
        JARVIS_ASSERT(Scheduler::set_sched_policy(*watchers[i],
                                                  SchedPolicy::FIXED));
    }
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*server);
        for (int i = 0; i < 8; ++i) {
            Scheduler::add_task(*clients[i]);
            Scheduler::add_task(*watchers[i]);
        }
    }
    Scheduler::reschedule();
    // All clients must be parked in the wait before the kill.
    bool all_blocked = false;
    for (uint64_t i = 0; i < 400000ULL; ++i) {
        all_blocked = true;
        for (int k = 0; k < 8; ++k) {
            if (clients[k]->state != TaskState::BLOCKED) {
                all_blocked = false;
                break;
            }
        }
        if (all_blocked)
            break;
        Scheduler::reschedule();
        arch::hlt();
    }
    JARVIS_ASSERT(all_blocked);
    // All watchers must hold their watch before the kill.
    bool all_watching = false;
    for (uint64_t i = 0; i < 400000ULL; ++i) {
        all_watching = true;
        for (int k = 0; k < 8; ++k) {
            if (!ipc::DeathNotify::is_watching(*watchers[k],
                                               server->id)) {
                all_watching = false;
                break;
            }
        }
        if (all_watching)
            break;
        Scheduler::reschedule();
        arch::hlt();
    }
    JARVIS_ASSERT(all_watching);
    JARVIS_ASSERT(Scheduler::terminate_err(*server, 0) ==
                  errors::SCHED_ERR_OK);
    test::wait_for_termination_safe(server);
    for (int i = 0; i < 8; ++i) {
        test::wait_for_termination_safe(clients[i]);
        test::wait_for_termination_safe(watchers[i]);
    }
    Logger::info("[MK-DEATH] storm wakes=%lu records=%lu contract=%lu",
                 mk_death::g_storm_wakes, mk_death::g_storm_records,
                 sync::spinlock_held_by_current_count());
    JARVIS_ASSERT_EQ(8ULL, mk_death::g_storm_wakes);
    JARVIS_ASSERT_EQ(8ULL, mk_death::g_storm_records);
    JARVIS_ASSERT(sync::spinlock_contract_check_clean("storm"));
    for (int i = 0; i < 8; ++i)
        test::terminate_and_drain2(clients[i], watchers[i]);
    test::terminate_and_drain2(server, nullptr);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(live0, ipc::DeathNotify::live_count());
    mk_death::g_storm_server = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: M1 QE (#516): a teardown double-call after the funnel is a
// no-op — one record, one clear, restart count untouched.
// Input: Scratch daemon qe_mk3 ensured; supervisor watches; fault-kill;
// then daemon::notify_death(pid) again (teardown path).
// Expect: Still exactly 1 record (second recv==0); cell stays 0;
// restart_count unchanged; watch slot released.
// Depends: notify_death idempotency, funnel first-writer-wins
JARVIS_TEST(daemon_cell_no_double_clear, "PRE: none | POST: none") {
    const char *path = mk_death::vfsd_path();
    JARVIS_ASSERT(path != nullptr);
    mk_death::g_cell_pid = 0;
    JARVIS_ASSERT(daemon::register_daemon("qe_mk3", path,
                                          mk_death::scratch_set_pid,
                                          mk_death::scratch_get_pid));
    daemon::ensure_running("qe_mk3");
    const uint64_t pid = mk_death::scratch_pid_of("qe_mk3");
    JARVIS_ASSERT(pid != 0);
    auto *daemon_task = Scheduler::find_task(pid);
    JARVIS_ASSERT(daemon_task != nullptr);
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(ipc::DeathNotify::watch(*self, pid, 0));
    const uint64_t restarts0 = mk_death::scratch_restarts_of("qe_mk3");
    JARVIS_ASSERT(Scheduler::terminate_err(
                      *daemon_task, 0, ipc::DeathCause::Fault) ==
                  errors::SCHED_ERR_OK);
    ipc::DeathRecord rec;
    JARVIS_ASSERT_EQ(1, ipc::DeathNotify::recv(*self, rec));
    JARVIS_ASSERT_EQ(pid, rec.dead_id);
    JARVIS_ASSERT_EQ(0ULL, mk_death::scratch_pid_of("qe_mk3"));
    // Teardown double-call: must be a no-op.
    daemon::notify_death(pid, false);
    JARVIS_ASSERT_EQ(0, ipc::DeathNotify::recv(*self, rec));
    JARVIS_ASSERT_EQ(0ULL, mk_death::scratch_pid_of("qe_mk3"));
    JARVIS_ASSERT_EQ(restarts0, mk_death::scratch_restarts_of("qe_mk3"));
    Scheduler::terminate(*daemon_task, 0);
    Scheduler::drain_zombie_list();
    mk_death::g_cell_pid = 0;
    JARVIS_TEST_PASS();
}

void register_mk_death_tests() {
    JARVIS_REGISTER_TEST(mk_death_visible_bounded);
    JARVIS_REGISTER_TEST(fault_kill_clears_daemon_cell);
    JARVIS_REGISTER_TEST(daemon_cell_stale_heal);
    JARVIS_REGISTER_TEST(mk_reply_waiter_wake);
    JARVIS_REGISTER_TEST(mk_reply_waiter_wake_bounded);
    JARVIS_REGISTER_TEST(plain_send_sync_peer_died_bound);
    JARVIS_REGISTER_TEST(mk_death_waiter_wake_storm);
    JARVIS_REGISTER_TEST(daemon_cell_no_double_clear);
}
