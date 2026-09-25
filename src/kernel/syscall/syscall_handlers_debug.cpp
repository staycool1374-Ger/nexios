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

/// @file syscall_handlers_debug.cpp
/// @brief Debugger syscalls (issue #225, spec docs/specs/debugd.md §3–§4).
///        Five slots (86–90): attach (selector + id), read/write regs,
///        read/write mem. Handles are kernel-minted (target, debugger,
///        gen) bindings — never pids. Raw pid lookup does not exist; the
///        only mint paths are launcher-claim (parenthood proof, selector 1)
///        and supervisor grant (deferred to Phase 4 with debugd).
///        Data calls on RUNNING/READY targets arm the deferred stop and
///        return EAGAIN; BLOCKED tasks are read directly (frames stable).
///        Release builds map these slots to sys_unimplemented (table level).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/debug/debug_regs.hpp>
#include <kernel/arch/page_table.hpp>

namespace kernel {

namespace {

// Linux errno numerics (cf. sys_unimplemented ENOSYS = 38).
constexpr uint64_t kEperm = 1;
constexpr uint64_t kEsrch = 3;
constexpr uint64_t kEbadf = 9;
constexpr uint64_t kEagain = 11;
constexpr uint64_t kEfault = 14;
constexpr uint64_t kEbusy = 16;
constexpr uint64_t kEinval = 22;

/// @brief Debug handle binding (issue #225). Fixed table, no heap.
///        Handle = (gen << 32) | index. Forgery requires guessing a live
///        (index, gen, debugger) triple; gen never repeats (atomic bump).
struct DebugBinding {
    uint64_t target_id = 0;
    uint64_t debugger_id = 0;
    uint32_t gen = 0;
    uint32_t target_gen = 0;
    bool live = false;
};

constexpr size_t kDebugBindings = 64;

DebugBinding g_bindings[kDebugBindings]{};
uint64_t g_debug_gen = 1; // gen 0 is never minted (decode sentinel)

// Serializes binding mint/clear/resolve (task context only; the tick park
// hook never takes this lock, so nesting it outside scheduler_lock_ in
// debugger_resume cannot invert the lock order).
sync::SpinLock g_bind_lock{};

/// @brief Resolve a handle for caller. Returns 0 with slot/target set,
///        or a Linux errno (EBADF/ESRCH) otherwise.
uint64_t debug_resolve(uint64_t caller_id, uint64_t handle,
                       DebugBinding *&slot_out, TaskControlBlock *&tgt_out) {
    slot_out = nullptr;
    tgt_out = nullptr;
    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    uint32_t gen = static_cast<uint32_t>(handle >> 32);
    if (idx >= kDebugBindings || gen == 0)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEbadf));
    // Caller holds g_bind_lock: slot mutation (mint/clear) is serialized.
    DebugBinding &slot = g_bindings[idx];
    if (!slot.live || slot.gen != gen || slot.debugger_id != caller_id)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEbadf));
    TaskControlBlock *tgt = Scheduler::find_task(slot.target_id);
    if (tgt == nullptr || !TaskControlBlock::is_valid(tgt) ||
        tgt->state == TaskState::TERMINATED || tgt->state == TaskState::REAPED ||
        slot.target_gen != tgt->generation)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    slot_out = &slot;
    tgt_out = tgt;
    return 0;
}

/// @brief Debuggable-state gate: user tasks only (kernel tasks EPERM).
bool debug_target_ok(const TaskControlBlock &tgt) noexcept {
    return tgt.is_user_;
}

} // namespace

uint64_t Syscall::sys_task_debug_attach(uint64_t sel, uint64_t id, uint64_t,
                                        uint64_t, uint64_t *) {
    TaskControlBlock *caller = syscall_task();
    if (caller == nullptr)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    if (sel == 0) {
        // Handle path: a valid owned handle toggles DETACH (the only
        // detach in Phase 1 — keeps the exact-five slot count). Unknown
        // handles fail EBADF (supervisor grants arrive in Phase 4).
        SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
        DebugBinding *slot = nullptr;
        TaskControlBlock *tgt = nullptr;
        uint64_t err = debug_resolve(caller->id, id, slot, tgt);
        if (err != 0)
            return err;
        // Detach: resume first while debugger_id is still set (the
        // resume guard requires a live id to prove the park hook still
        // owns the task), then drop the binding and clear target state.
        // Resume is scheduler-locked inside debugger_resume (READY, never
        // RUNNING: the runq owns READY tasks only). No interleaving risk:
        // handles are single-debugger (EBUSY) and the tick only parks on
        // stop_requested, which resume clears.
        Scheduler::debugger_resume(*tgt);
        slot->live = false;
        slot->target_gen = 0;
        __atomic_store_n(&tgt->debugger_id, 0, __ATOMIC_RELEASE);
        return 0;
    }
    if (sel == 1) {
        // Launcher-claim path: caller must be the target's parent.
        TaskControlBlock *tgt = Scheduler::find_task(id);
        if (tgt == nullptr || !TaskControlBlock::is_valid(tgt) ||
            tgt->state == TaskState::TERMINATED ||
            tgt->state == TaskState::REAPED)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
        if (!tgt->is_user_)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
        if (tgt->parent_id != caller->id)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
        if (__atomic_load_n(&tgt->debugger_id, __ATOMIC_ACQUIRE) != 0)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEbusy));
        SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
        if (__atomic_load_n(&tgt->debugger_id, __ATOMIC_ACQUIRE) != 0)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEbusy));
        for (size_t i = 0; i < kDebugBindings; ++i) {
            if (g_bindings[i].live)
                continue;
            uint32_t gen = static_cast<uint32_t>(
                __atomic_fetch_add(&g_debug_gen, 1u, __ATOMIC_RELAXED));
            if (gen == 0) // never mint gen 0 (decode sentinel above)
                gen = static_cast<uint32_t>(
                    __atomic_fetch_add(&g_debug_gen, 1u, __ATOMIC_RELAXED));
            g_bindings[i].target_id = tgt->id;
            g_bindings[i].debugger_id = caller->id;
            g_bindings[i].gen = gen;
            g_bindings[i].target_gen = tgt->generation;
            g_bindings[i].live = true;
            __atomic_store_n(&tgt->debugger_id, caller->id, __ATOMIC_RELEASE);
            return (static_cast<uint64_t>(gen) << 32) | i;
        }
        return static_cast<uint64_t>(-static_cast<int64_t>(kEbusy));
    }
    return static_cast<uint64_t>(-static_cast<int64_t>(kEbadf));
}

/// @brief Shared prologue for the four data calls: resolve + gate +
///        EAGAIN arming. Returns 0 with tgt set when the target's state
///        may be read; otherwise a Linux errno.
uint64_t debug_data_begin(uint64_t caller_id, uint64_t handle,
                          TaskControlBlock *&tgt_out) {
    DebugBinding *slot = nullptr;
    TaskControlBlock *tgt = nullptr;
    // Binding lookup serialized (mint/clear cannot interleave the resolve).
    SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
    uint64_t err = debug_resolve(caller_id, handle, slot, tgt);
    if (err != 0)
        return err;
    (void)slot;
    if (!debug_target_ok(*tgt))
        return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
    if (tgt->state == TaskState::RUNNING || tgt->state == TaskState::READY) {
        // Runnable: arm the deferred stop (tick parks at a user-mode
        // boundary) and ask the debugger to retry. Never torn reads.
        __atomic_store_n(&tgt->debug_stop_requested, true, __ATOMIC_RELEASE);
        return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
    }
    if (!__atomic_load_n(&tgt->has_utrap_frame, __ATOMIC_ACQUIRE))
        return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
    tgt_out = tgt;
    return 0;
}

uint64_t Syscall::sys_task_debug_read_regs(uint64_t handle, uint64_t ubuf,
                                           uint64_t, uint64_t, uint64_t *) {
    TaskControlBlock *caller = syscall_task();
    if (caller == nullptr)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    TaskControlBlock *tgt = nullptr;
    uint64_t err = debug_data_begin(caller->id, handle, tgt);
    if (err != 0)
        return err;
    constexpr size_t kQwords = 34; // max blob (aarch64 272B); all arches fit
    uint64_t blob[kQwords] = {};
    if (!debug::debug_read_regs(*tgt, blob, kQwords))
        return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
    const size_t bytes = debug::debug_blob_bytes();
    // The target may wake past BLOCKED between resolve and delivery
    // (sleep/IPC expiry): recheck, never deliver a torn frame.
    if (tgt->state == TaskState::RUNNING || tgt->state == TaskState::READY)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
    auto *dst = reinterpret_cast<uint8_t *>(ubuf);
    if (!safe_copy_to_user(dst, reinterpret_cast<const uint8_t *>(blob),
                           bytes))
        return static_cast<uint64_t>(-static_cast<int64_t>(kEfault));
    return 0;
}

uint64_t Syscall::sys_task_debug_write_regs(uint64_t handle, uint64_t ubuf,
                                            uint64_t, uint64_t, uint64_t *) {
    TaskControlBlock *caller = syscall_task();
    if (caller == nullptr)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    TaskControlBlock *tgt = nullptr;
    uint64_t err = debug_data_begin(caller->id, handle, tgt);
    if (err != 0)
        return err;
    constexpr size_t kQwords = 34;
    uint64_t blob[kQwords] = {};
    const size_t bytes = debug::debug_blob_bytes();
    auto *src = reinterpret_cast<const uint8_t *>(ubuf);
    if (!safe_copy_from_user(reinterpret_cast<uint8_t *>(blob), src, bytes))
        return static_cast<uint64_t>(-static_cast<int64_t>(kEfault));
    // The target may wake past BLOCKED between resolve and delivery:
    // recheck, never race live execution with a register write.
    if (tgt->state == TaskState::RUNNING || tgt->state == TaskState::READY)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
    if (!debug::debug_write_regs(*tgt, blob, kQwords))
        return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
    return 0;
}

uint64_t Syscall::sys_task_debug_read_mem(uint64_t handle, uint64_t uva,
                                          uint64_t len, uint64_t ubuf,
                                          uint64_t *) {
    TaskControlBlock *caller = syscall_task();
    if (caller == nullptr)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    if (len > 64 * 1024)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEinval));
    TaskControlBlock *tgt = nullptr;
    uint64_t err = debug_data_begin(caller->id, handle, tgt);
    if (err != 0)
        return err;
    uint64_t done = 0;
    while (done < len) {
        uint64_t va = uva + done;
        uint64_t page_off = va & 0xFFFULL;
        uint64_t chunk = 0x1000ULL - page_off;
        if (chunk > len - done)
            chunk = len - done;
        uint64_t phys = VMM::virt_to_phys_in_pml4(va, tgt->page_table_);
        if (phys == 0)
            break; // unmapped: stop with bytes-done (EFAULT below)
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *src = reinterpret_cast<const uint8_t *>(arch::HHDM_OFFSET +
                                                      phys + page_off);
        auto *dst = reinterpret_cast<uint8_t *>(ubuf + done);
        // Bounded stack staging keeps each user copy fault-contained.
        uint8_t stage[256];
        uint64_t off = 0;
        while (off < chunk) {
            uint64_t n = chunk - off;
            if (n > sizeof(stage))
                n = sizeof(stage);
            for (uint64_t i = 0; i < n; ++i)
                stage[i] = src[off + i];
            // Copy-side faults report EIO (distinct from walker-side
            // EFAULT above): tells the debugger whether the target
            // mapping (EFAULT) or the caller buffer (EIO) faulted.
            if (!safe_copy_to_user(dst + off, stage, n))
                return static_cast<uint64_t>(-5);
            off += n;
        }
        if (off < chunk)
            break;
        done += chunk;
    }
    if (done < len)
        return done > 0 ? done
                        : static_cast<uint64_t>(-static_cast<int64_t>(kEfault));
    return done;
}

uint64_t Syscall::sys_task_debug_write_mem(uint64_t handle, uint64_t uva,
                                           uint64_t len, uint64_t ubuf,
                                           uint64_t *) {
    TaskControlBlock *caller = syscall_task();
    if (caller == nullptr)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    if (len > 64 * 1024)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEinval));
    TaskControlBlock *tgt = nullptr;
    uint64_t err = debug_data_begin(caller->id, handle, tgt);
    if (err != 0)
        return err;
    uint64_t done = 0;
    while (done < len) {
        uint64_t va = uva + done;
        uint64_t page_off = va & 0xFFFULL;
        uint64_t chunk = 0x1000ULL - page_off;
        if (chunk > len - done)
            chunk = len - done;
        uint64_t phys = VMM::virt_to_phys_in_pml4(va, tgt->page_table_);
        if (phys == 0)
            break;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *dst = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys +
                                                page_off);
        auto *src = reinterpret_cast<const uint8_t *>(ubuf + done);
        uint8_t stage[256];
        uint64_t off = 0;
        while (off < chunk) {
            uint64_t n = chunk - off;
            if (n > sizeof(stage))
                n = sizeof(stage);
            if (!safe_copy_from_user(stage, src + off, n))
                return static_cast<uint64_t>(-5); // EIO: caller buffer
            for (uint64_t i = 0; i < n; ++i)
                dst[off + i] = stage[i];
            off += n;
        }
        if (off < chunk)
            break;
        done += chunk;
    }
    if (done > 0) {
        // I-cache coherence is the kernel's job (spec §3): TCG hides a
        // missing flush, real hardware does not (issue #186 pitfall).
#if defined(CONFIG_ARCH_RISCV64)
        asm volatile(".option push\n.option arch, +zifencei\nfence.i\n.option pop" ::
                         : "memory");
#elif defined(CONFIG_ARCH_AARCH64)
        asm volatile("ic ialluis\n dsb ish\n isb" ::: "memory");
#else
        // x86_64 is I-cache coherent; no flush needed.
#endif
        arch::ArchPageTable::tlb_flush_all();
    }
    if (done < len)
        return done > 0 ? done
                        : static_cast<uint64_t>(-static_cast<int64_t>(kEfault));
    return done;
}

} // namespace kernel
