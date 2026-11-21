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

/// @file tcb_control.cpp
/// @brief F4 TCB control gate implementation (issue #557, INV-ADM-12/15).
/// Pure/read-only apart from the CNode leaf take; no scheduler lock.

#include <kernel/task/tcb_control.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/tcb_cap.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/task/boot_sdf.hpp> // kPdRoleCount tripwire (INV-ADM-15c)
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel::tcb_control {

// INV-ADM-15(c): the mem-mgr/supervisor identity is a role-table row
// until M4/F10.  The moment such a row exists this predicate must be
// revisited in the same commit — the tripwire forces that review.
// F11 (#564) revisit, recorded here: PROC_MGR is not the mem-mgr (no
// Untyped possession, no SUPERVISOR scope), adds no new exclusion, and
// its CREATOR holds are lifecycle tracking, not stop power (INV-PROC-6:
// the PD never calls this gate as caller).
// #194 revisit, recorded here: TIMER_SRV is message-only (zero-cap SDF
// row, no Untyped possession, no SUPERVISOR scope) and adds no new
// exclusion either — the gate stays property-based (pid cell + Untyped
// possession), never a role list.
static_assert(
    static_cast<uint64_t>(sdf::PdRole::TIMER_SRV) + 1 == sdf::kPdRoleCount,
    "INV-ADM-15(c): PdRole grew — revisit tcb_control_role_protected");

// INV-ADM-15(c), second anchor: the count literal itself (15 today).
// A row addition moves this number and breaks the build here too.
static_assert(sdf::kPdRoleCount == 15,
              "INV-ADM-15(c): kPdRoleCount moved — revisit the gate");

// File-static mem-mgr pid cell (M4/F10 hook).  0 today: no user-space
// mem-mgr exists (memory is kernel-internal until M4/F10 #563), and 0
// matches no task id.  __atomic access, no lock, no config key.
static uint64_t s_mem_mgr_pid = 0;

uint64_t tcb_control_mem_mgr_pid() noexcept {
    return __atomic_load_n(&s_mem_mgr_pid, __ATOMIC_ACQUIRE);
}

void tcb_control_set_mem_mgr_pid(uint64_t pid) noexcept {
    __atomic_store_n(&s_mem_mgr_pid, pid, __ATOMIC_RELEASE);
}

TcbControlResolved tcb_control_resolve(TaskControlBlock &caller,
                                       uint64_t handle) noexcept {
    TcbControlResolved out{};
    caller.ensure_cspace();
    cap::CNode *cspace = caller.get_cspace();
    if (cspace == nullptr) {
        out.err = errors::TCBCTL_ERR_BAD_HANDLE;
        return out;
    }
    KernelObject *obj = cap::lookup(cspace, handle, cap::CapType::TCB,
                                    cap::CapRights::WRITE);
    if (obj == nullptr) {
        out.err = errors::TCBCTL_ERR_BAD_HANDLE;
        return out;
    }
    auto *cap = static_cast<cap::TcbCap *>(obj);
    const bool tagged = (cap->tag == cap::TcbCap::kTcbCapTag);
    out.cap.scope = cap->scope;
    out.cap.target_id = cap->target_id;
    out.cap.target_gen = cap->target_gen;
    out.cap.grantee_id = cap->grantee_id;
    out.cap.grantee_gen = cap->grantee_gen;
    obj->release();
    // The tag keeps the TCB-type invariant checked instead of assumed:
    // a future TCB-typed object (carved TCB, M12/M14) must never be
    // mistaken for a TcbCap.
    if (!tagged) {
        out.err = errors::TCBCTL_ERR_BAD_HANDLE;
        return out;
    }
    if (!Scheduler::validate_id(out.cap.target_id)) {
        out.err = errors::TCBCTL_ERR_BAD_HANDLE;
        return out;
    }
    auto *target = Scheduler::find_task(out.cap.target_id);
    if (target == nullptr ||
        !TaskControlBlock::is_valid(target)) {
        out.err = errors::TCBCTL_ERR_BAD_HANDLE;
        return out;
    }
    // Generation equality: ABA guard on TCB slot reuse (ids are never
    // reused in-boot today; the check holds regardless).
    if (target->generation != out.cap.target_gen) {
        out.err = errors::TCBCTL_ERR_STALE_TARGET;
        return out;
    }
    if (target->state == TaskState::TERMINATED ||
        target->state == TaskState::REAPED) {
        out.err = errors::TCBCTL_ERR_DEAD;
        return out;
    }
    out.target = target;
    out.err = errors::TCBCTL_ERR_OK;
    return out;
}

errors::TcbCtlError
tcb_control_authorize(const TaskControlBlock &caller,
                      const TaskControlBlock &target,
                      const TcbControlAuthority &cap) noexcept {
    // INV-ADM-12, evaluated in order; every refusal is typed with ZERO
    // side effects.
    // 1. Self-stop is never allowed (a task cannot park itself through
    //    the cap gate — EXIT/sleep are the self-stop paths).
    if (caller.id == target.id) {
        return errors::TCBCTL_ERR_SELF;
    }
    // 2. Kernel tasks (idle, monitor, daemons, harness) are never
    //    controlled through this gate.
    if (!target.is_user_) {
        return errors::TCBCTL_ERR_KERNEL_TASK;
    }
    // 3. The idle task is never controlled.
    if (Scheduler::is_idle_task(&target)) {
        return errors::TCBCTL_ERR_IDLE;
    }
    // 4. Protected roles (never mem-mgr) refuse before any relation
    //    branch is consulted.
    if (tcb_control_role_protected(target)) {
        return errors::TCBCTL_ERR_PROTECTED_ROLE;
    }
    // 5. CREATOR / Q9 client-stop branch: the kernel-assigned parent_id
    //    (set at creation only, cleared on orphan/reap) is NOT
    //    caller-writable, so this branch cannot be forged and fails
    //    closed once the relation ends.
    if (target.parent_id == caller.id) {
        return errors::TCBCTL_ERR_OK;
    }
    // 6. SUPERVISOR / Q9 server-stop branch (the explicit override):
    //    ALLOW only when the resolved TcbCap carries SUPERVISOR scope
    //    AND binds this exact caller (grantee_id + grantee_gen).
    //    Copy/grant preserve scope and grantee, so a delegate cannot
    //    exercise it.  With no syscall producer (unchanged F4 fact) and
    //    no compose mint until M5/M12, this branch is deny-all in
    //    production and provably so (fail closed).
    //    Explicitly NOT authority: DeathNotify::is_watching (its slots
    //    are self-grantable) and raw WdogCap possession (watchdog
    //    authority only).
    if (cap.scope ==
            static_cast<uint8_t>(cap::TcbCap::Authority::SUPERVISOR) &&
        cap.grantee_id == caller.id &&
        cap.grantee_gen == caller.generation) {
        // The cap must name this exact target (binding, not ambient).
        if (cap.target_id == target.id &&
            cap.target_gen == target.generation) {
            return errors::TCBCTL_ERR_OK;
        }
    }
    // 7. Q9 cross-subtree attempts fail closed with a typed error.
    return errors::TCBCTL_ERR_NO_AUTHORITY;
}

bool tcb_control_role_protected(const TaskControlBlock &target) noexcept {
    // Layer (a): registered mem-mgr pid cell.  0 today (truthful: no
    // user-space mem-mgr exists) and 0 matches nothing.
    const uint64_t mem_mgr = tcb_control_mem_mgr_pid();
    if (mem_mgr != 0 && target.id == mem_mgr) {
        return true;
    }
    // Layer (b): Untyped possession — the spec's defining property of
    // the mem-mgr ("mem-mgr PD owns all free memory as Untyped",
    // §6.1).  Bounded O(CONFIG_CSLOT_COUNT)=O(64) walk under the CNode
    // leaf lock (same take class as cap::lookup); runs in AUTHORITY
    // (syscall) context only, never in an ISR or tick path.  When M4
    // hands the root Untyped to a user PD, that PD becomes
    // unsuspendable AUTOMATICALLY, with no table to remember.
    if (target.is_user_ && target.get_cspace() != nullptr &&
        cap::cnode_holds_type(target.get_cspace(),
                              cap::CapType::Untyped)) {
        return true;
    }
    // Layer (c): the static_assert tripwire above breaks the build the
    // moment a mem-mgr/supervisor role row is added.
    return false;
}

} // namespace kernel::tcb_control
