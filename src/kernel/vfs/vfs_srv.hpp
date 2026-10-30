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

/// @file vfs_srv.hpp
/// @brief VFS server proxy (issue #47, strangler step a).
///        Kernel-side contract for the userspace vfs-srv PD: 48 B wire
///        RPC, PID cell, epoch/liveness, tombstone drain, and the sys_*
///        proxy entry points. Everything here is declaration-only;
///        definitions live in vfs_srv.cpp inside `#if CONFIG_VFS_SRV`,
///        so the OFF build (`CONFIG_VFS_SRV=0`, shipped default) is
///        byte-identical to the pre-#47 tree. Authorize stays kernel-side:
///        this proxy is diagnostic/parity only until the flip.

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel::vfs_srv {

#if CONFIG_VFS_SRV

/// @brief Wire ops carried in VfsRpc::op (server dispatch switch).
enum class VfsSrvOp : uint8_t {
    BIND = 0,          ///< server announce / capability handshake
    INITRD = 1,        ///< initrd backend probe
    FAT32 = 2,         ///< fat32 backend probe
    TMPFS = 3,         ///< tmpfs backend probe
    DEVFS = 4,         ///< devfs backend probe
    PIPE = 5,          ///< pipe backend probe
    RAMDISK = 6,       ///< ramdisk_fs backend probe
    PROCFS_RENDER = 7, ///< procfs render probe (renders stay in-kernel)
    INJECT_CORRUPT = 8,///< one-shot corrupt-backend drill (diagnostic)
};

/// @brief 48 B wire struct (fits CONFIG_IPC_FAST_PAYLOAD_BYTES).
struct VfsRpc {
    uint8_t op;         ///< VfsSrvOp
    uint8_t flags;      ///< bit0: DEGRADED (backend unavailable, tick-derived)
    uint16_t rsvd;      ///< reserved (zero)
    uint32_t epoch;     ///< proxy epoch (server purges on increase)
    uint32_t owner_id;  ///< requesting task id
    uint32_t owner_gen; ///< requesting task generation
    uint64_t ino;       ///< vnode number (probe-scoped)
    uint64_t offset;    ///< byte offset (probe-scoped)
    uint64_t size;      ///< byte count (probe-scoped)
    uint16_t status_op; ///< echoed op for mismatch detection
    uint16_t status;    ///< reply: negated errno (0 = ok)
    uint32_t rsvd2;     ///< reserved (zero)
};

static_assert(sizeof(VfsRpc) <= 48,
              "VfsRpc must fit the 48 B fast IPC payload");

/// @brief Record the PID of the vfs-srv daemon task.
void set_vfs_srv_pid(uint64_t pid);
/// @brief Get the recorded vfs-srv PID (0 when unset).
uint64_t get_vfs_srv_pid();
/// @brief True when the server task is live (present, not TERMINATED/REAPED).
bool vfs_srv_alive();
/// @brief Proxy epoch (bumped on restart / snapshot reset / boot table).
uint32_t vfs_srv_epoch();
/// @brief Bump the epoch and purge tombstones (server restart path).
void vfs_srv_bump_epoch();
/// @brief Supervisor hook: record a restart (store pid, bump epoch).
void vfs_srv_on_restart(uint64_t pid);
/// @brief Teardown hook: latch a dead owner's id+generation (lock-free).
void vfs_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen);
/// @brief Generic RPC: validate server, bounded wait, validated reply.
/// @return true with *reply filled on a validated reply, false fail-closed
///         (server down, timeout, or reply mismatch) with no block held.
[[nodiscard]] bool vfs_srv_rpc(const VfsRpc &req, VfsRpc &reply);

/// @brief One-shot corrupt-backend drill (INJECT_CORRUPT probe).
/// @return 0 on posted drill, -ENOSYS when the server is unavailable.
int64_t vfs_srv_fault_corrupt();

#endif // CONFIG_VFS_SRV

} // namespace kernel::vfs_srv
