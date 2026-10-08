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

/// @file ramdiskd.cpp
/// @brief Ramdisk daemon PID cell, boot-time 16 MiB carve, and FrameCap
/// grant into the daemon CSpace (issue #275).

#include <kernel/ramdisk/ramdiskd.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/log/dmesg.hpp>
#include <logger.hpp>

namespace kernel {
namespace ramdiskd {

static uint64_t g_ramdiskd_pid = 0;

/// @brief Boot-carved backing segments (UntypedMem creator references,
/// one 2 MiB region each; consumed by grant, re-carved on re-grant).
/// Non-null entries are owned here; grant drops each creator reference
/// after its exact retype and nulls the slot.
static cap::UntypedMem *g_segments[RAMDISK_SEGMENTS] = {nullptr};

void set_ramdiskd_pid(uint64_t pid) {
    g_ramdiskd_pid = pid;
}

uint64_t get_ramdiskd_pid() {
    return g_ramdiskd_pid;
}

bool is_ramdiskd_task() {
    auto *cur = Scheduler::current_task();
    return cur && cur->id == g_ramdiskd_pid;
}

bool boot_allocate() {
    for (uint64_t i = 0; i < RAMDISK_SEGMENTS; ++i) {
        if (g_segments[i] != nullptr)
            continue;
        cap::UntypedMem *seg = cap::UntypedMem::create(
            static_cast<size_t>(RAMDISK_SEGMENT_SIZE), true /* is_user */);
        if (seg == nullptr) {
            for (uint64_t j = 0; j < i; ++j) {
                if (g_segments[j] != nullptr) {
                    g_segments[j]->release();
                    g_segments[j] = nullptr;
                }
            }
            log::dmesg_push_sev(log::ErrorSubsystem::DAEMON,
                                 log::kDmesgBase_DAEMON + 9,
                                 log::LogSeverity::ERROR, "ramdisk carve",
                                 i);
            return false;
        }
        g_segments[i] = seg;
    }
    return true;
}

/// @brief Exact-retype one carved segment into the daemon CSpace.
/// @param[out] gen_out Receives the frame slot generation.
/// @return Frame slot index, or -1. On success the segment creator
/// reference is released (slots hold the region); on install failure
/// the segment is retained for a later retry; on retype failure the
/// caller unwinds prior frame slots.
static int grant_one_segment(cap::CNode *cs, uint64_t seg_idx,
                             uint32_t *gen_out) {
    cap::UntypedMem *seg = g_segments[seg_idx];
    uint32_t gen = 0;
    int ut_idx =
        cs->install(seg, cap::CapType::Untyped,
                    cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE, &gen);
    if (ut_idx < 0)
        return -1;
    uint64_t ut_handle =
        cap::encode_user_handle(cs->cspace_id, ut_idx, gen);
    int frame_idx = cap::retype(cs, ut_handle, cap::CapType::Frame,
                                static_cast<size_t>(RAMDISK_SEGMENT_SIZE),
                                cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE,
                                gen_out);
    if (frame_idx < 0) {
        cs->remove(static_cast<uint32_t>(ut_idx));
        return -1;
    }
    // Slots hold references now; drop the boot creator reference (the
    // spent parent owns nothing — dispose frees no frames) and remove
    // the spent Untyped slot: each segment occupies exactly one frame
    // slot, so chain unwind and teardown stay exact.
    seg->release();
    g_segments[seg_idx] = nullptr;
    cs->remove(static_cast<uint32_t>(ut_idx));
    return frame_idx;
}

int64_t grant_storage(uint64_t daemon_pid, uint64_t *out_handles) {
    if (out_handles == nullptr)
        return RAMDISK_ERR_RANGE;
    for (uint64_t i = 0; i < RAMDISK_SEGMENTS; ++i)
        out_handles[i] = static_cast<uint64_t>(-1);
    if (daemon_pid == 0)
        return RAMDISK_ERR_NOGRANT;
    // Re-carve when segments are gone (first grant, or re-grant after
    // the previous daemon died and its slots freed the frames).
    if (!boot_allocate())
        return RAMDISK_ERR_NOMEM;
    auto *daemon = Scheduler::find_task(daemon_pid);
    if (daemon == nullptr ||
        daemon->magic != TaskControlBlock::TCB_MAGIC)
        return RAMDISK_ERR_NOGRANT;
    daemon->ensure_cspace();
    cap::CNode *cs = daemon->get_cspace();
    if (cs == nullptr)
        return RAMDISK_ERR_NOGRANT;

    // Exact-retype each segment into the daemon CSpace (canonical
    // create/install/retype per test_cap_untyped; exact size installs
    // ONE frame slot, no child). Chain failure revokes every installed
    // frame slot, so no partial grant is ever observable.
    for (uint64_t i = 0; i < RAMDISK_SEGMENTS; ++i) {
        uint32_t frame_gen = 0;
        int frame_idx = grant_one_segment(cs, i, &frame_gen);
        if (frame_idx < 0) {
            for (uint64_t j = 0; j < i; ++j)
                cap::revoke(cs, out_handles[j]);
            for (uint64_t j = 0; j < RAMDISK_SEGMENTS; ++j)
                out_handles[j] = static_cast<uint64_t>(-1);
            return RAMDISK_ERR_NOMEM;
        }
        out_handles[i] =
            cap::encode_user_handle(cs->cspace_id, frame_idx, frame_gen);
    }
    return RAMDISK_OK;
}

int64_t regrant(uint64_t daemon_pid) {
    uint64_t handles[RAMDISK_SEGMENTS];
    const int64_t gr = grant_storage(daemon_pid, handles);
    if (gr != RAMDISK_OK)
        return gr;
    Message grant{};
    grant.sender_id = 1;
    grant.type = RAMDISK_GRANT;
    __builtin_memcpy(grant.data, handles, sizeof(handles));
    grant.data_size = sizeof(handles);
    IPC::send(daemon_pid, grant, 0);
    return RAMDISK_OK;
}

} // namespace ramdiskd
} // namespace kernel
