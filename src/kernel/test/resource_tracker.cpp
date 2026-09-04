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

/// @file resource_tracker.cpp
/// @brief Resource tracker implementation.

#include <kernel/test/resource_tracker.hpp>
#include <kernel/task/scheduler.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <constants.hpp>

namespace kernel::test {

#ifdef CONFIG_DEBUG

#if defined(CONFIG_ARCH_X86_64)
static inline uint64_t read_fp() {
    uint64_t v;
    asm volatile("mov %%rbp, %0" : "=r"(v));
    return v;
}
#elif defined(CONFIG_ARCH_AARCH64)
static inline uint64_t read_fp() {
    uint64_t v;
    asm volatile("mov %0, x29" : "=r"(v));
    return v;
}
#elif defined(CONFIG_ARCH_RISCV64)
static inline uint64_t read_fp() {
    uint64_t v;
    asm volatile("mv %0, s0" : "=r"(v));
    return v;
}
#endif

static void print_backtrace() {
    uint64_t fp = read_fp();
    Logger::warn("[RESOURCE]   Call stack:");
    for (int i = 0; i < 5 && fp; ++i) {
        if (fp < arch::HHDM_OFFSET)
            break;
        uint64_t ret = *reinterpret_cast<uint64_t *>(fp + 8);
        char buf[20];
        int p = 18;
        buf[18] = '\0';
        for (int j = 0; j < 16; ++j) {
            int nibble = static_cast<int>((ret >> (4 * j)) & 0xF);
            buf[--p] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
        }
        buf[0] = '0';
        buf[1] = 'x';
        Logger::warn("[RESOURCE]     #%d  %s", i, buf);
        fp = *reinterpret_cast<uint64_t *>(fp);
    }
}

struct ResField {
    const char *name;
    size_t ResourceCounters::*field;
};

static bool any_leak(const ResourceCounters &baseline,
                     const ResourceCounters &current) {
    for (size_t i = 0; i < MemPool::POOL_COUNT; ++i)
        if (current.mempool_used[i] > baseline.mempool_used[i])
            return true;
    if (current.pmm_pages_used > baseline.pmm_pages_used)
        return true;
    if (current.tasks > baseline.tasks)
        return true;
    if (current.bufpool_entries > baseline.bufpool_entries)
        return true;
    if (current.msg_queues > baseline.msg_queues)
        return true;
    if (current.notifies > baseline.notifies)
        return true;
    if (current.event_groups > baseline.event_groups)
        return true;
    if (current.drivers > baseline.drivers)
        return true;
    if (current.pipe_buffers > baseline.pipe_buffers)
        return true;
    if (current.vnodes > baseline.vnodes)
        return true;
    if (current.open_fds > baseline.open_fds)
        return true;
    if (current.cap_objects > baseline.cap_objects)
        return true;
    if (current.cap_slots > baseline.cap_slots)
        return true;
    if (current.death_watches > baseline.death_watches)
        return true;
    return false;
}

bool ResourceTracker::check(const ResourceCounters &baseline,
                            const char *test_name) {
    if (!any_leak(baseline, counters_))
        return true;

    // Dump the live task list to locate a leaked (un-freed) task that is
    // keeping the ready queue inconsistent across tests.
    {
        Logger::warn("[RESOURCE] live task list:");
        const auto &reg = kernel::Scheduler::all_tasks();
        for (auto *t = reg.first_ptr(); t; t = reg.next_ptr(t)) {
            if (!t || t->magic != kernel::TaskControlBlock::TCB_MAGIC)
                continue;
            Logger::warn("  id=%u state=%u prio=%u in_rq=%u rq_prio=%u name=%s",
                         (unsigned)t->id, (unsigned)t->state,
                         (unsigned)t->priority, (unsigned)t->in_ready_queue_,
                         (unsigned)t->rq_priority_, t->name);
        }
    }

    auto print_row = [&](const char *label, size_t base, size_t cur) {
        if (cur <= base)
            return;
        char buf[64];
        int p = 0;
        const char *s = label;
        while (*s && p < 19)
            buf[p++] = *s++;
        while (p < 20)
            buf[p++] = ' ';
        buf[p] = '\0';
        auto fmt = [](char *dst, size_t v) {
            char rev[20];
            int r = 0;
            if (v == 0) {
                rev[r++] = '0';
            } else {
                while (v > 0) {
                    rev[r++] = '0' + (v % 10);
                    v /= 10;
                }
            }
            int d = 0;
            while (r > 0)
                dst[d++] = rev[--r];
            dst[d] = '\0';
        };
        char base_s[20], cur_s[20];
        fmt(base_s, base);
        fmt(cur_s, cur);
        int base_w = 0;
        while (base_s[base_w])
            ++base_w;
        int cur_w = 0;
        while (cur_s[cur_w])
            ++cur_w;
        while (base_w < 6) {
            base_s[base_w++] = ' ';
        }
        base_s[base_w] = '\0';
        while (cur_w < 6) {
            cur_s[cur_w++] = ' ';
        }
        cur_s[cur_w] = '\0';
        Logger::warn("[RESOURCE] test=%s | %s | before=%s | after=%s <---",
                     test_name, buf, base_s, cur_s);
    };

    Logger::warn("[RESOURCE] %s leaked resources:", test_name);

    // Lossless lifecycle summary for leak analysis: how many tasks were created
    // vs cleanup()'d vs delete()'d since boot.  A gap (create > cleanup, or
    // cleanup > delete) proves tasks were torn down incompletely.  No asserts.
    {
        // Lossless lifecycle summary for leak analysis would be emitted here;
        // postponed (see ROADMAP §0.3.5.x.1) — instrumentation was inconclusive.
    }

    for (size_t i = 0; i < MemPool::POOL_COUNT; ++i) {
        char label[32];
        int pos = 0;
        auto append = [&](const char *s) {
            while (*s && pos < 30)
                label[pos++] = *s++;
        };
        append("MemPool[");
        size_t n = i;
        if (n == 0) {
            label[pos++] = '0';
        } else {
            char digits[8];
            int di = 0;
            while (n > 0) {
                digits[di++] = '0' + (n % 10);
                n /= 10;
            }
            while (di > 0)
                label[pos++] = digits[--di];
        }
        label[pos++] = ']';
        label[pos] = '\0';
        print_row(label, baseline.mempool_used[i], counters_.mempool_used[i]);
    }

    print_row("PMM pages", baseline.pmm_pages_used, counters_.pmm_pages_used);
    print_row("Tasks", baseline.tasks, counters_.tasks);
    print_row("BufPool entries", baseline.bufpool_entries,
              counters_.bufpool_entries);
    print_row("MessageQueues", baseline.msg_queues, counters_.msg_queues);
    print_row("Notify objects", baseline.notifies, counters_.notifies);
    print_row("EventGroups", baseline.event_groups, counters_.event_groups);
    print_row("Drivers", baseline.drivers, counters_.drivers);
    print_row("PipeBuffers", baseline.pipe_buffers, counters_.pipe_buffers);
    print_row("Vnodes", baseline.vnodes, counters_.vnodes);
    print_row("Open FDs", baseline.open_fds, counters_.open_fds);
    print_row("Cap objects", baseline.cap_objects, counters_.cap_objects);
    print_row("Cap slots", baseline.cap_slots, counters_.cap_slots);

    print_backtrace();
    return false;
}

#endif

} // namespace kernel::test
