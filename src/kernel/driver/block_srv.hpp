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
    CAP_GET = 9,       ///< per-index cap conveyance (issue #47 step-b1):
                       ///< kernel→PD carries index i + handle + phys +
                       ///< size; PD maps and echoes. CAP_GET rounds run
                       ///< first (index order); BIND goes last so the PD
                       ///< probes with every cap present.
};

/// @brief PD backend selected at BIND (issue #47 step-b1).
enum class BlkBackend : uint8_t {
    NONE = 0,       ///< no backend (loopback/diagnostic only)
    ATA_PIO = 1,    ///< x86_64 ATA PIO over I/O-port caps
    VIRTIO_MMIO = 2,///< aarch64/riscv64 virtio-blk over MMIO (v2)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct BlockRpc {
    uint8_t op;         ///< BlockSrvOp
    uint8_t flags;      ///< BIND/CAP_GET req: row cap count; BIND reply:
                        ///< row cap count echoed; bit0: DEGRADED
    uint16_t rsvd;      ///< CAP_GET req: io_port (0/1); else zero
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id
    uint32_t owner_gen; ///< requesting task generation
    uint64_t lba;       ///< logical block address / CAP_GET req: phys base
    uint64_t arg;       ///< op argument / CAP_GET req: encoded cap handle;
                        ///< READ/WRITE: request sequence; GEOM reply: size
    uint32_t handle;    ///< DMA/transfer handle / CAP_GET: row index
    uint16_t status_op; ///< echoed op / BIND reply: backend / CAP reply:
                        ///< kind echoed
    uint16_t status;    ///< reply: negated errno (0 = ok)
    uint64_t aux;       ///< CAP_GET req: cap byte size; BIND reply:
                        ///< sector count, or the PD probe stage
                        ///< (1..6) when backend NONE (diagnostic).
};

static_assert(sizeof(BlockRpc) == 48,
              "BlockRpc must be exactly the 48 B fast IPC payload");

/// @brief Shared data window layout (issue #47 step-b1): one 4-page
/// DMA_REGION frame shared kernel↔PD. Page 0 holds the virtio queue
/// area; the sector payload, request header, and status byte live at
/// fixed offsets. The PD mirrors these constants (userspace/block-srv.c).
inline constexpr uint64_t kBlockRegionPages = 4;
inline constexpr uint64_t kBlockQueueBytes = 0x1000;
inline constexpr uint64_t kBlockDataOffset = 0x1000;
inline constexpr uint64_t kBlockHdrOffset = 0x1200;
inline constexpr uint64_t kBlockStatusOffset = 0x1300;
inline constexpr uint64_t kBlockSectorSize = 512;

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

/// @brief Bind the PD (issue #47 step-b1): BIND + per-index CAP_GET
/// rounds with SDF cross-check (count, order, kind, encoded handle,
/// phys/size). Caches backend, geometry, and the data-window phys.
/// @return true when bound at the current epoch (rebinds on change).
bool block_srv_bind();

/// @brief Cached sector count from the last bind (0 when unbound).
uint64_t block_srv_sector_count();
/// @brief Cached sector size in bytes (512 when bound, 0 unbound).
uint64_t block_srv_sector_size();
/// @brief Backend selected by the PD at bind (NONE when unbound).
BlkBackend block_srv_backend();

/// @brief Read one 512 B sector through the PD (issue #47 step-b1).
/// @param lba Sector index; @p buffer receives 512 B on success.
/// @return 0 on success, negative errno otherwise (-ENOSYS unbound,
/// -EINVAL OOB, -EIO transport/mismatch, -EAGAIN epoch raced once).
int64_t block_srv_read_sector(uint64_t lba, uint8_t *buffer);

/// @brief Write one 512 B sector through the PD (issue #47 step-b1).
/// @param lba Sector index; @p buffer supplies 512 B.
/// @return 0 on success, negative errno otherwise (same as read).
int64_t block_srv_write_sector(uint64_t lba, const uint8_t *buffer);

#endif // CONFIG_BLOCK_SRV

} // namespace kernel::block_srv
