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

/// @file block_srv.hpp
/// @brief Block-storage server proxy (issue #47, strangler step a).
/// Kernel-side contract for the userspace block-srv PD: 48 B wire
/// RPC, PID cell, epoch/liveness, tombstone drain, and the fault-probe
/// entry points. Everything here is declaration-only;
///        definitions live in block_srv.cpp inside `#if CONFIG_BLOCK_SRV`,
///        so the OFF build (`CONFIG_BLOCK_SRV=0`, shipped default) is
///        byte-identical to the pre-#47 tree. AHCI/virtio-blk probing
///        stays kernel-side until the flip (callers gate on the knob).

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::block_srv {

#if CONFIG_BLOCK_SRV

/// @brief Wire ops carried in BlockRpc::op (server dispatch switch).
enum class BlockSrvOp : uint8_t {
    BIND = 0,          ///< server announce / capability handshake
    GEOMETRY = 1,      ///< block geometry probe
    READ_BLOCK = 2,    ///< read one 512 B block
    WRITE_BLOCK = 3,   ///< write one 512 B block
    FLUSH = 4,         ///< flush write-back cache
    DMA_MAP = 5,       ///< map a DMA window (typed deny without domain)
    DMA_UNMAP = 6,     ///< unmap a DMA window
    LOOPBACK = 7,      ///< loopback diagnostic (no hardware)
    INJECT_MID_TRANSFER = 8, ///< one-shot mid-transfer fault drill
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct BlockRpc {
    uint8_t op;         ///< BlockSrvOp
    uint8_t flags;      ///< bit0: DEGRADED (no backend, loopback only)
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id
    uint32_t owner_gen; ///< requesting task generation
    uint64_t lba;       ///< logical block address
    uint64_t arg;       ///< op argument (DMA handle, pattern seed)
    uint32_t handle;    ///< DMA/transfer handle (gen-tagged by server)
    uint16_t status_op; ///< echoed op for mismatch detection
    uint16_t status;    ///< reply: negated errno (0 = ok)
};

static_assert(sizeof(BlockRpc) <= 48,
              "BlockRpc must fit the 48 B fast IPC payload");

/// @brief Maximum transfer unit the loopback path moves per call (bytes).
inline constexpr uint64_t kBlockLoopbackBytes = 512;

/// @brief Record the PID of the block-srv daemon task.
void set_block_srv_pid(uint64_t pid);
/// @brief Get the recorded block-srv PID (0 when unset).
uint64_t get_block_srv_pid();
/// @brief True when the server task is live (present, not TERMINATED/REAPED).
bool block_srv_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t block_srv_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void block_srv_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void block_srv_on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void block_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block held.
[[nodiscard]] bool block_srv_rpc(const BlockRpc &req, BlockRpc &reply);

/// @brief Loopback diagnostic: fixed-pattern 512 B transfer via the PD.
/// @return 0 on verified loopback, negative errno otherwise.
int64_t block_srv_fault_loopback();

/// @brief DMA-map probe: DMA_MAP with a zero handle must fail closed.
/// @return -EACCES-shaped typed error when the server honors it.
int64_t block_srv_fault_nodomain();

#endif // CONFIG_BLOCK_SRV

} // namespace kernel::block_srv
