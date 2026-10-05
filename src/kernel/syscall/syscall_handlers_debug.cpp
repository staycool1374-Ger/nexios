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
///        and supervisor grant (selectors 7/8, issue #239; loop wiring in
///        Phase 4 with debugd, issue #232).
///        Data calls on RUNNING/READY targets arm the deferred stop and
///        return EAGAIN; BLOCKED tasks are read directly (frames stable).
///        Release builds map these slots to sys_unimplemented (table level).
///        Control selectors need no new syscall numbers (ABI frozen):
///        2 = poll, 3/4 = breakpoint insert/clear, 5 = step, 6 = continue,
///        9 = stop-request (Ctrl-C hook, issue #232).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/debug/debug_regs.hpp>
#include <kernel/debug/debug_stop.hpp>
#include <kernel/debug/debug_bind.hpp>
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
///        Supervisor-grant bindings (issue #239, spec §14) reuse these
///        slots: `debugger_id` stays the OWNER (= grantee) so all
///        single-debugger checks are untouched; grant metadata rides
///        alongside (`granted`, `grantor_id`). `attenuation` and
///        `grant_flags` are zero-reserved, never interpreted until #50.
struct DebugBinding {
    uint64_t target_id = 0;
    uint64_t debugger_id = 0;
    uint32_t gen = 0;
    uint32_t target_gen = 0;
    bool live = false;
    uint64_t grantor_id = 0;
    bool granted = false;
    uint64_t attenuation = 0;
    uint32_t grant_flags = 0;
};

constexpr size_t kDebugBindings = 64;

DebugBinding g_bindings[kDebugBindings]{};
uint64_t g_debug_gen = 1; // gen 0 is never minted (decode sentinel)
/// @brief Live-binding count for debug_session_active() (issue #232).
/// Updated at every live=true/false transition under g_bind_lock;
/// the predicate reads it lock-free (any context, including tick/ISR).
static uint64_t g_live_binding_count = 0;

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

/// @brief Shared detach disposition (issue #239, spec §14.5/I-3).
///        Lock-agnostic: takes no bind lock itself (callees self-lock);
///        callers hold `g_bind_lock` (sel8) or run the unlocked drain
///        loop. Mirrors the sel0 detach body exactly: shadows restore
///        first (tables still live); fault-stopped targets terminate
///        (clear id/flags first, then terminate); clean/breakpoint/step
///        stops resume first while the id is live, then clear. The
///        caller owns binding-slot clearing + event drops.
void debug_apply_disposition(TaskControlBlock &tgt) noexcept {
    uint64_t kind =
        __atomic_load_n(&tgt.debug_stop_kind, __ATOMIC_ACQUIRE);
    debug::debug_bp_restore_all(tgt);
    if (kind == static_cast<uint64_t>(debug::StopKind::FAULT)) {
        __atomic_store_n(&tgt.debugger_id, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&tgt.debug_stop_requested, false,
                         __ATOMIC_RELEASE);
        tgt.debug_parked = false;
        __atomic_store_n(&tgt.debug_stop_kind, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&tgt.debug_stop_va, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&tgt.debug_rearm_va, 0, __ATOMIC_RELEASE);
        (void)Scheduler::terminate_err(tgt, tgt.exit_code);
        return;
    }
    Scheduler::debugger_resume(tgt);
    __atomic_store_n(&tgt.debugger_id, 0, __ATOMIC_RELEASE);
}

} // namespace

namespace debug {

/// @brief Launch handoff for `runelf --debug` (issue #231, decision B1):
///        single call performing sel1-claim immediately followed by the
///        sel7 grant-transfer to @p grantee_id, then the sel9 belt arm.
///        A literal sel1→sel7→sel9 sequence through the syscall entry
///        cannot work (after sel1 the target reads EBUSY; the grant
///        binding is debugd-owned so a shell sel9 reads EBADF) — this
///        helper performs the three mutations with explicit ids instead.
///        Denial table mirrors sel1+sel7 (ESRCH/EPERM/EBUSY, no new
///        errno); no new syscall or selector (spec §9.6).
/// @param shell_id Launcher task id (parenthood proof, §9.3).
/// @param target_id Freshly added launch target.
/// @param grantee_id Debug daemon pid (named grantee, §9.3).
/// @param handle_out Minted binding handle on success.
/// @return 0 on success, else -ESRCH/-EPERM/-EBUSY.
/// @brief Shared handoff core: denial checks + slot mint on resolved
///        pointers (no registry lookup). Denial order mirrors sel1+sel7.
static uint64_t handoff_mint(uint64_t shell_id, TaskControlBlock *tgt,
                             TaskControlBlock *grantee,
                             uint64_t grantee_id,
                             uint64_t &handle_out) noexcept {
    handle_out = 0;
    if (tgt == nullptr || !TaskControlBlock::is_valid(tgt) ||
        tgt->state == TaskState::TERMINATED ||
        tgt->state == TaskState::REAPED)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    if (!tgt->is_user_)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
    if (grantee == nullptr || !TaskControlBlock::is_valid(grantee) ||
        grantee->state == TaskState::TERMINATED ||
        grantee->state == TaskState::REAPED)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    if (tgt->parent_id != shell_id)
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
        if (gen == 0)
            gen = static_cast<uint32_t>(
                __atomic_fetch_add(&g_debug_gen, 1u, __ATOMIC_RELAXED));
        g_bindings[i].target_id = tgt->id;
        g_bindings[i].debugger_id = grantee_id;
        g_bindings[i].gen = gen;
        g_bindings[i].target_gen = tgt->generation;
        g_bindings[i].live = true;
        __atomic_fetch_add(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
        g_bindings[i].grantor_id = shell_id;
        g_bindings[i].granted = true;
        g_bindings[i].attenuation = 0;
        g_bindings[i].grant_flags = 0;
        __atomic_store_n(&tgt->debugger_id, grantee_id, __ATOMIC_RELEASE);
        // sel9 belt (spec §9.4): arm the deferred stop on a runnable
        // target; parked targets need nothing. Verbatim sel9 semantics.
        if (tgt->state == TaskState::RUNNING ||
            tgt->state == TaskState::READY)
            __atomic_store_n(&tgt->debug_stop_requested, true,
                             __ATOMIC_RELEASE);
        handle_out = (static_cast<uint64_t>(gen) << 32) | i;
        return 0;
    }
    return static_cast<uint64_t>(-static_cast<int64_t>(kEbusy));
}

uint64_t debug_launch_handoff(uint64_t shell_id, uint64_t target_id,
                              uint64_t grantee_id,
                              uint64_t &handle_out) noexcept {
    TaskControlBlock *tgt = Scheduler::find_task(target_id);
    TaskControlBlock *grantee = Scheduler::find_task(grantee_id);
    return handoff_mint(shell_id, tgt, grantee, grantee_id, handle_out);
}

uint64_t debug_launch_handoff_tc(uint64_t shell_id, TaskControlBlock &tgt,
                                 uint64_t grantee_id,
                                 uint64_t &handle_out) noexcept {
    TaskControlBlock *grantee = Scheduler::find_task(grantee_id);
    return handoff_mint(shell_id, &tgt, grantee, grantee_id, handle_out);
}

/// @brief Fail-closed teardown for a `--debug` launch (issue #231, spec
///        §9.2e/§9.7): drop the handoff binding slot (if any), restore
///        breakpoint shadows via the shared disposition path, then
///        terminate the just-added task via the admitted-task path and
///        drain. Never leaves the target running. No new errno (caller
///        reports the original failure).
/// @param task Just-added launch target (live).
/// @param handle Handoff handle from debug_launch_handoff, or 0 when the
///        handoff never succeeded (pure terminate+drain then).
/// @brief Pre-add fail-closed undo (issue #295, see header): the
///        slot-drop + shadow-restore + flag-clear half of teardown
///        WITHOUT terminate/drain (the target was never added).
void debug_launch_undo(TaskControlBlock &task, uint64_t handle) noexcept {
    if (handle == 0)
        return;
    SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    uint32_t gen = static_cast<uint32_t>(handle >> 32);
    if (idx < kDebugBindings && gen != 0) {
        DebugBinding &slot = g_bindings[idx];
        if (slot.live && slot.gen == gen && slot.target_id == task.id) {
            debug::debug_bp_restore_all(task);
            debug::debug_drop_target(task.id, task.generation);
            slot.live = false;
            __atomic_fetch_sub(&g_live_binding_count, 1u,
                               __ATOMIC_RELAXED);
            slot.target_gen = 0;
            slot.granted = false;
            slot.grantor_id = 0;
        }
    }
    __atomic_store_n(&task.debugger_id, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&task.debug_stop_requested, false, __ATOMIC_RELEASE);
    task.debug_parked = false;
    __atomic_store_n(&task.debug_stop_kind, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&task.debug_stop_va, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&task.debug_rearm_va, 0, __ATOMIC_RELEASE);
}

void debug_launch_teardown(TaskControlBlock &task, uint64_t handle) noexcept {
    debug_launch_undo(task, handle);
    (void)Scheduler::terminate_err(task, task.exit_code);
    Scheduler::drain_zombie_list();
}

} // namespace debug

uint64_t Syscall::sys_task_debug_attach(uint64_t sel, uint64_t id, uint64_t arg2,
                                        uint64_t, uint64_t *) {
    TaskControlBlock *caller = syscall_task();
    if (caller == nullptr)
        return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
    if (sel == 0) {
        // Handle path: a valid owned handle toggles DETACH (supervisor
        // grants, selectors 7/8, issue #239). Disposition (spec §4): a fault-stopped
        // target (stop kind latched by the router) applies the default
        // disposition (terminate); a cleanly-stopped target resumes.
        SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
        DebugBinding *slot = nullptr;
        TaskControlBlock *tgt = nullptr;
        uint64_t err = debug_resolve(caller->id, id, slot, tgt);
        if (err != 0) {
            // Dead-target detach: the binding is still owned — drop it
            // (plus queued events/shadows by id) instead of leaking the
            // slot. Unknown handles still fail with the original error.
            uint32_t idx = static_cast<uint32_t>(id & 0xFFFFFFFFULL);
            uint32_t gen = static_cast<uint32_t>(id >> 32);
            if (idx < kDebugBindings && gen != 0) {
                DebugBinding &owned = g_bindings[idx];
                if (owned.live && owned.gen == gen &&
                    owned.debugger_id == caller->id) {
                    uint64_t dead_id = owned.target_id;
                    uint32_t dead_gen = owned.target_gen;
                    owned.live = false;
                    __atomic_fetch_sub(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
                    owned.target_gen = 0;
                    debug::debug_drop_target(dead_id, dead_gen);
                    return 0;
                }
            }
            return err;
        }
        uint64_t kind =
            __atomic_load_n(&tgt->debug_stop_kind, __ATOMIC_ACQUIRE);
        // Shadows restore first on every path (tables still live).
        debug::debug_bp_restore_all(*tgt);
        if (kind == static_cast<uint64_t>(debug::StopKind::FAULT)) {
            // Fault-stopped: default disposition (terminate). Breakpoint
            // and step stops are debugger-driven (GDB detach semantics:
            // resume); only genuine faults terminate. Binding/events drop
            // first, then terminate (task context — allowed here). The
            // exit code is whatever the target last set (the router does
            // not synthesize signals); waitpid parents observe it as-is.
            debug::debug_drop_target(tgt->id, tgt->generation);
            slot->live = false;
            __atomic_fetch_sub(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
            slot->target_gen = 0;
            __atomic_store_n(&tgt->debugger_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&tgt->debug_stop_requested, false,
                             __ATOMIC_RELEASE);
            tgt->debug_parked = false;
            __atomic_store_n(&tgt->debug_stop_kind, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&tgt->debug_stop_va, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&tgt->debug_rearm_va, 0, __ATOMIC_RELEASE);
            (void)Scheduler::terminate_err(*tgt, tgt->exit_code);
            return 0;
        }
        // Clean park, breakpoint, or step stop: resume first while
        // debugger_id is still set (the resume guard requires a live id
        // to prove the park hook still owns the task), then drop the
        // binding and clear state. Resume is scheduler-locked inside
        // debugger_resume (READY, never RUNNING: the runq owns READY
        // tasks only). No interleaving risk: handles are single-debugger
        // (EBUSY) and the tick only parks on stop_requested, which resume
        // clears.
        Scheduler::debugger_resume(*tgt);
        debug::debug_drop_target(tgt->id, tgt->generation);
        slot->live = false;
        __atomic_fetch_sub(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
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
            __atomic_fetch_add(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
            g_bindings[i].grantor_id = 0;
            g_bindings[i].granted = false;
            g_bindings[i].attenuation = 0;
            g_bindings[i].grant_flags = 0;
            __atomic_store_n(&tgt->debugger_id, caller->id, __ATOMIC_RELEASE);
            return (static_cast<uint64_t>(gen) << 32) | i;
        }
        return static_cast<uint64_t>(-static_cast<int64_t>(kEbusy));
    }
    if (sel == 7) {
        // Grant-mint (issue #239, spec §14.2): authorize debugger
        // `arg2` over `id` without parenthood. Grantor check: PID 1
        // supervises any user task; a launcher delegates for its own
        // claimed children only. No new syscall numbers (selector).
        TaskControlBlock *tgt = Scheduler::find_task(id);
        if (tgt == nullptr || !TaskControlBlock::is_valid(tgt) ||
            tgt->state == TaskState::TERMINATED ||
            tgt->state == TaskState::REAPED)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
        if (!tgt->is_user_)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
        TaskControlBlock *grantee = Scheduler::find_task(arg2);
        if (grantee == nullptr || !TaskControlBlock::is_valid(grantee) ||
            grantee->state == TaskState::TERMINATED ||
            grantee->state == TaskState::REAPED)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEsrch));
        if (caller->id != 1 && tgt->parent_id != caller->id)
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
            g_bindings[i].debugger_id = arg2;
            g_bindings[i].gen = gen;
            g_bindings[i].target_gen = tgt->generation;
            g_bindings[i].live = true;
            __atomic_fetch_add(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
            g_bindings[i].grantor_id = caller->id;
            g_bindings[i].granted = true;
            g_bindings[i].attenuation = 0;
            g_bindings[i].grant_flags = 0;
            __atomic_store_n(&tgt->debugger_id, arg2, __ATOMIC_RELEASE);
            return (static_cast<uint64_t>(gen) << 32) | i;
        }
        return static_cast<uint64_t>(-static_cast<int64_t>(kEbusy));
    }
    if (sel == 8) {
        // Grant-revoke (issue #239, spec §14.5): the grantor (or PID 1)
        // drops a granted binding with the shared disposition; post-
        // revoke resolves fail EBADF via the generation burn. Non-grant
        // slots are not revocable (EPERM — owned bindings detach via
        // sel0). Whole op under the outer guard (bind -> scheduler).
        SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
        uint32_t idx = static_cast<uint32_t>(id & 0xFFFFFFFFULL);
        uint32_t gen = static_cast<uint32_t>(id >> 32);
        if (idx >= kDebugBindings || gen == 0)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEbadf));
        DebugBinding &slot = g_bindings[idx];
        if (!slot.live || slot.gen != gen || !slot.granted)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEbadf));
        if (slot.grantor_id != caller->id && caller->id != 1)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
        TaskControlBlock *tgt = Scheduler::find_task(slot.target_id);
        uint64_t dead_id = slot.target_id;
        uint32_t dead_gen = slot.target_gen;
        if (tgt == nullptr || !TaskControlBlock::is_valid(tgt) ||
            tgt->state == TaskState::TERMINATED ||
            tgt->state == TaskState::REAPED ||
            slot.target_gen != tgt->generation ||
            __atomic_load_n(&tgt->debugger_id, __ATOMIC_ACQUIRE) !=
                slot.debugger_id) {
            slot.live = false;
            __atomic_fetch_sub(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
            slot.target_gen = 0;
            slot.granted = false;
            slot.grantor_id = 0;
            uint32_t burn = static_cast<uint32_t>(
                __atomic_fetch_add(&g_debug_gen, 1u, __ATOMIC_RELAXED));
            if (burn == 0)
                burn = static_cast<uint32_t>(__atomic_fetch_add(
                    &g_debug_gen, 1u, __ATOMIC_RELAXED));
            slot.gen = burn;
            debug::debug_drop_target(dead_id, dead_gen);
            return 0;
        }
        debug_apply_disposition(*tgt);
        debug::debug_drop_target(tgt->id, tgt->generation);
        slot.live = false;
        __atomic_fetch_sub(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
        slot.target_gen = 0;
        slot.granted = false;
        slot.grantor_id = 0;
        {
            uint32_t burn = static_cast<uint32_t>(
                __atomic_fetch_add(&g_debug_gen, 1u, __ATOMIC_RELAXED));
            if (burn == 0)
                burn = static_cast<uint32_t>(__atomic_fetch_add(
                    &g_debug_gen, 1u, __ATOMIC_RELAXED));
            slot.gen = burn;
        }
        return 0;
    }
    // Control selectors (issue #226 — no new syscall numbers, ABI frozen):
    // 2 = poll one stop event into the caller buffer (arg2 = ubuf);
    // 3 = insert breakpoint at VA (arg2 = va); 4 = clear breakpoint;
    // 5 = single-step the parked target; 6 = continue (step-over + re-arm
    // on breakpoint stops, plain resume otherwise).
    // LOCK DISCIPLINE (g_bind_lock is non-recursive): sel 2 validates +
    // polls through self-locking helpers with NO outer guard held (the
    // poll path re-enters the binding layer via lookup — nesting
    // self-deadlocks). Selectors 3–6 hold the outer guard across resolve
    // + table ops (they never re-enter the binding layer; order is
    // bind -> {bp, stop, scheduler}, consistent with detach/drain).
    if (sel == 2) {
        // Poll: ownership without liveness — death events stay pollable
        // after the target died. Empty reads EAGAIN (the debugger drains
        // after each Notify pulse); caller-buffer faults report EIO
        // (copy-side rule, spec §3).
        if (!debug::debug_validate_handle(caller->id, id))
            return static_cast<uint64_t>(-static_cast<int64_t>(kEbadf));
        debug::StopEvent event{};
        if (!debug::debug_poll_event(caller->id, event))
            return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
        auto *dst = reinterpret_cast<uint8_t *>(arg2);
        if (!safe_copy_to_user(dst, reinterpret_cast<const uint8_t *>(&event),
                               sizeof(event)))
            return static_cast<uint64_t>(-5); // EIO: caller buffer
        return 0;
    }
    if (sel >= 3 && sel <= 6) {
        SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
        DebugBinding *slot = nullptr;
        TaskControlBlock *tgt = nullptr;
        uint64_t err = debug_resolve(caller->id, id, slot, tgt);
        if (err != 0)
            return err;
        (void)slot;
        if (!debug_target_ok(*tgt))
            return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
        if (sel == 3 || sel == 4) {
            // Breakpoint insert/clear: pure table writes through the
            // target's tables — no park needed, never torn reads.
            bool ok = (sel == 3) ? debug::debug_bp_insert(*tgt, arg2)
                                 : debug::debug_bp_clear(*tgt, arg2);
            return ok ? 0
                      : static_cast<uint64_t>(-static_cast<int64_t>(kEinval));
        }
        // Step/continue need a parked target; a runnable target arms the
        // deferred stop and reports EAGAIN (data-call discipline).
        if (tgt->state == TaskState::RUNNING ||
            tgt->state == TaskState::READY) {
            if (sel == 6)
                return 0; // already running: continue is a no-op
            __atomic_store_n(&tgt->debug_stop_requested, true,
                             __ATOMIC_RELEASE);
            return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
        }
        if (!tgt->debug_parked)
            return static_cast<uint64_t>(-static_cast<int64_t>(kEagain));
        bool ok = (sel == 5) ? debug::debug_step(*tgt)
                             : debug::debug_continue(*tgt);
        return ok ? 0
                  : static_cast<uint64_t>(-static_cast<int64_t>(kEinval));
    }
    if (sel == 9) {
        // Stop-request (issue #232, Ctrl-C hook, §13.3): arm the
        // deferred stop on a runnable target, no-op on a parked one.
        // Never touches trap frames (unlike the read_regs EAGAIN-arm
        // side path). No new errno: unknown/foreign handles fail with
        // the original EBADF/ESRCH via debug_resolve, dead targets ESRCH.
        SpinLockGuard<sync::SpinLock> bind_guard(g_bind_lock);
        DebugBinding *slot = nullptr;
        TaskControlBlock *tgt = nullptr;
        uint64_t err = debug_resolve(caller->id, id, slot, tgt);
        if (err != 0)
            return err;
        (void)slot;
        if (!debug_target_ok(*tgt))
            return static_cast<uint64_t>(-static_cast<int64_t>(kEperm));
        if (tgt->state == TaskState::RUNNING ||
            tgt->state == TaskState::READY) {
            __atomic_store_n(&tgt->debug_stop_requested, true,
                             __ATOMIC_RELEASE);
            return 0;
        }
        return 0; // parked/terminated-adjacent: no-op, stays stopped
    }
    return static_cast<uint64_t>(-static_cast<int64_t>(kEbadf));
}

namespace debug {

bool debug_session_active() noexcept {
    // Lock-free relaxed read (see g_live_binding_count): safe from any
    // context including tick/ISR, where taking g_bind_lock could
    // self-deadlock against a preempted task-context holder.
    return __atomic_load_n(&g_live_binding_count, __ATOMIC_RELAXED) != 0;
}

bool debug_validate_handle(uint64_t caller_id, uint64_t handle) noexcept {
    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFULL);
    uint32_t gen = static_cast<uint32_t>(handle >> 32);
    if (idx >= kDebugBindings || gen == 0)
        return false;
    SpinLockGuard<sync::SpinLock> guard(g_bind_lock);
    const DebugBinding &slot = g_bindings[idx];
    return slot.live && slot.gen == gen && slot.debugger_id == caller_id;
}

void debug_drain_debugger(TaskControlBlock &dying) noexcept {
    // Collect owned bindings under the table lock; dispositions run after
    // (terminate/resume take scheduler_lock_: bind -> scheduler order).
    struct Owned {
        uint64_t target_id;
        uint32_t target_gen;
        size_t slot;
        // Third-party grantor death (issue #232, spec §14.5 literal):
        // this slot was granted BY the dying task TO a live grantee.
        // Recorded here because the dispose loop below can no longer
        // tell grantor-death from debugger-death once slots clear.
        bool grantor_death;
    };
    Owned owned[kDebugBindings];
    size_t n_owned = 0;
    {
        SpinLockGuard<sync::SpinLock> guard(g_bind_lock);
        for (size_t i = 0; i < kDebugBindings; ++i) {
            if (g_bindings[i].live && g_bindings[i].debugger_id == dying.id) {
                owned[n_owned].target_id = g_bindings[i].target_id;
                owned[n_owned].target_gen = g_bindings[i].target_gen;
                owned[n_owned].slot = i;
                owned[n_owned].grantor_death = false;
                ++n_owned;
            }
            // Grantor death (issue #239, spec §14.5): slots this task
            // granted to others revoke synchronously with the shared
            // disposition (grantee-death needs no new code — granted
            // slots are owned by the grantee and collected above).
            if (g_bindings[i].live && g_bindings[i].granted &&
                g_bindings[i].grantor_id == dying.id &&
                g_bindings[i].debugger_id != dying.id) {
                owned[n_owned].target_id = g_bindings[i].target_id;
                owned[n_owned].target_gen = g_bindings[i].target_gen;
                owned[n_owned].slot = i;
                owned[n_owned].grantor_death = true;
                ++n_owned;
            }
        }
    }
    for (size_t i = 0; i < n_owned; ++i) {
        TaskControlBlock *tgt = Scheduler::find_task(owned[i].target_id);
        bool alive = tgt != nullptr && TaskControlBlock::is_valid(tgt) &&
                     tgt->state != TaskState::TERMINATED &&
                     tgt->state != TaskState::REAPED &&
                     tgt->generation == owned[i].target_gen &&
                     __atomic_load_n(&tgt->debugger_id, __ATOMIC_ACQUIRE) ==
                         dying.id;
        if (alive) {
            uint64_t kind = __atomic_load_n(&tgt->debug_stop_kind,
                                            __ATOMIC_ACQUIRE);
            // Shadows restore first (tables still live). Disposition runs
            // BEFORE clearing debugger_id: the resume guard requires a
            // live id (same ordering as detach — resume-first). Only
            // genuine faults terminate; clean/breakpoint/step stops resume
            // (GDB detach semantics).
            debug::debug_bp_restore_all(*tgt);
            if (kind == static_cast<uint64_t>(debug::StopKind::FAULT)) {
                // Fault-stopped: clear first so the later reaper cleanup
                // publishes no orphan death event to the dead debugger,
                // then terminate.
                __atomic_store_n(&tgt->debugger_id, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&tgt->debug_stop_requested, false,
                                 __ATOMIC_RELEASE);
                tgt->debug_parked = false;
                __atomic_store_n(&tgt->debug_stop_kind, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&tgt->debug_stop_va, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&tgt->debug_rearm_va, 0, __ATOMIC_RELEASE);
                (void)Scheduler::terminate_err(*tgt, tgt->exit_code);
            } else {
                Scheduler::debugger_resume(*tgt);
                __atomic_store_n(&tgt->debugger_id, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&tgt->debug_stop_requested, false,
                                 __ATOMIC_RELEASE);
                tgt->debug_parked = false;
                __atomic_store_n(&tgt->debug_stop_kind, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&tgt->debug_stop_va, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&tgt->debug_rearm_va, 0, __ATOMIC_RELEASE);
            }
            debug::debug_drop_target(tgt->id, tgt->generation);
        } else if (owned[i].grantor_death) {
            // Third-party grantor death with a live grantee (issue #232,
            // spec §14.5 literal): the shared disposition applies even
            // though the debugger is alive — otherwise the target parks
            // forever with a stale debugger_id (permanent EBUSY). Mirrors
            // the sel8 live-target path: disposition first (resume needs
            // the live id), then drop + slot clear with gen burn below.
            TaskControlBlock *t2 =
                Scheduler::find_task(owned[i].target_id);
            if (t2 != nullptr && TaskControlBlock::is_valid(t2) &&
                t2->state != TaskState::TERMINATED &&
                t2->state != TaskState::REAPED &&
                t2->generation == owned[i].target_gen) {
                debug_apply_disposition(*t2);
                debug::debug_drop_target(t2->id, t2->generation);
            } else {
                debug::debug_drop_target(owned[i].target_id,
                                         owned[i].target_gen);
            }
        } else {
            debug::debug_drop_target(owned[i].target_id, owned[i].target_gen);
        }
        SpinLockGuard<sync::SpinLock> guard(g_bind_lock);
        g_bindings[owned[i].slot].live = false;
        __atomic_fetch_sub(&g_live_binding_count, 1u, __ATOMIC_RELAXED);
        g_bindings[owned[i].slot].target_gen = 0;
        g_bindings[owned[i].slot].granted = false;
        g_bindings[owned[i].slot].grantor_id = 0;
        g_bindings[owned[i].slot].attenuation = 0;
        g_bindings[owned[i].slot].grant_flags = 0;
        uint32_t burn = static_cast<uint32_t>(
            __atomic_fetch_add(&g_debug_gen, 1u, __ATOMIC_RELAXED));
        if (burn == 0)
            burn = static_cast<uint32_t>(
                __atomic_fetch_add(&g_debug_gen, 1u, __ATOMIC_RELAXED));
        g_bindings[owned[i].slot].gen = burn;
    }
}

size_t debug_collect_owned(uint64_t debugger_id, uint64_t *ids_out,
                             uint32_t *gens_out, size_t cap) noexcept {
    if (ids_out == nullptr || gens_out == nullptr || cap == 0)
        return 0;
    SpinLockGuard<sync::SpinLock> guard(g_bind_lock);
    size_t n = 0;
    for (size_t i = 0; i < kDebugBindings && n < cap; ++i) {
        if (g_bindings[i].live && g_bindings[i].debugger_id == debugger_id) {
            ids_out[n] = g_bindings[i].target_id;
            gens_out[n] = g_bindings[i].target_gen;
            ++n;
        }
    }
    return n;
}

void debug_bindings_reset() noexcept {
    SpinLockGuard<sync::SpinLock> guard(g_bind_lock);
    for (size_t i = 0; i < kDebugBindings; ++i) {
        g_bindings[i].live = false;
        g_bindings[i].target_id = 0;
        g_bindings[i].debugger_id = 0;
        g_bindings[i].gen = 0;
        g_bindings[i].target_gen = 0;
        g_bindings[i].grantor_id = 0;
        g_bindings[i].granted = false;
        g_bindings[i].attenuation = 0;
        g_bindings[i].grant_flags = 0;
    }
    __atomic_store_n(&g_live_binding_count, 0, __ATOMIC_RELAXED);
    // g_debug_gen stays monotonic (never mint a repeated generation).
}

} // namespace debug

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
        // Walk result is the exact phys (offset already included — never
        // add page_off again; issue #226 double-add).
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *src =
            reinterpret_cast<const uint8_t *>(arch::HHDM_OFFSET + phys);
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
        // Walk result is the exact phys (see read path note above).
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *dst = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
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
