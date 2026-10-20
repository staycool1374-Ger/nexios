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

/// @file asid.cpp
/// @brief Address-space identifier allocator (issue #352, aarch64 only).

#include <kernel/arch/aarch64/hal/asid.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/aarch64/hal/page_table_impl.hpp>

namespace arch {
namespace {

// Allocator state (create/cleanup-time mutation only — never on the
// timer-ISR or context-switch hot path; callers serialize via IrqGuard
// here because allocation can race task creation on SMP).
bool g_supported = false;
uint16_t g_next = 1;
uint64_t g_epoch = 0;
// Live-ID bitmap: 256 bits = 4 u64s, bit N set = ASID N live.  Claimed
// under IrqGuard (allocation never happens on the switch hot path).
uint64_t g_live[4] = {};

bool live_claim(uint16_t asid) {
    uint64_t *slot = &g_live[asid / 64];
    uint64_t bit = 1ULL << (asid % 64);
    uint64_t seen = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    while ((seen & bit) == 0) {
        if (__atomic_compare_exchange_n(slot, &seen, seen | bit, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

void live_clear(uint16_t asid) {
    __atomic_fetch_and(&g_live[asid / 64], ~(1ULL << (asid % 64)),
                       __ATOMIC_ACQ_REL);
}

} // namespace

void asid_init() {
    g_supported = true;
}

bool asid_supported() {
    return g_supported;
}

uint16_t asid_alloc() {
    if (!g_supported)
        return ASID_KERNEL;
    arch::IrqGuard guard{};
    // First pass: cursor to end.  Second pass: start to cursor.
    // A live ID is never reissued (bitmap-checked both passes).
    for (int pass = 0; pass < 2; ++pass) {
        uint16_t start = (pass == 0) ? g_next : 1;
        uint16_t limit = (pass == 0) ? (ASID_MAX + 1) : g_next;
        for (uint16_t id = start; id < limit; ++id) {
            if (live_claim(id)) {
                g_next = (id == ASID_MAX) ? 1 : (id + 1);
                return id;
            }
        }
    }
    // Exhausted: epoch bump + global purge, cursor reset to 1.  Live IDs
    // stay set, so reuse is still impossible and a fully-live space
    // answers 0 (caller falls back untagged).
    ++g_epoch;
    ArchPageTable::tlb_flush_all();
    g_next = 1;
    for (uint16_t id = 1; id <= ASID_MAX; ++id) {
        if (live_claim(id)) {
            g_next = (id == ASID_MAX) ? 1 : (id + 1);
            return id;
        }
    }
    return ASID_KERNEL; // all 255 live — caller falls back untagged
}

void asid_free(uint16_t asid) {
    if (asid == ASID_KERNEL)
        return;
    {
        arch::IrqGuard guard{};
        live_clear(asid);
    }
    // Purge-before-free: the freed address space's entries tagged with
    // this ASID survive in the TLB (tagged switches no longer flush).
    // A same-root full flush is the safe fallback until VA+ASID targeted
    // invalidation lands (step 4 of issue #352).
    if (g_supported)
        ArchPageTable::tlb_flush_all();
}

uint64_t asid_epoch() {
    return g_epoch;
}

} // namespace arch
