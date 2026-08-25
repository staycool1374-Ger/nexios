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

/// @file iopb.cpp
/// @brief x86_64 per-task I/O permission bitmap management (issue #3).
/// A single global TSS carries an 8 KiB bitmap inside its segment.  Per-task
/// bitmaps live in a static pool (default-deny all-1s); on context switch the
/// TSS bitmap is loaded only when the owner changes.  SMP (per-CPU TSS) is
/// v0.4.4 scope (docs/specs/per-cpu-smp.md) — this module is UP-only.

#if defined(CONFIG_ARCH_X86_64)

#include <kernel/arch/hal/iopb.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/nexios_config.h>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/task/task.hpp>
#include <types.hpp>

namespace arch {

/// @brief I/O port bitmap pool (default-deny).  Static .bss — no dynamic
/// allocation on real-time paths.
static uint8_t g_iopb_pool[CONFIG_IOPB_MAX_TASKS][8192];

/// @brief Slot-claim bitmask (bit i set = slot i in use).
static uint32_t g_iopb_claimed = 0;

/// @brief Task whose bitmap is currently loaded in the TSS.  Pointer-compared
/// only, never dereferenced (owner may be a recycled TCB until release).
static const kernel::TaskControlBlock *g_iopb_owner = nullptr;

/// @brief Serializes pool/owner/TSS-bitmap state.  Lock order:
/// scheduler_lock_ -> g_iopb_lock (never the reverse).
static kernel::sync::SpinLock g_iopb_lock{};

bool iopb_claim(kernel::TaskControlBlock &t) {
    SpinLockGuard<kernel::sync::SpinLock> guard(g_iopb_lock);
    if (t.iopb_slot_ != kernel::TaskControlBlock::IOPB_SLOT_NONE &&
        (g_iopb_claimed & (1u << t.iopb_slot_)) != 0)
        return true; // already holds a live slot

    // Stale non-NONE slot (creation path forgot to initialize) or no slot:
    // claim a fresh pool entry below.
    for (uint32_t i = 0; i < CONFIG_IOPB_MAX_TASKS; ++i) {
        if (g_iopb_claimed & (1u << i))
            continue;
        g_iopb_claimed |= (1u << i);
        t.iopb_slot_ = static_cast<uint8_t>(i);
        for (size_t b = 0; b < sizeof(g_iopb_pool[i]); ++b)
            g_iopb_pool[i][b] = 0xFF; // default-deny
        return true;
    }
    return false; // pool exhausted — fail closed
}

void iopb_release(kernel::TaskControlBlock &t) {
    SpinLockGuard<kernel::sync::SpinLock> guard(g_iopb_lock);
    if (t.iopb_slot_ == kernel::TaskControlBlock::IOPB_SLOT_NONE)
        return;

    g_iopb_claimed &= ~(1u << t.iopb_slot_);
    if (g_iopb_owner == &t) {
        GDT::iopb_mask_all(); // no dangling permissive bitmap
        g_iopb_owner = nullptr;
    }
    t.iopb_slot_ = kernel::TaskControlBlock::IOPB_SLOT_NONE;
}

bool iopb_grant_range(kernel::TaskControlBlock &t, uint16_t start,
                      uint32_t count) {
    SpinLockGuard<kernel::sync::SpinLock> guard(g_iopb_lock);
    if (t.iopb_slot_ == kernel::TaskControlBlock::IOPB_SLOT_NONE ||
        (g_iopb_claimed & (1u << t.iopb_slot_)) == 0)
        return false;

    uint8_t *slot = g_iopb_pool[t.iopb_slot_];
    const uint32_t end =
        static_cast<uint32_t>(start) + count; // caller validated <= 65536
    for (uint32_t p = static_cast<uint32_t>(start); p < end; ++p) {
        slot[p / 8] &= static_cast<uint8_t>(~(1u << (p % 8)));
    }

    if (g_iopb_owner == &t)
        GDT::iopb_load(slot); // apply immediately if currently loaded
    return true;
}

void iopb_switch_to(const kernel::TaskControlBlock &next) {
    SpinLockGuard<kernel::sync::SpinLock> guard(g_iopb_lock);
    if (!next.is_user_)
        return; // CPL0 ignores the bitmap; keep the loaded owner valid

    const uint8_t slot = next.iopb_slot_;
    // Defense-in-depth: a slot is loadable only if it is actually claimed.
    // A stale non-NONE value (e.g. a task-creation path that forgot to
    // initialize the field) must never load an unclaimed, all-zeros slot
    // into the TSS (that would make every port allowed).
    const bool valid_slot =
        (slot != kernel::TaskControlBlock::IOPB_SLOT_NONE) &&
        (g_iopb_claimed & (1u << slot)) != 0;
    if (valid_slot) {
        if (g_iopb_owner != &next) {
            GDT::iopb_load(g_iopb_pool[slot]);
            g_iopb_owner = &next;
        }
    } else if (g_iopb_owner != nullptr) {
        GDT::iopb_mask_all(); // user task with no grant: default-deny
        g_iopb_owner = nullptr;
    }
}

void iopb_snapshot_reset() {
    SpinLockGuard<kernel::sync::SpinLock> guard(g_iopb_lock);
    g_iopb_claimed = 0;
    g_iopb_owner = nullptr;
    GDT::iopb_mask_all();
}

const void *iopb_loaded_owner() {
    SpinLockGuard<kernel::sync::SpinLock> guard(g_iopb_lock);
    return g_iopb_owner;
}

bool iopb_port_allowed(const kernel::TaskControlBlock &t, uint16_t port) {
    SpinLockGuard<kernel::sync::SpinLock> guard(g_iopb_lock);
    if (t.iopb_slot_ == kernel::TaskControlBlock::IOPB_SLOT_NONE ||
        (g_iopb_claimed & (1u << t.iopb_slot_)) == 0)
        return false;
    const uint8_t *slot = g_iopb_pool[t.iopb_slot_];
    return (slot[port / 8] & (1u << (port % 8))) == 0;
}

} // namespace arch

#endif // CONFIG_ARCH_X86_64