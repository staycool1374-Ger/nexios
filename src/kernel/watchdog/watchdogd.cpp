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

/// @file watchdogd.cpp
/// @brief Watchdog daemon PID tracking + supervision grant (issue #277).

#include <kernel/watchdog/watchdogd.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/wdog.hpp>

namespace kernel {
namespace watchdogd {

static uint64_t g_watchdogd_pid = 0;

void set_watchdogd_pid(uint64_t pid) {
    g_watchdogd_pid = pid;
}

uint64_t get_watchdogd_pid() {
    return g_watchdogd_pid;
}

bool is_watchdogd_task() {
    auto *cur = Scheduler::current_task();
    return cur && cur->id == g_watchdogd_pid;
}

void grant_supervision(const uint64_t *supervised_pids, size_t count,
                       uint64_t *out_handles) {
    if (supervised_pids == nullptr || out_handles == nullptr)
        return;
    auto *daemon = Scheduler::find_task(g_watchdogd_pid);
    if (daemon == nullptr ||
        daemon->magic != TaskControlBlock::TCB_MAGIC)
        return;
    daemon->ensure_cspace();
    cap::CNode *cs = daemon->get_cspace();
    if (cs == nullptr)
        return;
    for (size_t i = 0; i < count; ++i) {
        out_handles[i] = static_cast<uint64_t>(-1);
        cap::WdogCap *cap = cap::WdogCap::create(supervised_pids[i]);
        if (cap == nullptr)
            continue;
        uint32_t gen = 0;
        int idx = cs->install(cap, cap::CapType::Wdog,
                              cap::CAP_RIGHT_WRITE, &gen);
        if (idx < 0) {
            cap->release(); // never installed — drop the creator reference
            continue;
        }
        out_handles[i] =
            cap::encode_user_handle(cs->cspace_id, idx, gen);
        cap->release();
    }
}

} // namespace watchdogd
} // namespace kernel
