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

/// @file pcid.cpp
/// @brief Process-context identifier allocator (issue #156, x86_64 only).

#include <kernel/arch/x86_64/hal/pcid.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/x86_64/hal/cpuid_impl.hpp>
#include <kernel/arch/x86_64/hal/io_impl.hpp>
#include <kernel/arch/x86_64/hal/page_table_impl.hpp>

namespace arch {
namespace {

// Allocator state (create/cleanup-time mutation only — never on the
// timer-ISR or context-switch hot path; callers serialize via the
// scheduler paths that already hold scheduler_lock_).
bool g_supported = false;
uint16_t g_next = 1;
uint64_t g_epoch = 0;
// Serialization note (audit S3): the live-ID bitmap is the safety
// state and is claimed atomically (CAS — never-reissue holds under
// concurrency).  g_next is a placement hint (a stale read only wastes
// a scan pass; the claim still decides).  g_epoch is diagnostic plus
// the rollover witness (monotonic by construction: only ever
// incremented under the same task-context paths as allocation, and
// tests assert relative deltas, never absolute values).
// Live-ID bitmap: 4096 bits = 64 u64s, bit N set = PCID N live.
uint64_t g_live[64] = {};

// Atomically claim a free ID: returns true iff this caller won it.
// CAS loop (concurrent creates on two CPUs can never share an ID).
bool live_claim(uint16_t pcid) {
    uint64_t *slot = &g_live[pcid / 64];
    uint64_t bit = 1ULL << (pcid % 64);
    uint64_t seen = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    while ((seen & bit) == 0) {
        if (__atomic_compare_exchange_n(slot, &seen, seen | bit, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

void live_clear(uint16_t pcid) {
    __atomic_fetch_and(&g_live[pcid / 64], ~(1ULL << (pcid % 64)),
                       __ATOMIC_ACQ_REL);
}

} // namespace

void pcid_init() {
    g_supported = has_pcid();
}

bool pcid_supported() {
    return g_supported;
}

uint16_t pcid_alloc() {
    if (!g_supported)
        return PCID_KERNEL;
    // First pass: cursor to end.  Second pass: start to cursor.
    // A live ID is never reissued (bitmap-checked both passes).
    for (int pass = 0; pass < 2; ++pass) {
        uint16_t start = (pass == 0) ? g_next : 1;
        uint16_t limit = (pass == 0) ? (PCID_MAX + 1) : g_next;
        for (uint16_t id = start; id < limit; ++id) {
            if (live_claim(id)) {
                g_next = (id == PCID_MAX) ? 1 : (id + 1);
                return id;
            }
        }
    }
    // Exhausted: epoch bump + global purge (INVPCID all-context when
    // available, else untagged CR3 reload), cursor reset to 1.  Live IDs
    // stay set, so reuse is still impossible and a fully-live space
    // answers 0 (caller falls back untagged).
    ++g_epoch;
    tlb_purge_all();
    g_next = 1;
    for (uint16_t id = 1; id <= PCID_MAX; ++id) {
        if (live_claim(id)) {
            g_next = (id == PCID_MAX) ? 1 : (id + 1);
            return id;
        }
    }
    return PCID_KERNEL; // all 4095 live — caller falls back untagged
}

void pcid_free(uint16_t pcid) {
    if (pcid == PCID_KERNEL)
        return;
    live_clear(pcid);
    // Issue #156 (audit S1): a freed PCID is immediately reusable, but the
    // freed address space's entries tagged with it survive in the TLB
    // (tagged switches no longer flush, and VMM::free_user_pages leaves
    // flushing to the caller). Purge them with one same-root CR3 reload
    // (NoFlush bit clear, so all PCID-tagged entries are invalidated
    // locally); the old table is destroyed right after, so nothing can
    // re-cache this ID in the meantime.
    //
    // Coherency bound (why a local purge suffices for now): user address
    // spaces — the only PCID holders — execute on CPU0 exclusively
    // (shared-TSS pin), so their tagged entries can only ever live in
    // CPU0's TLB.  User-task cleanup (self/parent-waitpid/deadline-BSP/
    // reboot-BSP) likewise executes on CPU0 in every current path — no
    // AP-context user-teardown producer exists — so this purge always
    // lands where the stale entries are.  General cross-CPU invalidation
    // (should APs ever terminate user tasks) is owned by the lazy
    // shootdown protocol (#158), which must cover it by design.
    if (g_supported)
        tlb_purge_context(pcid);
}

uint64_t pcid_epoch() {
    return g_epoch;
}

void pcid_test_force() {
    g_supported = true;
}

void pcid_test_reset() {
    g_supported = has_pcid();
}

} // namespace arch
