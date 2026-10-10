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

/// @file ramdisk_fs.hpp
/// @brief Minimal flat file store over ramdiskd block IPC (issue #314),
/// mounted at `/mnt/ramdisk`. Fixed directory region + bump-allocated
/// extents; same `VnodeOps` surface and mount point reserved for a later
/// full FAT32 backend (explicit non-goal of this issue).
///
/// Visibility rule (stage-commit without rename — there is no rename
/// syscall): create(name, expected_size) makes an INVISIBLE entry
/// (size 0); lookup/open/read refuse entries whose size != expected_size.
/// The final sized write flips the file visible atomically at the single
/// dir-region persist. Torn uploads are therefore never executable.
/// Integrity verification is client-side re-read (no stored checksum in
/// v1; bit-rot detection is out of scope — see docs/specs/ramdisk_fs.md).

#pragma once

#include <kernel/vfs/vfs.hpp>

namespace kernel {
namespace vfs {

/// @brief Try to mount the ramdisk filesystem at `/mnt/ramdisk`.
/// Best-effort and fail-closed: returns nonzero without mounting when
/// ramdiskd is ungranted; never blocks boot. Safe to call repeatedly
/// (already-mounted is a no-op success).
/// @return 0 on success (mounted or already mounted), VFS_INVALID else.
int ramdisk_fs_try_mount();

extern Filesystem ramdisk_fs;

} // namespace vfs
} // namespace kernel
