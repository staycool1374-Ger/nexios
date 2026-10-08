#pragma once

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

/// @file ramdiskd.hpp
/// @brief Ramdisk daemon (issue #275): opcodes, wire structs, PID cell,
/// error codes, boot allocation + grant entry points.

#pragma once

#include <types.hpp>

namespace kernel {

struct TaskControlBlock;

namespace ramdiskd {

/// @brief Ramdisk request opcodes (400-series; vfsd=100s, iocd=200s,
/// watchdogd=300s).
static constexpr uint64_t RAMDISK_READ_BLOCK = 400;
static constexpr uint64_t RAMDISK_WRITE_BLOCK = 401;
static constexpr uint64_t RAMDISK_STAT = 402;
static constexpr uint64_t RAMDISK_GRANT = 403;

/// @brief Geometry (all named per CODING_STYLE §10.5). One 512 B block =
/// 16 chunks x 32 B: request header (block_no + chunk_idx = 16 B) plus
/// chunk data (32 B) fits the 64 B IPC payload exactly (sender_id +
/// type overlay the first 16 B, as in VfsdMsg).
static constexpr uint64_t RAMDISK_BLOCK_SIZE = 512;
static constexpr uint64_t RAMDISK_CHUNK_DATA = 32;
static constexpr uint64_t RAMDISK_CHUNKS_PER_BLOCK = 16;

/// @brief Backing store: fixed 16 MiB = 32768 blocks. The store is
/// eight independent 2 MiB segments (FrameUserMap regions are 2 MiB, so
/// one 16 MiB FrameCap could never map): block B lives in segment
/// B / SEGMENT_BLOCKS at offset (B % SEGMENT_BLOCKS) * BLOCK_SIZE.
static constexpr uint64_t RAMDISK_SIZE = 16ULL * 1024ULL * 1024ULL;
static constexpr uint64_t RAMDISK_BLOCKS = 32768ULL;
static constexpr uint64_t RAMDISK_SEGMENTS = 8ULL;
static constexpr uint64_t RAMDISK_SEGMENT_SIZE = 2ULL * 1024ULL * 1024ULL;
static constexpr uint64_t RAMDISK_SEGMENT_BLOCKS = 4096ULL;

/// @brief Wire error codes (fail-closed; reachable IPC input never
/// panics, CODING_STYLE §5).
static constexpr int64_t RAMDISK_OK = 0;
static constexpr int64_t RAMDISK_ERR_RANGE = -1;
static constexpr int64_t RAMDISK_ERR_NOGRANT = -2;
static constexpr int64_t RAMDISK_ERR_NOMEM = -3;
static constexpr int64_t RAMDISK_ERR_UNKNOWN_OP = -4;

/// @brief Wire layout (issue #275): sys_receive copies ONLY msg.data[]
/// into the userspace buffer and returns msg.type (sender_id is NEVER
/// delivered — debugd.c documents the same contract). The client therefore
/// packs its own pid first: request data[] = sender_pid + block_no +
/// chunk_idx (24 B) + payload (32 B) = 56 B; reply data[] = result (8 B)
/// + payload (32 B) = 40 B. Replies go to the data-packed pid.
struct Msg {
    uint64_t sender_pid;                   ///< Client pid (packed by client).
    uint64_t block_no;                     ///< 512 B block index.
    uint64_t chunk_idx;                    ///< Chunk within the block.
    uint8_t data[RAMDISK_CHUNK_DATA];      ///< Chunk payload (write path).
};

/// @brief Chunk reply data[] layout (40 B).
struct Reply {
    int64_t result;                        ///< RAMDISK_OK or RAMDISK_ERR_*.
    uint8_t data[RAMDISK_CHUNK_DATA];      ///< Chunk payload (read path).
};

static_assert(sizeof(Msg) == 56, "ramdisk request must fit the 64 B payload");
static_assert(sizeof(Reply) == 40, "ramdisk reply must fit the 64 B payload");

/// @brief Normative chunk geometry (the userspace daemon mirrors this
/// math in C; kernel tests assert it here). Block B lives in segment
/// B / SEGMENT_BLOCKS at byte offset
/// (B % SEGMENT_BLOCKS) * BLOCK_SIZE + chunk * CHUNK_DATA.
static constexpr bool chunk_valid(uint64_t block_no, uint64_t chunk_idx) {
    return block_no < RAMDISK_BLOCKS && chunk_idx < RAMDISK_CHUNKS_PER_BLOCK;
}

/// @brief Byte offset of a chunk inside its segment window.
static constexpr uint64_t chunk_offset(uint64_t block_no,
                                       uint64_t chunk_idx) {
    return (block_no % RAMDISK_SEGMENT_BLOCKS) * RAMDISK_BLOCK_SIZE +
           chunk_idx * RAMDISK_CHUNK_DATA;
}

/// @brief Segment index holding a block.
static constexpr uint64_t chunk_segment(uint64_t block_no) {
    return block_no / RAMDISK_SEGMENT_BLOCKS;
}

/// @brief Record the PID of the ramdisk daemon task.
void set_ramdiskd_pid(uint64_t pid);
/// @brief Get the recorded ramdisk daemon PID.
/// @return The PID, or 0 if not yet set.
uint64_t get_ramdiskd_pid();
/// @brief Check if the current task is the ramdisk daemon.
/// @return true if the current task's PID matches the daemon PID.
bool is_ramdiskd_task();

/// @brief Carve the 16 MiB backing region (boot-time, before driver/DMA
/// setup; fail-closed nullptr on exhaustion).
/// @return True when the region is carved and retained.
bool boot_allocate();

/// @brief Mint one full-segment FrameCap per backing segment into the
/// daemon's CSpace (READ|WRITE) and deliver the encoded handles.
/// Re-carves segments when they are gone (re-grant after daemon death).
/// @param daemon_pid Target daemon task ID (0/absent fails closed).
/// @param out_handles Caller-provided array of exactly RAMDISK_SEGMENTS
/// slots; receives the encoded user handles (-1 per failed slot, all
/// -1 unless the return is RAMDISK_OK).
/// @return RAMDISK_OK or a RAMDISK_ERR_* code.
int64_t grant_storage(uint64_t daemon_pid, uint64_t *out_handles);

/// @brief Re-grant after a daemon restart: grant_storage plus the
/// one-way GRANT message push (the daemon maps on receipt). Failures
/// log + dmesg and leave the daemon serving NOGRANT (fail-closed).
/// @return RAMDISK_OK or a RAMDISK_ERR_* code.
int64_t regrant(uint64_t daemon_pid);

} // namespace ramdiskd
} // namespace kernel
