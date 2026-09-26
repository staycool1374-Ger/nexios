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

/// @file debug_stop.cpp
/// @brief Debugger stop-event routing + kernel breakpoint shadows (issue
///        #226, spec docs/specs/debugd.md §4–§5). Mirrors the DeathNotify /
///        PagerRegistry discipline: static bounded tables, claim-under-lock,
///        poke debuggers OUTSIDE the lock (documented order scheduler_lock_
///        -> registry locks). Fault handlers call debug_route_fault from ISR
///        context; the deferred switch is armed via the same publish path
///        as switch_away_from_terminating (parked tasks never run again
///        until debugger_resume re-queues them READY).

#include <kernel/debug/debug_stop.hpp>
#include <kernel/debug/debug_bind.hpp>
#include <kernel/debug/debug_regs.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/nexios_config.h>

namespace kernel::debug {

namespace {

// Linux errno numerics are owned by the syscall layer; this module only
// reports success/failure booleans.

// ---- Stop-event ring ----------------------------------------------------
// CONFIG_DEBUG_MAX_STOPS normal slots + death bypass (death evicts the
// oldest non-death event; a ring of pure deaths evicts the oldest death
// and still counts it — bounded memory, never unbounded queues).

struct StopSlot {
    StopEvent event{};
    uint64_t seq = 0;
    bool used = false;
};

constexpr size_t kStopCap = CONFIG_DEBUG_MAX_STOPS;

/// @brief Owned-snapshot capacity: at most one entry per binding slot.
constexpr size_t kDebugOwnedCap = 64;

StopSlot g_stops[kStopCap]{};
uint64_t g_stop_seq = 1;
uint64_t g_stop_dropped = 0;
sync::SpinLock g_stop_lock{};

// ---- Breakpoint shadow table --------------------------------------------

struct BpShadow {
    uint64_t target_id = 0;
    uint64_t target_gen = 0;
    uint64_t va = 0;
    uint8_t orig[4] = {};
    uint8_t len = 0;
    uint64_t sie_saved = 0; // RISC-V emu-step: SSTATUS.SIE at plant time
    bool temp = false; // RISC-V emulated-step breakpoint (not user-visible)
    bool used = false;
};

constexpr size_t kBpCap = CONFIG_DEBUG_MAX_BREAKS;

BpShadow g_bp[kBpCap]{};
sync::SpinLock g_bp_lock{};

// ---- Per-arch breakpoint instructions (spec §4 single source of truth) --

#if defined(CONFIG_ARCH_X86_64)
constexpr uint8_t kBpInsn[] = {0xCC};
#elif defined(CONFIG_ARCH_AARCH64)
constexpr uint8_t kBpInsn[] = {0x00, 0x00, 0x20, 0xD4}; // brk #0, LE
#elif defined(CONFIG_ARCH_RISCV64)
constexpr uint8_t kBpInsn16[] = {0x02, 0x90}; // c.ebreak
constexpr uint8_t kBpInsn32[] = {0x73, 0x00, 0x10, 0x00}; // ebreak, LE
#else
constexpr uint8_t kBpInsn[] = {0xCC};
#endif

/// @brief Breakpoint instruction length for @p va (RISC-V honors RVC).
// Release builds stub out every caller (debug-only facility, spec §10/N4)
// — silence -Werror=unused-function without touching the debug paths.
[[maybe_unused]] size_t
bp_insn_len(uint64_t va, const TaskControlBlock &target) noexcept {
#if defined(CONFIG_ARCH_RISCV64)
    (void)va;
    (void)target;
    // Compressed layouts need the target's actual halfword; default to the
    // 4-byte form — bp_write_at refines per-VA below.
    return 4;
#else
    (void)va;
    (void)target;
    return sizeof(kBpInsn);
#endif
}

/// @brief Write @p len bytes at @p va through the target's tables + the
///        per-arch coherence the #186 pitfalls demand (spec §3: I-cache
///        coherence is the kernel's job; TCG hides a missing flush).
///        NOTE: virt_to_phys_in_pml4 already includes the page offset in
///        its result (4K and 2MB paths alike) — use it directly, never add
///        page_off again (double-add misplaces the access on unaligned
///        VAs, issue #226).
[[maybe_unused]] bool bp_write_at(TaskControlBlock &target, uint64_t va,
                                    const uint8_t *bytes,
                                    size_t len) noexcept {
    if (len == 0 || len > 4 || target.page_table_ == 0)
        return false;
    uint64_t page_off = va & 0xFFFULL;
    if (page_off + len > 0x1000ULL)
        return false; // never straddle a page (all bp insns fit)
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, target.page_table_);
    if (phys == 0)
        return false;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *dst = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
    for (size_t i = 0; i < len; ++i)
        dst[i] = bytes[i];
#if defined(CONFIG_ARCH_RISCV64)
    asm volatile(".option push\n.option arch, +zifencei\nfence.i\n.option pop" ::
                     : "memory");
    arch::ArchPageTable::tlb_flush(va);
#elif defined(CONFIG_ARCH_AARCH64)
    asm volatile("ic ialluis\n dsb ish\n isb" ::: "memory");
    arch::ArchPageTable::tlb_flush(va);
#else
    // x86_64 is I-cache coherent; the VMM already flushes TLB on remaps,
    // but a present->present insn patch needs an explicit invalidate.
    arch::ArchPageTable::tlb_flush(va);
#endif
    return true;
}

/// @brief Read @p len bytes at @p va through the target's tables.
[[maybe_unused]] bool bp_read_at(TaskControlBlock &target, uint64_t va,
                                   uint8_t *out, size_t len) noexcept {
    if (len == 0 || len > 4 || target.page_table_ == 0)
        return false;
    uint64_t page_off = va & 0xFFFULL;
    if (page_off + len > 0x1000ULL)
        return false;
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, target.page_table_);
    if (phys == 0)
        return false;
    // Walk result is the exact phys (see bp_write_at note) — no page_off.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto *src =
        reinterpret_cast<const uint8_t *>(arch::HHDM_OFFSET + phys);
    for (size_t i = 0; i < len; ++i)
        out[i] = src[i];
    return true;
}

/// @brief Enqueue helper: caller holds no locks; takes g_stop_lock.
///        Death kind evicts oldest non-death (reserved-slot guarantee).
[[maybe_unused]] void stop_enqueue(const StopEvent &event) noexcept {
    SpinLockGuard<sync::SpinLock> guard(g_stop_lock);
    StopSlot *slot = nullptr;
    for (size_t i = 0; i < kStopCap; ++i) {
        if (!g_stops[i].used) {
            slot = &g_stops[i];
            break;
        }
    }
    if (slot == nullptr) {
        // Full: death evicts the oldest non-death; anything else drops
        // the oldest event outright. Both count as drops (bounded memory).
        size_t victim = 0;
        bool found = false;
        for (size_t i = 0; i < kStopCap; ++i) {
            if (event.kind ==
                    static_cast<uint64_t>(StopKind::DEATH) &&
                g_stops[i].event.kind ==
                    static_cast<uint64_t>(StopKind::DEATH))
                continue;
            if (!found || g_stops[i].seq < g_stops[victim].seq) {
                victim = i;
                found = true;
            }
        }
        if (!found)
            victim = 0; // pure-death ring: evict oldest death anyway
        if (!found) {
            uint64_t oldest = g_stops[0].seq;
            for (size_t i = 1; i < kStopCap; ++i) {
                if (g_stops[i].seq < oldest) {
                    oldest = g_stops[i].seq;
                    victim = i;
                }
            }
        }
        slot = &g_stops[victim];
        ++g_stop_dropped;
    }
    slot->event = event;
    slot->seq = g_stop_seq++;
    slot->used = true;
}

/// @brief Poke the debugger's Notify outside all locks (DeathNotify
///        precedent: capture id+gen under lock, re-resolve at poke time).
[[maybe_unused]] void poke_debugger(uint64_t debugger_id) noexcept {
    if (debugger_id == 0)
        return;
    TaskControlBlock *dbg = Scheduler::find_task(debugger_id);
    if (dbg == nullptr || !TaskControlBlock::is_valid(dbg) ||
        dbg->state == TaskState::TERMINATED ||
        dbg->state == TaskState::REAPED)
        return;
    dbg->notify.notify(DEBUG_STOP_PULSE);
}

/// @brief Temp-breakpoint hit bookkeeping (emulated step on RISC-V and
///        AArch64): remove the temp shadow and report whether @p pc was a
///        temp hit. The shadow slot is freed under the lock, but the temp
///        breakpoint bytes stay patched in the target until the caller
///        restores the snapshotted bytes below (outside the lock — the
///        restore walks tables and must never run under g_bp_lock).
/// @return true when @p pc matched a temp entry (removed, SIE bit and
///         orig-byte snapshot out).
[[maybe_unused]] bool debug_bp_consume_temp(TaskControlBlock &target,
                                               uint64_t pc,
                                               uint64_t &sie_out,
                                               uint64_t &va_out,
                                               uint8_t *orig_out,
                                               uint8_t &len_out) noexcept {
    sie_out = 0;
    va_out = 0;
    len_out = 0;
    SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
    for (size_t i = 0; i < kBpCap; ++i) {
        if (g_bp[i].used && g_bp[i].temp &&
            g_bp[i].target_id == target.id &&
            g_bp[i].target_gen == target.generation && g_bp[i].va == pc) {
            sie_out = g_bp[i].sie_saved;
            va_out = g_bp[i].va;
            len_out = g_bp[i].len;
            for (size_t b = 0; b < g_bp[i].len; ++b)
                orig_out[b] = g_bp[i].orig[b];
            g_bp[i].used = false;
            return true;
        }
    }
    return false;
}

} // namespace

/// @brief Forward: STEP-completion trailer (defined below).
void debug_step_complete(TaskControlBlock &target) noexcept;

#if defined(CONFIG_ARCH_RISCV64) || defined(CONFIG_ARCH_AARCH64)
/// @brief Forward: emulated step plant (defined below).
bool emu_step_plant(TaskControlBlock &target) noexcept;
#endif

bool debug_route_fault(TaskControlBlock &target, uint64_t kind, uint64_t num,
                       uint64_t addr) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)target;
    (void)kind;
    (void)num;
    (void)addr;
    return false;
#else
    if (kind != static_cast<uint64_t>(StopKind::BREAKPOINT) &&
        kind != static_cast<uint64_t>(StopKind::STEP) &&
        kind != static_cast<uint64_t>(StopKind::FAULT))
        return false;
    if (!target.is_user_)
        return false;
    uint64_t debugger_id =
        __atomic_load_n(&target.debugger_id, __ATOMIC_ACQUIRE);
    if (debugger_id == 0)
        return false; // unattached: existing disposition unchanged
    // Temp-breakpoint hits (RISC-V emulated step) arrive as BREAKPOINT
    // traps: consume the temp shadow, restore the saved SIE bit, and
    // convert to a STEP stop (with step-over re-arm when pending).
    // Native STEP traps skip straight to completion.
    if (kind == static_cast<uint64_t>(StopKind::BREAKPOINT)) {
        uint64_t sie_saved = 0;
        uint64_t tmp_va = 0;
        uint8_t tmp_orig[4] = {};
        uint8_t tmp_len = 0;
        if (debug_bp_consume_temp(target, addr, sie_saved, tmp_va, tmp_orig,
                                  tmp_len)) {
            // Restore the consumed temp breakpoint bytes (outside the bp
            // lock): without this the emulated-step trap instruction stays
            // patched at the new pc and re-traps spuriously on resume.
            // The slot is already freed, so a re-entrant fault cannot
            // double-consume it; fail-open when unmapped (dying target —
            // its tables are freed on the death path anyway).
            if (tmp_len != 0)
                (void)bp_write_at(target, tmp_va, tmp_orig, tmp_len);
#if defined(CONFIG_ARCH_RISCV64)
            // SIE restore is RISC-V-only: frame[32] is OFF_SSTATUS there.
            // On AArch64 frame[32] is ELR — writing the SIE bit there
            // would corrupt PC bit 1 (issue #226 follow-up to the audit
            // fix; AArch64 temps need no state restore).
            uint64_t *frame = debug_frame_slot(target);
            if (frame != nullptr && debug_frame_is_user(frame))
                frame[32] = (frame[32] & ~(1ULL << 1)) |
                            ((sie_saved & 1ULL) << 1); // OFF_SSTATUS SIE
#endif
            kind = static_cast<uint64_t>(StopKind::STEP);
            debug_step_complete(target);
        }
    } else if (kind == static_cast<uint64_t>(StopKind::STEP)) {
        debug_step_complete(target);
    }
    // One debugger per target, one stop per trap: a target that is already
    // parked for the debugger has stable frames — a second trap while
    // parked means the resume path re-entered unexpectedly; still report
    // it (never silently swallow a trap), the debugger owns the policy.
    // Park under the scheduler lock (re-checks the binding: detach could
    // have cleared it between the load above and the park below), then
    // enqueue (nested scheduler_lock_ -> g_stop_lock_, DeathNotify order).
    if (!Scheduler::debug_park_stop(target, kind, addr))
        return false;
    {
        StopEvent event{};
        event.target_id = target.id;
        event.target_gen = target.generation;
        event.kind = kind;
        event.fault_num = num;
        event.fault_addr = addr;
        event.snap_state = static_cast<uint64_t>(TaskState::BLOCKED);
        event.snap_prio = target.priority;
        event.snap_budget = 0; // Phase 2: budget export is zero (see §9)
        stop_enqueue(event);
    }
    poke_debugger(debugger_id);
    // Arm the deferred switch away (same publish path as terminating tasks:
    // a BLOCKED+dequeued task is invisible to next_task() either way).
    Scheduler::switch_away_from_terminating(target);
    return true;
#endif
}

bool debug_poll_event(uint64_t debugger_id, StopEvent &out) noexcept {
    // Owned-target snapshot first (bind released on return) — the scan
    // below then holds only the queue lock (bind -> stop order kept).
    // Ownership ignores liveness: death events stay pollable after the
    // target died (debug_resolve would ESRCH).
    uint64_t owned_ids[kDebugOwnedCap];
    uint32_t owned_gens[kDebugOwnedCap];
    size_t n_owned =
        debug_collect_owned(debugger_id, owned_ids, owned_gens,
                            kDebugOwnedCap);
    if (n_owned == 0)
        return false;
    SpinLockGuard<sync::SpinLock> guard(g_stop_lock);
    StopSlot *best = nullptr;
    for (size_t i = 0; i < kStopCap; ++i) {
        if (!g_stops[i].used)
            continue;
        bool mine = false;
        for (size_t j = 0; j < n_owned; ++j) {
            if (g_stops[i].event.target_id == owned_ids[j] &&
                static_cast<uint32_t>(g_stops[i].event.target_gen) ==
                    owned_gens[j]) {
                mine = true;
                break;
            }
        }
        if (!mine)
            continue;
        if (best == nullptr || g_stops[i].seq < best->seq)
            best = &g_stops[i];
    }
    if (best == nullptr)
        return false;
    out = best->event;
    best->used = false;
    best->event = StopEvent{};
    return true;
}

uint64_t debug_dropped_count() noexcept {
    return __atomic_load_n(&g_stop_dropped, __ATOMIC_ACQUIRE);
}

bool debug_bp_insert(TaskControlBlock &target, uint64_t va) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)target;
    (void)va;
    return false;
#else
    if (__atomic_load_n(&target.debugger_id, __ATOMIC_ACQUIRE) == 0)
        return false;
    SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
    for (size_t i = 0; i < kBpCap; ++i) {
        if (g_bp[i].used && g_bp[i].target_id == target.id &&
            g_bp[i].target_gen == target.generation && g_bp[i].va == va &&
            !g_bp[i].temp)
            return true; // idempotent re-insert
    }
    size_t insn_len = bp_insn_len(va, target);
#if defined(CONFIG_ARCH_RISCV64)
    // Honor RVC: the halfword at VA decides 2- vs 4-byte ebreak.
    uint8_t probe[2] = {};
    if (!bp_read_at(target, va, probe, 2))
        return false;
    insn_len = ((probe[0] & 0x3U) != 0x3U) ? 2 : 4;
    const uint8_t *insn = (insn_len == 2) ? kBpInsn16 : kBpInsn32;
#else
    const uint8_t *insn = kBpInsn;
#endif
    for (size_t i = 0; i < kBpCap; ++i) {
        if (g_bp[i].used)
            continue;
        uint8_t orig[4] = {};
        if (!bp_read_at(target, va, orig, insn_len))
            return false; // unmapped: fail closed, slot stays free
        if (!bp_write_at(target, va, insn, insn_len))
            return false;
        g_bp[i].target_id = target.id;
        g_bp[i].target_gen = target.generation;
        g_bp[i].va = va;
        for (size_t b = 0; b < insn_len; ++b)
            g_bp[i].orig[b] = orig[b];
        g_bp[i].len = static_cast<uint8_t>(insn_len);
        g_bp[i].temp = false;
        g_bp[i].used = true;
        return true;
    }
    return false; // table full: fail closed
#endif
}

bool debug_bp_clear(TaskControlBlock &target, uint64_t va) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)target;
    (void)va;
    return false;
#else
    uint8_t orig[4] = {};
    uint8_t len = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
        for (size_t i = 0; i < kBpCap; ++i) {
            if (g_bp[i].used && !g_bp[i].temp &&
                g_bp[i].target_id == target.id &&
                g_bp[i].target_gen == target.generation && g_bp[i].va == va) {
                // Collect under the lock; the byte restore (with its
                // coherence flush, which takes no locks) runs after.
                len = g_bp[i].len;
                for (size_t b = 0; b < len; ++b)
                    orig[b] = g_bp[i].orig[b];
                g_bp[i].used = false;
                break;
            }
        }
    }
    if (len == 0)
        return false;
    return bp_write_at(target, va, orig, len);
#endif
}

void debug_bp_restore_all(TaskControlBlock &target) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)target;
#else
    struct Restore {
        uint64_t va;
        uint8_t orig[4];
        uint8_t len;
    };
    Restore pending[kBpCap];
    size_t n = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
        for (size_t i = 0; i < kBpCap; ++i) {
            if (g_bp[i].used && g_bp[i].target_id == target.id &&
                g_bp[i].target_gen == target.generation) {
                pending[n].va = g_bp[i].va;
                pending[n].len = g_bp[i].len;
                for (size_t b = 0; b < g_bp[i].len; ++b)
                    pending[n].orig[b] = g_bp[i].orig[b];
                ++n;
                g_bp[i].used = false;
            }
        }
    }
    // Byte restore outside the lock (tables may fault-walk; no locks held).
    for (size_t i = 0; i < n; ++i)
        bp_write_at(target, pending[i].va, pending[i].orig, pending[i].len);
#endif
}

uint64_t debug_bp_match(TaskControlBlock &target, uint64_t pc) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)target;
    (void)pc;
    return 0;
#else
    SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
    for (size_t i = 0; i < kBpCap; ++i) {
        if (g_bp[i].used && g_bp[i].target_id == target.id &&
            g_bp[i].target_gen == target.generation && g_bp[i].va == pc)
            return pc;
    }
    return 0;
#endif
}

bool debug_continue(TaskControlBlock &target) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)target;
    return false;
#else
    if (!target.debug_parked)
        return target.state == TaskState::RUNNING; // already running: no-op
    uint64_t kind = __atomic_load_n(&target.debug_stop_kind, __ATOMIC_ACQUIRE);
    uint64_t va = __atomic_load_n(&target.debug_stop_va, __ATOMIC_ACQUIRE);
    if (kind == static_cast<uint64_t>(StopKind::BREAKPOINT) && va != 0 &&
        debug_bp_match(target, va) != 0) {
        // Stopped on a breakpoint: single-step-over with re-arm (standard).
        // Restore orig bytes, arm one step, resume; STEP completion
        // re-inserts at rearm_va.
        uint8_t orig[4] = {};
        uint8_t len = 0;
        {
            SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
            for (size_t i = 0; i < kBpCap; ++i) {
                if (g_bp[i].used && !g_bp[i].temp &&
                    g_bp[i].target_id == target.id &&
                    g_bp[i].target_gen == target.generation &&
                    g_bp[i].va == va) {
                    len = g_bp[i].len;
                    for (size_t b = 0; b < len; ++b)
                        orig[b] = g_bp[i].orig[b];
                    // Drop the shadow: STEP completion re-inserts fresh
                    // (an idempotent re-insert would skip the insn write).
                    g_bp[i].used = false;
                    break;
                }
            }
        }
        if (len == 0)
            return false;
        if (!bp_write_at(target, va, orig, len))
            return false;
#if defined(CONFIG_ARCH_RISCV64) || defined(CONFIG_ARCH_AARCH64)
        // Emulated step-over: plant the temp breakpoint (native arm
        // always fails off-x86); STEP completion re-inserts at rearm.
        if (!emu_step_plant(target))
            return false;
#else
        if (!debug_step_arm(target))
            return false;
#endif
        __atomic_store_n(&target.debug_rearm_va, va, __ATOMIC_RELEASE);
        __atomic_store_n(&target.debug_stop_kind, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&target.debug_stop_va, 0, __ATOMIC_RELEASE);
        Scheduler::debugger_resume(target);
        return true;
    }
    __atomic_store_n(&target.debug_stop_kind, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&target.debug_stop_va, 0, __ATOMIC_RELEASE);
    Scheduler::debugger_resume(target);
    return true;
#endif
}

#if defined(CONFIG_ARCH_RISCV64) || defined(CONFIG_ARCH_AARCH64)
/// @brief Emulated step plant (no hardware step used): plant a temp
///        breakpoint past the current pc and resume; the temp hit reports
///        STEP via debug_route_fault. RISC-V decodes the RVC length and
///        saves+masks SSTATUS.SIE for the one step (#206 SIE discipline);
///        AArch64 instructions are fixed 4 bytes (temp brk at pc+4, no
///        masking needed — the temp persists across preemption).
/// @return false when the frame is unusable, the page is unmapped, or the
///         table is full (fail closed — the target stays parked).
bool emu_step_plant(TaskControlBlock &target) noexcept {
    uint64_t pc = 0;
    {
        uint64_t *frame = debug_frame_slot(target);
        if (frame == nullptr || !debug_frame_is_user(frame))
            return false;
#if defined(CONFIG_ARCH_RISCV64)
        pc = frame[31]; // OFF_SEPC
#else
        pc = frame[32]; // ELR
#endif
    }
    size_t insn_len = 4;
#if defined(CONFIG_ARCH_RISCV64)
    const uint8_t *insn = kBpInsn32;
    uint8_t half[2] = {};
    if (!bp_read_at(target, pc, half, 2))
        return false;
    insn_len = ((half[0] & 0x3U) != 0x3U) ? 2 : 4;
    insn = (insn_len == 2) ? kBpInsn16 : kBpInsn32;
#else
    const uint8_t *insn = kBpInsn;
#endif
    uint64_t next_va = pc + insn_len;
    bool planted = false;
    {
        SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
        for (size_t i = 0; i < kBpCap && !planted; ++i) {
            if (g_bp[i].used)
                continue;
            uint8_t orig[4] = {};
            if (!bp_read_at(target, next_va, orig, insn_len))
                return false;
            if (!bp_write_at(target, next_va, insn, insn_len))
                return false;
#if defined(CONFIG_ARCH_RISCV64)
            uint64_t *frame = debug_frame_slot(target);
            if (frame == nullptr || !debug_frame_is_user(frame))
                return false;
            // Save SIE before masking so the temp-hit path restores it.
            g_bp[i].sie_saved = (frame[32] >> 1) & 1ULL; // OFF_SSTATUS
#endif
            g_bp[i].target_id = target.id;
            g_bp[i].target_gen = target.generation;
            g_bp[i].va = next_va;
            for (size_t b = 0; b < insn_len; ++b)
                g_bp[i].orig[b] = orig[b];
            g_bp[i].len = static_cast<uint8_t>(insn_len);
            g_bp[i].temp = true;
            g_bp[i].used = true;
            planted = true;
        }
    }
    if (!planted)
        return false; // table full: stay parked, report failure
#if defined(CONFIG_ARCH_RISCV64)
    {
        uint64_t *frame = debug_frame_slot(target);
        if (frame == nullptr || !debug_frame_is_user(frame))
            return false;
        frame[32] &= ~(1ULL << 1); // mask SIE for the emulated step
    }
#endif
    return true;
}
#endif

bool debug_step(TaskControlBlock &target) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)target;
    return false;
#else
    if (!target.debug_parked)
        return false;
#if defined(CONFIG_ARCH_RISCV64) || defined(CONFIG_ARCH_AARCH64)
    // Temp hit reports STEP via debug_route_fault (SIE restored there on
    // RISC-V; AArch64 needs no masking — the temp persists).
    if (!emu_step_plant(target))
        return false;
    __atomic_store_n(&target.debug_stop_kind, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&target.debug_stop_va, 0, __ATOMIC_RELEASE);
    Scheduler::debugger_resume(target);
    return true;
#else
    if (!debug_step_arm(target))
        return false;
    __atomic_store_n(&target.debug_stop_kind, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&target.debug_stop_va, 0, __ATOMIC_RELEASE);
    Scheduler::debugger_resume(target);
    return true;
#endif
#endif
}

/// @brief STEP-completion trailer shared by the router: disarm the arch
///        step state and re-insert a step-over breakpoint when armed.
void debug_step_complete(TaskControlBlock &target) noexcept {
    debug_step_disarm(target);
    uint64_t rearm = __atomic_load_n(&target.debug_rearm_va, __ATOMIC_ACQUIRE);
    if (rearm != 0) {
        __atomic_store_n(&target.debug_rearm_va, 0, __ATOMIC_RELEASE);
        (void)debug_bp_insert(target, rearm);
    }
}

void debug_publish_death(TaskControlBlock &dying) noexcept {
#if !defined(CONFIG_DEBUG)
    (void)dying;
#else
    uint64_t debugger_id =
        __atomic_load_n(&dying.debugger_id, __ATOMIC_ACQUIRE);
    // Clear this target's shadows by id (tables are about to be freed —
    // mark unused, no byte restore).
    {
        SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
        for (size_t i = 0; i < kBpCap; ++i) {
            if (g_bp[i].used && g_bp[i].target_id == dying.id &&
                g_bp[i].target_gen == dying.generation)
                g_bp[i].used = false;
        }
    }
    if (debugger_id == 0)
        return;
    StopEvent event{};
    event.target_id = dying.id;
    event.target_gen = dying.generation;
    event.kind = static_cast<uint64_t>(StopKind::DEATH);
    event.fault_num = 0;
    event.fault_addr = 0;
    event.snap_state = static_cast<uint64_t>(TaskState::TERMINATED);
    event.snap_prio = dying.priority;
    event.snap_budget = 0;
    stop_enqueue(event); // death bypass: never dropped by normal pressure
    poke_debugger(debugger_id);
#endif
}

void debug_drop_target(uint64_t target_id, uint32_t target_gen) noexcept {
    {
        SpinLockGuard<sync::SpinLock> guard(g_stop_lock);
        for (size_t i = 0; i < kStopCap; ++i) {
            if (g_stops[i].used &&
                g_stops[i].event.target_id == target_id &&
                static_cast<uint32_t>(g_stops[i].event.target_gen) ==
                    target_gen) {
                g_stops[i].used = false;
                g_stops[i].event = StopEvent{};
            }
        }
    }
    {
        SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
        for (size_t i = 0; i < kBpCap; ++i) {
            if (g_bp[i].used && g_bp[i].target_id == target_id &&
                g_bp[i].target_gen == target_gen)
                g_bp[i].used = false;
        }
    }
}

void debug_snapshot_reset() noexcept {
    {
        SpinLockGuard<sync::SpinLock> guard(g_stop_lock);
        for (size_t i = 0; i < kStopCap; ++i) {
            g_stops[i].used = false;
            g_stops[i].event = StopEvent{};
            g_stops[i].seq = 0;
        }
    }
    __atomic_store_n(&g_stop_dropped, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stop_seq, 1, __ATOMIC_RELEASE);
    {
        SpinLockGuard<sync::SpinLock> guard(g_bp_lock);
        for (size_t i = 0; i < kBpCap; ++i)
            g_bp[i].used = false;
    }
}

} // namespace kernel::debug
