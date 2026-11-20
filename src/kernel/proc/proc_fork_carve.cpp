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

/// @file proc_fork_carve.cpp
/// @brief Carve-composed fork composer (issue #564, F11 Stage C): the
///        fork child is built from retype + VSPACE_MAP + VSPACE_FRAME_MAP
///        + VSPACE_ACTIVATE, with no kernel deep copy (INV-CARVE-2).
///        Entire TU under #if CONFIG_PROC_MGR_CARVE_FORK (empty when 0;
///        OFF identity is the statement-identical clone).
///
///        Composition order is top-down and total (INV-CARVE-1): retype
///        (TCB, VSpace, PageTables, Frames) -> VSPACE_MAP links
///        intermediates root-first and leaves last -> VSPACE_FRAME_MAP
///        populates the leaves -> activation, last.  The child is not in
///        all_tasks_ until Scheduler::add_task_err (INV-CARVE-3, the sole
///        user-task publication path); every earlier failure is one
///        TaskControlBlock::destroy(child) plus the rollback below —
///        never a panic, never a published partial child.  The kernel
///        stack and kslot window keep their PMM/kslot allocation: they
///        are not address-space objects (INV-CARVE-2).

#if CONFIG_PROC_MGR_CARVE_FORK

#include <kernel/proc/proc_fork_carve.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/carve_binding.hpp>
#include <kernel/cap/carved_object.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/frame_foreign_map.hpp>
#include <kernel/cap/object_sizing.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/cap/vspace_map.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/hal/context_id.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/nexios_config.h>
#include <lib/scope_guard.hpp>
#include <constants.hpp>

namespace kernel::proc {

namespace {

// Failure taxonomy: pre-adoption guard failures (GUARD, REGION_BOUND at
// the enumerate step) return a typed refusal with zero side effects — no
// child, no destroy, no rollback, no +22.  Every later failure unwinds
// scratch, destroys the child (which drains the binding by INV-CARVE-4)
// and pushes +22.  VMM errors map typed at the call site (CODING_STYLE
// §5.1: reachable exhaustion returns errors, never an ENSURE).
enum class CarveFailure : uint8_t {
    NONE = 0,
    GUARD = 1,        ///< pre-adoption guard refusal (steps 1-2)
    REGION_BOUND = 2, ///< enumerate empty or truncated
    SCRATCH = 3,      ///< CNode scratch install failure
    RETYPE = 4,       ///< retype refusal (CapError carried)
    LINK = 5,         ///< table link refusal (VspaceError carried)
    MAP = 6,          ///< foreign frame map refusal (VmmError carried)
    KSTACK = 7,       ///< kernel stack allocation failure
    ACTIVATE = 8,     ///< activation refusal (VspaceError carried)
};

struct CarveStatus {
    CarveFailure kind = CarveFailure::NONE;
    errors::CapError cap_err = errors::CAP_ERR_OK;
    errors::VspaceError vspace_err = errors::VSPACE_ERR_OK;
    errors::VmmError vmm_err = errors::VMM_ERR_OK;
};

bool carve_ok(const CarveStatus &status) noexcept {
    return status.kind == CarveFailure::NONE;
}

/// @brief One retyped scratch object with its composer pin (lookup pin;
///        the CNode slot is removed immediately so the parent can never
///        reach the child's storage).
struct CarvedLocal {
    KernelObject *obj = nullptr;
    uint64_t phys = 0;
};

/// @brief Carve one object over a fresh Untyped region (exact size, no
///        child remainder): create -> install in the caller's CNode ->
///        retype -> pin -> remove both slots -> release the creator.
///        On success the caller owns exactly one pin; on failure nothing
///        is held (the region is returned to PMM).
CarveStatus carve_object(cap::CNode *cspace, cap::CapType kind,
                         size_t bytes, bool is_user,
                         CarvedLocal &out) noexcept {
    out.obj = nullptr;
    out.phys = 0;
    cap::UntypedMem *ut = cap::UntypedMem::create(bytes, is_user);
    if (ut == nullptr) {
        CarveStatus status{};
        status.kind = CarveFailure::RETYPE;
        status.cap_err = errors::CAP_ERR_OBJECT_EXHAUSTED;
        return status;
    }
    uint32_t ut_gen = 0;
    const int ut_idx =
        cspace->install(ut, cap::CapType::Untyped, cap::CapRights::WRITE,
                        &ut_gen);
    if (ut_idx < 0) {
        ut->release(); // creator ref; the region returns to PMM
        CarveStatus status{};
        status.kind = CarveFailure::SCRATCH;
        return status;
    }
    const uint64_t ut_handle =
        cap::encode_user_handle(cspace->cspace_id,
                                static_cast<uint32_t>(ut_idx), ut_gen);
    uint32_t target_gen = 0;
    errors::CapError retype_err = errors::CAP_ERR_OK;
    const int target_idx =
        cap::retype(cspace, ut_handle, kind, bytes, cap::CapRights::WRITE,
                    &target_gen, &retype_err);
    if (target_idx < 0) {
        cspace->remove(static_cast<uint32_t>(ut_idx));
        ut->release(); // creator ref; spent-or-valid, dispose is exact
        CarveStatus status{};
        status.kind = CarveFailure::RETYPE;
        status.cap_err = retype_err;
        return status;
    }
    const uint64_t target_handle = cap::encode_user_handle(
        cspace->cspace_id, static_cast<uint32_t>(target_idx), target_gen);
    KernelObject *obj =
        cap::lookup(cspace, target_handle, kind, cap::CapRights::WRITE);
    cspace->remove(static_cast<uint32_t>(target_idx));
    cspace->remove(static_cast<uint32_t>(ut_idx));
    ut->release(); // creator ref; the spent Untyped owns nothing
    if (obj == nullptr) {
        // Unreachable through valid caps (lookup pins a live install);
        // fail closed rather than assume.
        CarveStatus status{};
        status.kind = CarveFailure::RETYPE;
        status.cap_err = errors::CAP_ERR_INSTALL_FAILED;
        return status;
    }
    uint64_t phys = 0;
    if (kind == cap::CapType::Frame)
        phys = static_cast<cap::FrameCap *>(obj)->phys;
    else
        phys = static_cast<cap::CarvedObjectCap *>(obj)->phys;
    out.obj = obj;
    out.phys = phys;
    return CarveStatus{};
}

/// @brief Link-or-fail: maps every VspaceError to the composer failure
///        at the call site (typed map, never an ENSURE on a link path).
CarveStatus link_or_fail(uint64_t root_phys, uint64_t table_phys,
                         uint64_t path, uint8_t path_len) noexcept {
    const errors::VspaceError err =
        cap::VSpaceMap::link(root_phys, table_phys, path, path_len);
    if (err == errors::VSPACE_ERR_OK)
        return CarveStatus{};
    CarveStatus status{};
    status.kind = CarveFailure::LINK;
    status.vspace_err = err;
    return status;
}

/// @brief One linked table with its composer pin (the binding takes its
///        own pin at bind_table; the composer pin drops at unwind).
struct TableLocal {
    uint64_t path = 0;
    uint8_t path_len = 0;
    uint8_t level = 0;
    uint64_t phys = 0;
    cap::CarvedObjectCap *cap = nullptr;
};

/// @brief Arch-neutral child index vector for @p va: component l rides
///        bits [12 + 9*(levels-1-l) .. +8] (4-level x86_64/aarch64 and
///        3-level Sv39 riscv64 share the 9-bit level geometry — no arch
///        branch, INV-CARVE-9).
void child_indices(uint64_t va, unsigned levels,
                   uint64_t idx_out[4]) noexcept {
    for (unsigned level = 0; level < levels; ++level) {
        const unsigned shift = 12u + 9u * (levels - 1u - level);
        idx_out[level] = (va >> shift) & 0x1FFu;
    }
    for (unsigned level = levels; level < 4u; ++level)
        idx_out[level] = 0;
}

/// @brief Register-frame push for the carved child: DUPLICATE of the
///        three arch blocks in TaskControlBlock::clone (task.cpp) with
///        provenance.  clone stays statement-identical as the knob-OFF
///        comparator — extraction would break comparator integrity (a
///        diff-checker step verifies both directions stay in sync).
void push_clone_frame(TaskControlBlock *child, uint64_t *regs) noexcept {
#if defined(CONFIG_ARCH_X86_64)
    // Build register frame on child's kernel stack
    // Layout matches isr_common push order (high to low):
    // SS, RSP, RFLAGS, CS, RIP, error, vector, r15..r8, rbp, rdi, rsi, rdx,
    // rcx, rbx, rax
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint64_t *stack = reinterpret_cast<uint64_t *>(child->kernel_stack_top);
    *--stack = regs[21]; // SS
    *--stack = regs[20]; // RSP
    *--stack = regs[19]; // RFLAGS
    *--stack = regs[18]; // CS
    *--stack = regs[17]; // RIP
    *--stack = regs[16]; // error_code
    *--stack = regs[15]; // vector
    *--stack = regs[14]; // r15
    *--stack = regs[13]; // r14
    *--stack = regs[12]; // r13
    *--stack = regs[11]; // r12
    *--stack = regs[10]; // r11
    *--stack = regs[9];  // r10
    *--stack = regs[8];  // r9
    *--stack = regs[7];  // r8
    *--stack = regs[6];  // rbp
    *--stack = regs[5];  // rdi
    *--stack = regs[4];  // rsi
    *--stack = regs[3];  // rdx
    *--stack = regs[2];  // rcx
    *--stack = regs[1];  // rbx
    *--stack = 0;        // rax = 0 (child return value)
    child->context.rsp = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_AARCH64)
    // Issue #104: indices follow the vectors.S save-area layout (x0-x30 @
    // regs[0..30], SP_EL0 @ regs[31], ELR @ regs[32], SPSR @ regs[33]) —
    // NOT the x86 map (regs[17/19/20] are user x17/x19/x20 here).  Push
    // DESCENDING (like x86) so memory order matches the layout (lowest =
    // X0); ascending would mirror x1-x30.  X0 reads back zero so the child
    // observes fork-return-0 (x86 zeroes rax and riscv zeroes A0 the same
    // way).
    uint64_t *stack = reinterpret_cast<uint64_t *>(child->kernel_stack_top);
    // aarch64 exception frame: padding, SPSR, ELR, SP_EL0, X0-X30
    *--stack = 0;        // padding
    *--stack = 0;        // padding
    *--stack = regs[33]; // SPSR_EL1
    *--stack = regs[32]; // ELR_EL1
    *--stack = regs[31]; // SP_EL0
    for (int reg_idx = 30; reg_idx >= 0; --reg_idx)
        *--stack = regs[reg_idx];
    stack[0] = 0; // X0 = 0 (fork returns 0 in child)
    child->context.sp_el0 = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_RISCV64)
    // Copy trap frame from parent (regs matches syscall_entry.S save area
    // layout: [0..30]=X1-X31, [31]=SEPC, [32]=SSTATUS, [33]=SCAUSE, [34]=STVAL)
    uint64_t *stack = reinterpret_cast<uint64_t *>(child->kernel_stack_top);
    stack -= 37;
    for (int reg_idx = 0; reg_idx < 37; ++reg_idx)
        stack[reg_idx] = regs[reg_idx];
    stack[9] = 0; // X10/A0 = 0 (fork returns 0 in child)
    child->context.sp = reinterpret_cast<uint64_t>(stack);
#endif
}

} // namespace

TaskControlBlock *carve_fork(uint64_t *regs) noexcept {
    if (regs == nullptr)
        return nullptr;
    // Step 1 — guards (the #400 validate-before-allocate discipline):
    // zero side effects.  Pre-adoption failures return a typed refusal
    // (nullptr at the ProcSvc face) with no child, no destroy, no
    // rollback, no +22.
    TaskControlBlock *parent = Scheduler::current_task();
    if (parent == nullptr || !parent->is_user_ ||
        parent->get_page_table() == 0)
        return nullptr;
    parent->ensure_cspace();
    cap::CNode *cs = parent->get_cspace();
    if (cs == nullptr)
        return nullptr;
    const uint64_t parent_root = parent->get_page_table();

    // Step 2 — region enumeration (bounded, allocation-free, read-only).
    VMM::UserRegion regions[CONFIG_CARVE_MAX_REGIONS];
    const size_t found = VMM::enumerate_user_regions(
        parent_root, regions, CONFIG_CARVE_MAX_REGIONS);
    if (found == 0 || found > CONFIG_CARVE_MAX_REGIONS)
        return nullptr; // empty or truncated — fail closed, no +22

    // Composer-local pins (lookup pins; the binding takes its own pins
    // at bind_*).  Released on every failure path below.
    TaskControlBlock *child = nullptr;
    int slot = -1;
    CarvedLocal tcb_local{};
    CarvedLocal root_local{};
    TableLocal tables[CONFIG_CARVE_MAX_TABLES];
    size_t table_total = 0;
    CarvedLocal frames[CONFIG_CARVE_MAX_REGIONS];
    size_t frame_total = 0;
    uint64_t root_phys = 0;
    bool stack_found = false;

    // Post-adoption unwind: release composer pins, destroy the child
    // (which drains the binding by INV-CARVE-4 and, through it, the
    // foreign map) or abandon the slot when no child exists, then +22.
    // Never a panic, never a published partial child.
    auto fail = [&](CarveStatus status) -> TaskControlBlock * {
        // Issue #564 audit (Stage C code r1, S1): the child is destroyed
        // FIRST while the composer pins still keep the carved TCB region
        // alive.  Releasing the TCB pin before destroy() frees the very
        // region destroy() runs on (the begin/bind_tcb_cap failure paths
        // hold no binding pin) — a use-after-free.  destroy() drains the
        // binding (INV-CARVE-4); the composer pins drop after it.  The two
        // pin sets are independent acquire()s, so no double-release.
        if (child != nullptr) {
            TaskControlBlock::destroy(child);
            child = nullptr;
        } else if (slot >= 0) {
            cap::CarveBinding::abandon(slot);
            slot = -1;
        }
        for (size_t table_idx = 0; table_idx < table_total; ++table_idx) {
            if (tables[table_idx].cap != nullptr) {
                tables[table_idx].cap->release();
                tables[table_idx].cap = nullptr;
            }
        }
        for (size_t frame_idx = 0; frame_idx < frame_total; ++frame_idx) {
            if (frames[frame_idx].obj != nullptr) {
                frames[frame_idx].obj->release();
                frames[frame_idx].obj = nullptr;
            }
        }
        if (root_local.obj != nullptr) {
            root_local.obj->release();
            root_local.obj = nullptr;
        }
        if (tcb_local.obj != nullptr) {
            tcb_local.obj->release();
            tcb_local.obj = nullptr;
        }
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 22,
                            log::LogSeverity::WARN,
                            "Carve fork rolled back",
                            static_cast<uint64_t>(status.kind));
        return nullptr;
    };

    // Step 3 — TCB carve (kernel-owned memory) into the composer's own
    // CNode, then out of it: the parent must never reach the child's
    // storage (the pin below is the only reference).
    CarveStatus status =
        carve_object(cs, cap::CapType::TCB, cap::kObjectBytesTcb,
                     /*is_user=*/false, tcb_local);
    if (!carve_ok(status))
        return fail(status);

    // Step 4 — TCB adoption: memset-then-fields, fresh generation,
    // CARVED storage/kind, hierarchy, IPC births.  The fd/cwd/FPU
    // content arrives in step 9; the register frame in step 10.
    child = TaskControlBlock::create_carved(*parent, tcb_local.phys);
    slot = cap::CarveBinding::begin(child->id, child->generation);
    if (slot < 0) {
        CarveStatus begin_status{};
        begin_status.kind = CarveFailure::ACTIVATE;
        begin_status.vspace_err = errors::VSPACE_ERR_REGISTRY_FULL;
        return fail(begin_status);
    }
    if (!cap::CarveBinding::bind_tcb_cap(
            slot, static_cast<cap::CarvedObjectCap *>(tcb_local.obj))) {
        CarveStatus bind_status{};
        bind_status.kind = CarveFailure::ACTIVATE;
        bind_status.vspace_err = errors::VSPACE_ERR_TARGET_REJECTED;
        return fail(bind_status);
    }

    // Step 5 — VSpace root carve (is_user = true) -> bind_root, then the
    // kernel-half merge + convergence assert.  Link semantics with zero
    // allocation — not the deep copy — required on every arch.
    status = carve_object(cs, cap::CapType::VSpace,
                          cap::kObjectBytesVSpace, /*is_user=*/true,
                          root_local);
    if (!carve_ok(status))
        return fail(status);
    root_phys = root_local.phys;
    if (!cap::CarveBinding::bind_root(
            slot, static_cast<cap::CarvedObjectCap *>(root_local.obj))) {
        CarveStatus bind_status{};
        bind_status.kind = CarveFailure::ACTIVATE;
        bind_status.vspace_err = errors::VSPACE_ERR_TARGET_REJECTED;
        return fail(bind_status);
    }
    if (!VMM::merge_kernel_half(VMM::get_kernel_pml4(), root_phys)) {
        CarveStatus merge_status{};
        merge_status.kind = CarveFailure::ACTIVATE;
        merge_status.vspace_err = errors::VSPACE_ERR_SUBTREE_INVALID;
        return fail(merge_status);
    }
    VMM::assert_kernel_half_converged(root_phys);

    // Step 6 — table carve + top-down link: derive the index chain from
    // each region's VA; for level 1 .. levels-2 carve one PageTable per
    // distinct (level, prefix) and link it at path_len = level; then the
    // leaf table at path_len = levels-1 (registry demands ancestors
    // first — walk_parent fails UNMAPPED otherwise).  Total tables are
    // capped (typed fail-closed, never an unbounded walk).
    const unsigned levels = VMM::page_table_levels();
    for (size_t region_idx = 0; region_idx < found; ++region_idx) {
        const VMM::UserRegion &region = regions[region_idx];
        if (region.bytes == 0 ||
            (region.bytes & (arch::PAGE_SIZE - 1u)) != 0 ||
            (region.va & (arch::PAGE_SIZE - 1u)) != 0) {
            CarveStatus bound_status{};
            bound_status.kind = CarveFailure::REGION_BOUND;
            return fail(bound_status);
        }
        if (region.va + region.bytes < region.va ||
            region.va + region.bytes > CONFIG_USER_SPACE_LIMIT) {
            CarveStatus bound_status{};
            bound_status.kind = CarveFailure::REGION_BOUND;
            return fail(bound_status);
        }
        uint64_t comp[4] = {};
        child_indices(region.va, levels, comp);
        for (unsigned link_level = 1; link_level < levels; ++link_level) {
            uint64_t path = 0;
            for (unsigned comp_idx = 0; comp_idx < link_level; ++comp_idx)
                path |= comp[comp_idx] << (9u * comp_idx);
            const auto want_len = static_cast<uint8_t>(link_level);
            bool known = false;
            for (size_t table_idx = 0; table_idx < table_total;
                 ++table_idx) {
                if (tables[table_idx].level == link_level &&
                    tables[table_idx].path_len == want_len &&
                    tables[table_idx].path == path) {
                    known = true;
                    break;
                }
            }
            if (known)
                continue;
            if (table_total >= CONFIG_CARVE_MAX_TABLES) {
                CarveStatus bound_status{};
                bound_status.kind = CarveFailure::LINK;
                bound_status.vspace_err =
                    errors::VSPACE_ERR_REGISTRY_FULL;
                return fail(bound_status);
            }
            CarvedLocal table_local{};
            status = carve_object(cs, cap::CapType::PageTable,
                                  cap::kObjectBytesPageTable,
                                  /*is_user=*/true, table_local);
            if (!carve_ok(status))
                return fail(status);
            status = link_or_fail(root_phys, table_local.phys, path,
                                  want_len);
            if (!carve_ok(status)) {
                table_local.obj->release();
                return fail(status);
            }
            if (!cap::CarveBinding::bind_table(
                    slot,
                    static_cast<cap::CarvedObjectCap *>(table_local.obj))) {
                table_local.obj->release();
                CarveStatus bind_status{};
                bind_status.kind = CarveFailure::ACTIVATE;
                bind_status.vspace_err =
                    errors::VSPACE_ERR_TARGET_REJECTED;
                return fail(bind_status);
            }
            // The binding holds the table alive from here (the link
            // registry stores phys only — INV-VS-8); the composer pin
            // drops below with the rest on unwind, at commit on success.
            tables[table_total].path = path;
            tables[table_total].path_len = want_len;
            tables[table_total].level =
                static_cast<uint8_t>(link_level);
            tables[table_total].phys = table_local.phys;
            tables[table_total].cap =
                static_cast<cap::CarvedObjectCap *>(table_local.obj);
            ++table_total;
        }
    }

    // Step 7 — per region: one contiguous Untyped -> retype Frame ->
    // bind_frame -> foreign map (executable regions install executable
    // first and never pass through NX; read-only regions take the RO
    // pass inside the map) -> bounded per-page HHDM copy with W^X.  A
    // hole in the parent (src == 0) or a non-RAM page leaves the child
    // page unmapped (deep_copy_user_pages' non-RAM skip): the mapped
    // page is punched back out (holes fold-and-ignore, #366 Q2b).
    for (size_t region_idx = 0; region_idx < found; ++region_idx) {
        const VMM::UserRegion &region = regions[region_idx];
        CarvedLocal frame_local{};
        status = carve_object(cs, cap::CapType::Frame, region.bytes,
                              /*is_user=*/true, frame_local);
        if (!carve_ok(status))
            return fail(status);
        auto *frame =
            static_cast<cap::FrameCap *>(frame_local.obj);
        if (!cap::CarveBinding::bind_frame(slot, frame)) {
            frame_local.obj->release();
            CarveStatus bind_status{};
            bind_status.kind = CarveFailure::ACTIVATE;
            bind_status.vspace_err = errors::VSPACE_ERR_TARGET_REJECTED;
            return fail(bind_status);
        }
        // VMM::map_frame_from_cap_err must never return
        // VMM_ERR_PAGE_ALLOC here: a walk that has to allocate proves
        // the tree was not pre-linked (INV-CARVE-2) — the link above
        // installed every table on this region's chain.
        const errors::VmmError map_err = cap::ForeignFrameMap::map(
            *frame, root_phys, region.va, region.executable,
            region.writable);
        if (map_err != errors::VMM_ERR_OK) {
            CarveStatus map_status{};
            map_status.kind = CarveFailure::MAP;
            map_status.vmm_err = map_err;
            return fail(map_status);
        }
        const size_t pages = region.bytes / arch::PAGE_SIZE;
        for (size_t page_idx = 0; page_idx < pages; ++page_idx) {
            const uint64_t page_va =
                region.va + page_idx * arch::PAGE_SIZE;
            const uint64_t src = VMM::virt_to_phys_in_pml4(
                page_va, parent_root);
            const uint64_t dst =
                frame->phys + page_idx * arch::PAGE_SIZE;
            if (src == 0 || !PMM::is_user_page(src)) {
                (void)VMM::unmap_frame_from_cap_err(frame, page_va,
                                                    root_phys);
                continue;
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            __builtin_memcpy(reinterpret_cast<void *>(arch::HHDM_OFFSET +
                                                      dst),
                             // NOLINTNEXTLINE(performance-no-int-to-ptr)
                             reinterpret_cast<const void *>(
                                 arch::HHDM_OFFSET + src),
                             arch::PAGE_SIZE);
        }
        frames[frame_total] = frame_local;
        ++frame_total;
        // Step 8 (stack leg): the stack region is one of the enumerated
        // regions (mem::STACK_VADDR + PAGE_SIZE, parent->user_stack_size_).
        // Containment (not exact match) so a coalesced neighbour run
        // still resolves; the child records the frame phys + offset.
        const uint64_t stack_base =
            mem::STACK_VADDR + arch::PAGE_SIZE;
        if (!stack_found && region.va <= stack_base &&
            stack_base + parent->user_stack_size_ > stack_base &&
            stack_base + parent->user_stack_size_ <=
                region.va + region.bytes) {
            child->user_stack_ =
                frame->phys + (stack_base - region.va);
            child->user_stack_size_ = parent->user_stack_size_;
            stack_found = true;
        }
    }
    if (!stack_found) {
        // A user parent always owns its stack window; a missing run
        // names a corrupt parent image — fail closed.
        CarveStatus stack_status{};
        stack_status.kind = CarveFailure::REGION_BOUND;
        return fail(stack_status);
    }
    // Canary fields ride the deep copy (TEXT/DATA/HEAP sentinels carried
    // the bytes; the fresh stack copy carries the STACK sentinels).
    for (size_t seg_idx = 0;
         seg_idx < TaskControlBlock::CANARY_SEGMENTS; ++seg_idx) {
        child->canary_before[seg_idx] = parent->canary_before[seg_idx];
        child->canary_after[seg_idx] = parent->canary_after[seg_idx];
    }
    child->canary_installed = parent->canary_installed;

    // Step 9 — inheritance, with the shared-vs-copied contract explicit:
    // fd_table copied shallow + vnode_ref_inc per used fd (shared vnode,
    // independent reference — clone:1925-1929 unchanged); cwd copied +
    // vnode_ref_inc (clone:1931-1938 unchanged); FPU state copied with
    // the per-arch flush-before-copy (clone:1949-1979 logic DUPLICATED
    // here with provenance — clone keeps its text); msg_queue/notify/
    // event_group re-created + buffer pool empty + TLS/brk inherited by
    // value in create_carved.  Nothing here is deep-copied: no page
    // tables, no vnodes, no CNode.
    for (size_t fd_idx = 0; fd_idx < vfs::MAX_FDS; ++fd_idx) {
        child->fd_table.fds[fd_idx] = parent->fd_table.fds[fd_idx];
        if (child->fd_table.fds[fd_idx].used &&
            child->fd_table.fds[fd_idx].vnode)
            vfs::vnode_ref_inc(child->fd_table.fds[fd_idx].vnode);
    }
    size_t cwd_len = 0;
    while (parent->cwd[cwd_len] && cwd_len < 255)
        ++cwd_len;
    for (size_t cwd_idx = 0; cwd_idx <= cwd_len; ++cwd_idx)
        child->cwd[cwd_idx] = parent->cwd[cwd_idx];
    child->cwd_vnode = parent->cwd_vnode;
    vfs::vnode_ref_inc(child->cwd_vnode);

#if defined(CONFIG_ARCH_X86_64)
    // Save parent's FPU state if it's currently in this CPU's registers
    // (issue #151: ownership is per-CPU; the cloning CPU's slot decides).
    if (__atomic_load_n(&fpu_owner_own(), __ATOMIC_ACQUIRE) == parent) {
        arch::fxsave(parent->fpu_state);
        ++parent->fpu_state_gen;
    }
#else
    // Issue #403: aarch64/riscv64 switch FP eagerly, and only inside
    // scheduler_on_context_switch. A synchronous fork trap performs no
    // switch, so the parent's live FP regs are still unflushed here;
    // materialize them before the child copy (mirrors the x86 fxsave).
    // The gate mirrors the eager switch (docs/specs/fpu-context.md §7):
    // only user-side FP state is tracked (kernel code carries no FP
    // instructions) and fpu_available() keeps the degraded, FP-absent
    // path from executing save instructions.
    if (parent->is_user_ && arch::fpu_available()) {
        arch::fpu_ensure_enabled();
        arch::fpu_save(parent->fpu_state);
        parent->fpu_used = true;
        ++parent->fpu_state_gen;
    }
#endif
    // Copy FPU state to child (full arch area, not hardcoded 512 —
    // issue #353: NEON needs 528, riscv F/D 272).
    child->fpu_used = parent->fpu_used;
    child->fpu_state_gen = parent->fpu_state_gen;
    if (parent->fpu_used) {
        __builtin_memcpy(child->fpu_state, parent->fpu_state,
                         sizeof(child->fpu_state));
    }

    // Step 10 — kernel stack: unchanged (PMM::alloc_contiguous +
    // alloc_kslot window + the budget reserve/charge, the clone:1981-2052
    // sequence); it is not an address-space object (INV-CARVE-2 states
    // this).  Then the register frame (clone:2054-2112 DUPLICATED with
    // provenance — clone stays statement-identical).
    const size_t stack_pages =
        (TaskControlBlock::STACK_SIZE + 4095) / arch::PAGE_SIZE;
#if CONFIG_MEMORY_BUDGET
    // Issue #328: charge the child's kernel stack against the global
    // budget, symmetric to create().  Fail-closed before any allocation;
    // the teardown below releases exactly these pages (stack_phys_ set
    // after), so no rollback is needed beyond destroy()+22.
    if (!Scheduler::reserve_memory_pages(stack_pages)) {
        CarveStatus budget_status{};
        budget_status.kind = CarveFailure::KSTACK;
        return fail(budget_status);
    }
#endif
    const uint64_t kstack_phys = PMM::alloc_contiguous(stack_pages);
    if (kstack_phys == 0) {
#if CONFIG_MEMORY_BUDGET
        Scheduler::release_memory_pages(stack_pages);
#endif
        CarveStatus kstack_status{};
        kstack_status.kind = CarveFailure::KSTACK;
        return fail(kstack_status);
    }
    child->stack_phys_ = kstack_phys;
    TaskControlBlock::provision_kstack(child, kstack_phys,
                                      TaskControlBlock::STACK_SIZE);
    push_clone_frame(child, regs);

    // Step 11 — activation: install the carved root (field write only —
    // the canary install below translates through it), arm both canary
    // domains (already translating through get_page_table()), publish
    // the binding, then return (the caller's hold + add_task_err tail
    // publishes; Rollback::commit() is the fall-through below).
    child->install_carved_root(root_phys, arch::context_id_alloc());
    canary_install_kernel_stack(child);
    canary_install_user_segments(child);
    const errors::VspaceError act_err =
        cap::CarveBinding::activate_err(slot, root_phys);
    if (act_err != errors::VSPACE_ERR_OK) {
        CarveStatus act_status{};
        act_status.kind = CarveFailure::ACTIVATE;
        act_status.vspace_err = act_err;
        return fail(act_status);
    }

    // Step 12 — return the unpublished child; the caller (ProcSvc::fork)
    // applies the pre-publication TCB-cap hold and add_task_err.
    for (size_t table_idx = 0; table_idx < table_total; ++table_idx) {
        if (tables[table_idx].cap != nullptr) {
            tables[table_idx].cap->release(); // binding pin keeps it
            tables[table_idx].cap = nullptr;
        }
    }
    for (size_t frame_idx = 0; frame_idx < frame_total; ++frame_idx) {
        if (frames[frame_idx].obj != nullptr) {
            frames[frame_idx].obj->release(); // binding pin keeps it
            frames[frame_idx].obj = nullptr;
        }
    }
    if (root_local.obj != nullptr) {
        root_local.obj->release(); // binding pin keeps it
        root_local.obj = nullptr;
    }
    if (tcb_local.obj != nullptr) {
        tcb_local.obj->release(); // binding pin keeps it
        tcb_local.obj = nullptr;
    }
    return child;
}

} // namespace kernel::proc

#endif // CONFIG_PROC_MGR_CARVE_FORK
