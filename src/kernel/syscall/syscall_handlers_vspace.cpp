/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file syscall_handlers_vspace.cpp
/// @brief F5 VSPACE_MAP syscall (issue #558): link a carved PageTable
///        object at a 9-bit-component path under a carved VSpace root.
///        Four-arg form only.  Every path is resolve -> validate ->
///        authorize -> mechanism, every return is SYS_ERR_OK or a typed
///        VSPACE_* code via the 1100-block; no raw -1.  Allocates
///        nothing (INV-VS-2); bounded (arch depth, 32-entry registry,
///        one 8-byte store); never records a derivation edge for a table
///        link (uses-not-caps).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/carve_binding.hpp>
#include <kernel/cap/carved_object.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/frame_foreign_map.hpp>
#include <kernel/cap/tcb_cap.hpp>
#include <kernel/cap/vspace_errors.hpp>
#include <kernel/cap/vspace_map.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/hal/context_id.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/nexios_config.h>
#include <lib/scope_guard.hpp>
#include <constants.hpp>

#if defined(CONFIG_DEBUG)
extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);
#endif

namespace kernel {

namespace {

/// @brief Resolves the current task's root CNode (mirrors
///        syscall_handlers_shm.cpp::shm_current_cspace).
cap::CNode *vspace_current_cspace() {
    auto *task = Scheduler::current_task();
    if (!task)
        return nullptr;
    task->ensure_cspace();
    return task->get_cspace();
}

/// @brief Map a VspaceError to the syscall return encoding: 0 on OK,
///        else -(1100 + code) via from_vspace_error.  Zero side effects.
uint64_t vspace_return(errors::VspaceError err) noexcept {
    if (err == errors::VSPACE_ERR_OK)
        return static_cast<uint64_t>(errors::SYS_ERR_OK);
    return static_cast<uint64_t>(
        -errors::from_vspace_error(static_cast<uint64_t>(err)));
}

} // namespace

uint64_t Syscall::sys_vspace_map(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                 uint64_t arg3, uint64_t *) {
    // Truncation-trap guard (LEARNINGS #337): arg3 narrows to uint8_t
    // for the mechanism.  Reject > 255 BEFORE the narrowing — after it,
    // 256 would masquerade as a valid length 0 re-check downstream.
    if (arg3 > 0xFFu)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    cap::CNode *cs = vspace_current_cspace();
    if (!cs)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Capability-gated: WRITE on the VSpace is the authority to reshape
    // the tree; READ on the PageTable is the authority to install it.
    // Q9 is N/A to N4 (subtree rule governs task control, not address
    // spaces) but cap-gating is still required — no ambient authority.
    KernelObject *vs_obj = cap::lookup(cs, arg0, cap::CapType::VSpace,
                                       cap::CapRights::WRITE);
    if (!vs_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto vs_release = ScopeGuard([&]() { vs_obj->release(); });
    KernelObject *pt_obj = cap::lookup(cs, arg1, cap::CapType::PageTable,
                                       cap::CapRights::READ);
    if (!pt_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto pt_release = ScopeGuard([&]() { pt_obj->release(); });
    // Stale-slot guard (F4 S1(a) pattern): a static_cast from a lookup
    // result must re-verify the wrapping discriminator and the kind.
    auto *vs = static_cast<cap::CarvedObjectCap *>(vs_obj);
    auto *pt = static_cast<cap::CarvedObjectCap *>(pt_obj);
    if (vs->tag != cap::CarvedObjectCap::kCarvedObjectTag ||
        vs->kind != cap::CapType::VSpace ||
        pt->tag != cap::CarvedObjectCap::kCarvedObjectTag ||
        pt->kind != cap::CapType::PageTable)
        return vspace_return(errors::VSPACE_ERR_BAD_OBJECT);
    // A kernel-owned page must never become user-visible hierarchy
    // (the FrameUserMap !is_user refusal is the precedent).
    if (vs->is_user != pt->is_user)
        return vspace_return(errors::VSPACE_ERR_OWNERSHIP_MISMATCH);
    const errors::VspaceError link_err = cap::VSpaceMap::link(
        vs->phys, pt->phys, arg2, static_cast<uint8_t>(arg3));
#if defined(CONFIG_DEBUG)
    // Diagnostic only (no control-flow divergence): name the refused
    // link on the serial console so a failed test names its reason.
    if (link_err != errors::VSPACE_ERR_OK) {
        debug_write("[vspace_map] link refused err=0x");
        debug_write_hex(static_cast<uint64_t>(link_err));
        debug_write("\n");
    }
#endif
    return vspace_return(link_err);
}

uint64_t Syscall::sys_vspace_frame_map(uint64_t arg0, uint64_t arg1,
                                       uint64_t arg2, uint64_t arg3,
                                       uint64_t *) {
    // Truncation-trap guard (LEARNINGS #337): the flag word narrows
    // below — reject unknown bits BEFORE any narrowing or masking, and
    // zero handles before any lookup (a zero handle must never alias
    // slot 0).
    if (arg0 == 0 || arg1 == 0)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    if ((arg3 & ~0x3ULL) != 0)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    const bool executable = (arg3 & 0x1ULL) != 0;
    const bool writable = (arg3 & 0x2ULL) == 0;
    cap::CNode *cs = vspace_current_cspace();
    if (!cs)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Capability-gated: WRITE on the VSpace is the authority to reshape
    // the tree; READ on the FrameCap is the authority to install it.
    KernelObject *vs_obj = cap::lookup(cs, arg0, cap::CapType::VSpace,
                                       cap::CapRights::WRITE);
    if (!vs_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto vs_release = ScopeGuard([&]() { vs_obj->release(); });
    KernelObject *fc_obj = cap::lookup(cs, arg1, cap::CapType::Frame,
                                       cap::CapRights::READ);
    if (!fc_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto fc_release = ScopeGuard([&]() { fc_obj->release(); });
    // Stale-slot guard (F4 S1(a) pattern): a static_cast from a lookup
    // result must re-verify the wrapping discriminator and the kind.
    auto *vs = static_cast<cap::CarvedObjectCap *>(vs_obj);
    auto *fc = static_cast<cap::FrameCap *>(fc_obj);
    if (vs->tag != cap::CarvedObjectCap::kCarvedObjectTag ||
        vs->kind != cap::CapType::VSpace || vs->phys == 0)
        return vspace_return(errors::VSPACE_ERR_BAD_OBJECT);
    // Carved-kind gate: foreign maps land only in user-owned carved
    // roots — a carved VSpace bound to a non-carved task would take the
    // CLONE teardown arm where free_user_pages frees FrameCap-owned
    // leaves (the double-free shape); the kind check closes it.
    if (!vs->is_user)
        return vspace_return(errors::VSPACE_ERR_TARGET_REJECTED);
    // A kernel-backed frame is never user-mappable (the FrameUserMap
    // !is_user precedent): mapping kernel physical memory into a user VA
    // window is a privilege escalation even if such a cap ever reached a
    // user CNode.  The lookup's acquire() already refused revoked caps.
    if (!fc->is_user || fc->revoked())
        return vspace_return(errors::VSPACE_ERR_BAD_OBJECT);
    // The caller's own active space keeps the SYS_FRAME_MAP path (fixed
    // window); a foreign root that is already activated is immutable —
    // post-activation inserts would escape the activation-time
    // validate_subtree guarantee (mirror of the link() ACTIVATED bar).
    if (vs->phys == VMM::current_root_phys())
        return vspace_return(errors::VSPACE_ERR_ACTIVE_SPACE);
    if (cap::carve_root_is_activated(vs->phys))
        return vspace_return(errors::VSPACE_ERR_ACTIVATED);
    // VA window rule: page-aligned, inside the user limit, no wrap; the
    // count comes from the pinned cap (lookup pin held).
    if (fc->count == 0)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    if (fc->count > (0xFFFFFFFFFFFFFFFFULL / arch::PAGE_SIZE))
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    const uint64_t bytes = fc->count * arch::PAGE_SIZE;
    if (arg2 == 0 || (arg2 & (arch::PAGE_SIZE - 1u)) != 0 ||
        arg2 >= CONFIG_USER_SPACE_LIMIT)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    if (arg2 + bytes < arg2 || arg2 + bytes > CONFIG_USER_SPACE_LIMIT)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    // Unmapped-window probe: map_frame_from_cap_err's rollback contract
    // requires an unmapped target (it clears every leaf it installed and
    // cannot distinguish a pre-existing live mapping).
    for (uint64_t page_idx = 0; page_idx < fc->count; ++page_idx) {
        if (VMM::virt_to_phys_in_pml4(
                arg2 + page_idx * arch::PAGE_SIZE, vs->phys) != 0)
            return vspace_return(errors::VSPACE_ERR_SLOT_OCCUPIED);
    }
    // Residual in-flight overlap (a claimed-but-unmapped slot from a
    // concurrent map on the same root) folds into REGISTRY_FULL here —
    // fail-closed either way; the pre-probe above is the precise
    // SLOT_OCCUPIED face for committed mappings.
    if (cap::ForeignFrameMap::map(*fc, vs->phys, arg2, executable,
                                  writable) != errors::VMM_ERR_OK)
        return vspace_return(errors::VSPACE_ERR_REGISTRY_FULL);
    return vspace_return(errors::VSPACE_ERR_OK);
}

uint64_t Syscall::sys_vspace_activate(uint64_t arg0, uint64_t arg1,
                                      uint64_t arg2, uint64_t arg3,
                                      uint64_t *) {
    // Unpublished-composer-child-only surface (INV-VS-3-prime(b),
    // INV-CARVE-6): the bare surface can never retarget a live task.
    // Flags/reserved must be zero — no silent ignore (S3-b face).
    if (arg2 != 0 || arg3 != 0)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    if (arg0 == 0 || arg1 == 0)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    cap::CNode *cs = vspace_current_cspace();
    if (!cs)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    KernelObject *vs_obj = cap::lookup(cs, arg0, cap::CapType::VSpace,
                                       cap::CapRights::WRITE);
    if (!vs_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto vs_release = ScopeGuard([&]() { vs_obj->release(); });
    // The TcbCap is the only authority that may name an activation
    // target (READ suffices — activation installs, never configures).
    KernelObject *tc_obj = cap::lookup(cs, arg1, cap::CapType::TCB,
                                       cap::CapRights::READ);
    if (!tc_obj)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    auto tc_release = ScopeGuard([&]() { tc_obj->release(); });
    auto *vs = static_cast<cap::CarvedObjectCap *>(vs_obj);
    auto *tcb_cap = static_cast<cap::TcbCap *>(tc_obj);
    if (vs->tag != cap::CarvedObjectCap::kCarvedObjectTag ||
        vs->kind != cap::CapType::VSpace || vs->phys == 0)
        return vspace_return(errors::VSPACE_ERR_BAD_OBJECT);
    if (tcb_cap->tag != cap::TcbCap::kTcbCapTag)
        return vspace_return(errors::VSPACE_ERR_BAD_HANDLE);
    TaskControlBlock *caller = Scheduler::current_task();
    if (caller == nullptr)
        return vspace_return(errors::VSPACE_ERR_INVALID_ARGS);
    // Resolve the target (generation-checked, live only).  Resolution
    // failures name no unpublished idle task — TARGET_REJECTED, never a
    // handle face (the handle itself resolved; the target is unusable).
    if (!Scheduler::validate_id(tcb_cap->target_id))
        return vspace_return(errors::VSPACE_ERR_TARGET_REJECTED);
    TaskControlBlock *target =
        Scheduler::find_task(tcb_cap->target_id);
    if (target == nullptr || !TaskControlBlock::is_valid(target) ||
        target->generation != tcb_cap->target_gen)
        return vspace_return(errors::VSPACE_ERR_TARGET_REJECTED);
    // Unpublished-target gate: the target never reached any dispatcher —
    // the sole user-task publication path is Scheduler::add_task_err
    // (INV-CARVE-3: user tasks publish only through add_task_err; the
    // non-add_task_err publishers carry kernel tasks, excluded by
    // is_user_), so no CPU's dispatcher can reach the target and no
    // scheduler_lock_ exclusion is needed on the gate+install sequence.  The page_table_
    // == 0 && pcid_ == 0 pair also closes the pcid-incumbent leak (a
    // fresh child has no ID to free) and doubles as thread-confusion
    // exclusion (threads carry a non-zero shared root).
    const bool unpublished_gate =
        (target->magic == TaskControlBlock::TCB_MAGIC) &&
        (target->state != TaskState::TERMINATED) &&
        (target->state != TaskState::REAPED) && (target != caller) &&
        target->is_user_ && (target->state != TaskState::RUNNING) &&
        (target->get_page_table() == 0) && (target->pcid_ == 0);
    if (!unpublished_gate) {
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 23,
                            log::LogSeverity::ERROR,
                            "Carved root activation refused", target->id);
        return vspace_return(errors::VSPACE_ERR_TARGET_REJECTED);
    }
    // Issue #564 audit (Stage C code r1, S2): refuse an already-activated
    // root (INV-VS-3-prime(b), ACTIVATED) and the caller's live root
    // (ACTIVE_SPACE, retained) with their typed faces BEFORE
    // merge_kernel_half — the merge writes kernel-half entries and must
    // never run on a live address space, and a second activation of one
    // root would alias two TCBs onto one address space.  Both refusals
    // are side-effect free (no slot, no pcid_, no page_table_ write) and
    // carry no +22 (the +22 rule names TARGET_REJECTED/SUBTREE_INVALID
    // only).  validate_subtree alone cannot close this: it never consults
    // the activation flag and probe_publish_locked is idempotent.
    if (vs->phys == VMM::current_root_phys())
        return vspace_return(errors::VSPACE_ERR_ACTIVE_SPACE);
    if (cap::carve_root_is_activated(vs->phys))
        return vspace_return(errors::VSPACE_ERR_ACTIVATED);
    // Kernel-half converge on the bare path (replicates composer step 5):
    // link semantics, zero allocation — a kernel-half-less root can never
    // reach first dispatch.  Merge is infallible for valid distinct roots
    // (false only on null/same roots, unconstructible through valid
    // caps); fail closed on the impossible.
    if (!VMM::merge_kernel_half(VMM::get_kernel_pml4(), vs->phys)) {
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 23,
                            log::LogSeverity::ERROR,
                            "Carved root activation refused", target->id);
        return vspace_return(errors::VSPACE_ERR_SUBTREE_INVALID);
    }
    VMM::assert_kernel_half_converged(vs->phys);
    // Activation-time subtree re-validation (top-down through the
    // per-arch codec) before any install.
    const errors::VspaceError valid =
        cap::CarveBinding::validate_subtree(vs->phys);
    if (valid != errors::VSPACE_ERR_OK) {
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 23,
                            log::LogSeverity::ERROR,
                            "Carved root activation refused", target->id);
        return vspace_return(valid);
    }
    // Binding: begin on the target (absent — a second activation of the
    // same target fails the gate above first, so begin-failure here
    // names a composing sibling or exhaustion), bind the root pin, then
    // activate (re-validates internally).  Every refusal is side-effect
    // free: no slot, no pcid_, no page_table_ write.
    const int slot =
        cap::CarveBinding::begin(target->id, target->generation);
    if (slot < 0) {
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 23,
                            log::LogSeverity::ERROR,
                            "Carved root activation refused", target->id);
        return vspace_return(errors::VSPACE_ERR_TARGET_REJECTED);
    }
    if (!cap::CarveBinding::bind_root(slot, vs)) {
        cap::CarveBinding::abandon(slot);
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 23,
                            log::LogSeverity::ERROR,
                            "Carved root activation refused", target->id);
        return vspace_return(errors::VSPACE_ERR_TARGET_REJECTED);
    }
    const errors::VspaceError act =
        cap::CarveBinding::activate_err(slot, vs->phys);
    if (act != errors::VSPACE_ERR_OK) {
        cap::CarveBinding::abandon(slot);
        if (act == errors::VSPACE_ERR_TARGET_REJECTED ||
            act == errors::VSPACE_ERR_SUBTREE_INVALID) {
            log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                                log::kDmesgBase_TASK + 23,
                                log::LogSeverity::ERROR,
                                "Carved root activation refused",
                                target->id);
        }
#if defined(CONFIG_DEBUG)
        debug_write("[vspace_activate] refused err=0x");
        debug_write_hex(static_cast<uint64_t>(act));
        debug_write("\n");
#endif
        return vspace_return(act);
    }
    // Install last (INV-CARVE-6: no register write, no flush — the target
    // was never live).  context_id_alloc() happens here, after every
    // fallible step, so no refusal leg leaks a context ID.
    target->install_carved_root(vs->phys, arch::context_id_alloc());
    return vspace_return(errors::VSPACE_ERR_OK);
}

} // namespace kernel
