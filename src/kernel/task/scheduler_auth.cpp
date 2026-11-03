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

/// @file scheduler_auth.cpp
/// @brief User-ELF authenticity slice for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_auth_internal.hpp>
#include <crc32.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <lib/logger.hpp>

namespace kernel {

// Issue #46: per-slice page budget (bounded IRQ-mask window + WCET-flat).
static constexpr uint64_t kAuthSlicePages = 2;

Scheduler::AuthStepResult Scheduler::auth_verify_step(uint64_t &out_task_id,
                                                      char *out_name,
                                                      uint64_t name_len) noexcept {

    out_task_id = 0;
    if (out_name && name_len > 0)
        out_name[0] = '\0';
    // Whole-step IRQ mask: on UP nothing preempts between select and kill
    // (bounded work: one registry walk + at most kAuthSlicePages CRCs, no
    // allocation, no reschedule).  TCB frees funnel through this same idle
    // task's cleanup_step (sequential) or invisible non-registry paths, so
    // the unlocked work phase cannot race a free; the commit re-validation
    // below is belt-and-braces (SMP-future).
    arch::IrqGuard irq_guard{};
    // Issue #46: the re-verify side needs the table live too — the loader
    // inits its own side, but this step also runs driven directly from
    // tests (where idle_task_main never runs).  Idempotent.
    CRC32::init();

    TaskControlBlock *cand = nullptr;
    uint64_t cand_pml4 = 0;
    uint64_t cand_off = 0;
    uint32_t cand_acc = CRC32::INITIAL;
    uint32_t cand_baseline = 0;
    uint64_t cand_total = 0;
    {
        ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take (non-recursive)
    SchedulerLockGuard guard(scheduler_lock_);
        uint64_t n = all_tasks_.size();
        for (uint64_t i = 0; i < n; ++i) {
            TaskControlBlock *t = task_at(i);
            if (!t || !TaskControlBlock::is_valid(t))
                continue;
            if (is_idle_task(t) || !t->is_user_)
                continue;
            if (t->ro_seg_count_ == 0)
                continue; // no baseline (kernel task / direct elf::load)
            auto st = t->ro_verify_state_;
            if (st != TaskControlBlock::RoVerifyState::UNVERIFIED &&
                st != TaskControlBlock::RoVerifyState::VERIFYING)
                continue;
            if (t->state == TaskState::TERMINATED ||
                t->state == TaskState::REAPED)
                continue; // dead: nothing left to protect
            if (st == TaskControlBlock::RoVerifyState::UNVERIFIED)
                t->ro_verify_state_ =
                    TaskControlBlock::RoVerifyState::VERIFYING;
            cand = t;
            cand_pml4 = t->get_page_table();
            cand_off = t->ro_verify_off_;
            cand_acc = t->ro_verify_acc_;
            cand_baseline = t->ro_crc32_;
            for (uint64_t r = 0; r < t->ro_seg_count_; ++r)
                cand_total += t->ro_ranges_[r].len;
            break; // one candidate per slice (round-robin by registry order)
        }
    }
    if (!cand)
        return AuthStepResult::NONE;
    if (cand_off >= cand_total && cand_total > 0) {
        // Unreachable: commit finalizes exactly at total.  Defensive
        // restart (re-verify from scratch) rather than a false kill.
        ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take (non-recursive)
    SchedulerLockGuard guard(scheduler_lock_);
        if (TaskControlBlock::is_valid(cand) &&
            cand->get_page_table() == cand_pml4) {
            cand->ro_verify_off_ = 0;
            cand->ro_verify_acc_ = CRC32::INITIAL;
        }
        kernel::Logger::warn("auth: task id=%u progress reset (off>=total)",
                             static_cast<unsigned>(cand->id));
        return AuthStepResult::PROGRESS;
    }

    // ---- work phase (lock released, IRQs still masked): CRC up to
    // kAuthSlicePages of the uncovered ranges via the target page table.
    uint32_t acc = cand_acc;
    uint64_t off = cand_off;
    uint64_t pages_left = kAuthSlicePages;
    bool read_ok = true;
    uint64_t skip = off;
    for (uint64_t r = 0; r < cand->ro_seg_count_ && pages_left > 0; ++r) {
        // NOLINTNEXTLINE: ranges immutable post-publication (loader-owned
        // until completed_tcb_ handoff); re-validated at commit.
        uint64_t rv = cand->ro_ranges_[r].vaddr;
        uint64_t rl = cand->ro_ranges_[r].len;
        if (skip >= rl) {
            skip -= rl;
            continue;
        }
        uint64_t va = rv + skip;
        uint64_t left = rl - skip;
        skip = 0;
        while (left > 0 && pages_left > 0) {
            uint64_t page_off = va & (arch::PAGE_SIZE - 1);
            uint64_t take = arch::PAGE_SIZE - page_off;
            if (take > left)
                take = left;
            if (!elf::crc_user_range(cand_pml4, va, take, acc)) {
                read_ok = false;
                break;
            }
            va += take;
            off += take;
            left -= take;
            --pages_left; // each chunk touches one page (partials count)
        }
        if (!read_ok)
            break;
    }
    bool done = read_ok && off >= cand_total;

    // ---- commit phase: re-validate identity, then verdict.
    bool kill = false;
    bool verified = false;
    {
        ENSURE(scheduler_lock_depth_own() == 0); // issue #369 T2b: no reentrant take (non-recursive)
    SchedulerLockGuard guard(scheduler_lock_);
        if (!TaskControlBlock::is_valid(cand) ||
            cand->get_page_table() != cand_pml4 ||
            cand->ro_verify_state_ !=
                TaskControlBlock::RoVerifyState::VERIFYING ||
            cand->ro_verify_off_ != cand_off ||
            cand->state == TaskState::TERMINATED ||
            cand->state == TaskState::REAPED) {
            return AuthStepResult::PROGRESS; // lost race: re-select later
        }
        if (!read_ok) {
            // Range unmapped mid-image: not intact as baselined — fail
            // closed (latch FAILED so the task can never verify later).
            cand->ro_verify_state_ = TaskControlBlock::RoVerifyState::FAILED;
            kill = true;
        } else {
            cand->ro_verify_off_ = off;
            cand->ro_verify_acc_ = acc;
            if (done) {
                if (CRC32::finalize(acc) == cand_baseline) {
                    cand->ro_verify_state_ =
                        TaskControlBlock::RoVerifyState::VERIFIED;
                    verified = true;
                } else {
                    cand->ro_verify_state_ =
                        TaskControlBlock::RoVerifyState::FAILED;
                    kill = true;
                }
            }
        }
    }
    if (verified)
        return AuthStepResult::VERIFIED;
    if (!kill)
        return AuthStepResult::PROGRESS;
    // Mismatch verdict: terminate with the dmesg-matched exit code.  Lock
    // released (terminate_err retakes it); IRQs still masked so on UP no
    // task switch can interleave; live-state re-checked for the record.
    if (cand->state == TaskState::TERMINATED ||
        cand->state == TaskState::REAPED)
        return AuthStepResult::PROGRESS;
    const errors::SchedulerError err =
        terminate_err(*cand, elf::kElfAuthKillExitCode);
    if (err != errors::SCHED_ERR_OK) {
        kernel::Logger::warn("auth: task id=%u kill refused (err=%u)",
                             static_cast<unsigned>(cand->id),
                             static_cast<unsigned>(err));
        return AuthStepResult::PROGRESS;
    }
    out_task_id = cand->id; // zombie memory stays valid until cleanup_step
    if (out_name && name_len > 0) {
        uint64_t i = 0;
        while (i + 1 < name_len && cand->name[i] != '\0') {
            out_name[i] = cand->name[i];
            ++i;
        }
        out_name[i] = '\0';
    }
    return AuthStepResult::KILLED;
}

}  // namespace kernel
