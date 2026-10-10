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

/// @file ramdisk_fs.cpp
/// @brief Minimal flat file store over ramdiskd block IPC (issue #314).
///
/// Design (owner-directed minimal flat store; FAT32 explicitly not this
/// issue; same VnodeOps surface + mount point reserved for the later FAT
/// backend): superblock in ramdisk block 0, fixed directory region, bump
/// allocated extents, no free list, no journaling, no resize.
///
/// Deviation from the planner's rename-based staging (documented per
/// process rules): there is NO rename syscall in the ABI, so temp-name +
/// atomic rename is unimplementable without an ABI change (out of scope).
/// The torn-image safety chain is instead: (1) uploader re-read-verifies
/// byte-exact after upload (client-side, fail-closed); (2) the ELF loader
/// refuses truncated images (validate_header + VULN-H2 segment-bounds
/// checks against file size); (3) single-writer uploader discipline.
/// Created files are visible immediately (tmpfs semantics); unlinked
/// entries free no blocks (documented; 16 MiB backing, tiny files).
///
/// Concurrency (§11): the FS mutex is NEVER held across block IPC
/// (IPC::send_sync reschedules). Mutations snapshot under the lock, run
/// IPC unlocked, then re-validate an epoch counter (bounded 3 retries)
/// before committing. The daemon never takes this lock, so no wait cycle
/// exists. Resource discipline is MemPool-only (no new ResourceTracker
/// counters — same as tmpfs).

#include <kernel/vfs/ramdisk_fs.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/ramdisk/ramdiskd.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/sync/mutex.hpp>
#include <crc32.hpp>
#include <logger.hpp>
#include <string.hpp>

namespace kernel {
namespace vfs {

// Geometry (all named per CODING_STYLE §10.5; consistent with
// ramdiskd.hpp: 512 B blocks, 16 x 32 B chunks, 32768 blocks total).
static constexpr uint64_t RDSB_MAGIC = 0x52445331464C4154ULL; // "RDS1FLAT"
// v2 (issue #318): the directory grew a per-file CRC32 + validity flags, and the
// superblock carries a CRC32 over the whole directory region (metadata
// integrity).  A v1 image is discarded fail-closed (no free list => not
// resumable); see rd_ensure_cached.
static constexpr uint64_t RDSB_VERSION = 2;
static constexpr uint64_t RDBLK_SUPER = 0;
static constexpr uint64_t RDBLK_DIR_START = 1;
static constexpr uint64_t RDBLK_DIR_COUNT = 10;
static constexpr uint64_t RDBLK_DATA_START = 11;
static constexpr uint64_t RDF_MAX_FILES = 50;
static constexpr uint64_t RDF_NAME_LEN = 63; // +1 NUL in the 64 B field
static constexpr uint64_t RDF_MAX_FILE_SIZE = 1ULL * 1024 * 1024; // 1 MiB
static constexpr uint64_t RD_MUTATE_RETRIES = 3;
static constexpr uint64_t RD_CHUNK_DATA = 32;
static constexpr uint64_t RD_CHUNKS_PER_BLOCK = 16;
static constexpr uint64_t RD_BLOCK_SIZE = 512;
static constexpr uint64_t RD_DIR_REGION_BYTES = RDBLK_DIR_COUNT * RD_BLOCK_SIZE;

/// @brief Directory-entry validity flag (rd_dir[i].flags bit).
static constexpr uint32_t RD_ENTRY_VALID = 1U;

/// @brief On-disk directory entry (96 B; serialized contiguously across
/// the dir region: entry i starts at byte i * 96).  `crc32` is the finalized
/// CRC32 (src/lib/crc32.hpp) over the file's LIVE byte range only (block
/// padding excluded); `flags` carries RD_ENTRY_VALID.  An entry is exposed
/// only when VALID and its CRC verifies (fail-closed; issue #318).
struct RdDirEntry {
    char name[64];          ///< NUL-terminated (name[0] == 0 => free slot).
    uint64_t size;          ///< Current file size in bytes.
    uint64_t start_block;   ///< First ramdisk data block of the extent.
    uint64_t block_count;   ///< Extent length in 512 B blocks.
    uint32_t crc32;         ///< CRC32 of the live bytes (finalized).
    uint32_t flags;         ///< RD_ENTRY_VALID etc.
};
static_assert(sizeof(RdDirEntry) == 96, "dir entry layout changed");

/// @brief On-disk superblock (first 40 B of ramdisk block 0).
struct RdSuper {
    uint64_t magic;
    uint64_t version;
    uint64_t file_count;
    uint64_t next_data_block;
    uint32_t dir_crc32;     ///< CRC32 over the serialized dir region.
    uint32_t reserved;      ///< padding (keeps 8-byte alignment).
};

/// @brief Per-open extent descriptor (vnode private_data, MemPool-owned).
struct RdExtent {
    uint64_t start_block;
    uint64_t block_count;
    uint64_t dir_index; // slot in rd_dir[] for size updates on write/close
    bool dirty;         // sized changed since last persist (flushed on close)
};

static sync::Mutex rd_lock{};
static uint64_t rd_epoch = 0;
static uint64_t rd_next_ino = 1;
static bool rd_cached = false;
static uint64_t rd_cached_pid = 0;
static RdSuper rd_super{};
static RdDirEntry rd_dir[RDF_MAX_FILES]{};
static Vnode rd_root{};

static_assert(sizeof(RdSuper) == 40, "superblock layout changed");

// Forward declaration (definition below; used by the CRC helpers).
static int rd_persist_slot(uint64_t slot);

/// @brief One 512 B block transfer through ramdiskd (never under rd_lock).
/// @return 0 on success, -1 on any failure (ungranted/range/IPC).
static int rd_block_op(uint64_t block_no, uint8_t *buf512, bool is_write) {
    if (block_no >= ramdiskd::RAMDISK_BLOCKS)
        return -1;
    uint64_t pid = ramdiskd::get_ramdiskd_pid();
    if (pid == 0)
        return -1;
    auto *cur = Scheduler::current_task();
    if (cur == nullptr)
        return -1;
    uint64_t caller_id = cur->id;
    for (uint64_t chunk = 0; chunk < RD_CHUNKS_PER_BLOCK; ++chunk) {
        Message req{};
        req.type = is_write ? ramdiskd::RAMDISK_WRITE_BLOCK
                            : ramdiskd::RAMDISK_READ_BLOCK;
        __builtin_memcpy(req.data, &caller_id, 8);
        __builtin_memcpy(req.data + 8, &block_no, 8);
        __builtin_memcpy(req.data + 16, &chunk, 8);
        if (is_write)
            __builtin_memcpy(req.data + 24, buf512 + chunk * RD_CHUNK_DATA,
                             RD_CHUNK_DATA);
        req.data_size = is_write ? 56 : 24;
        Message reply{};
        if (!IPC::send_sync(pid, req, reply))
            return -1;
        int64_t result = 0;
        __builtin_memcpy(&result, reply.data, 8);
        if (result != ramdiskd::RAMDISK_OK)
            return -1;
        if (!is_write)
            __builtin_memcpy(buf512 + chunk * RD_CHUNK_DATA, reply.data + 8,
                             RD_CHUNK_DATA);
    }
    return 0;
}

/// @brief Block transfer with bounded retries (never under rd_lock).
/// Daemon IPC can fail transiently on cold paths (notably the first
/// lazy segment map under TCG); chunk writes are idempotent (same
/// block/chunk/data), so retrying is safe. Metadata persists use the
/// outer epoch retry instead (same content re-persisted).
static int rd_block_op_retry(uint64_t block_no, uint8_t *buf512,
                             bool is_write) {
    for (uint64_t attempt = 0; attempt < RD_MUTATE_RETRIES; ++attempt) {
        if (rd_block_op(block_no, buf512, is_write) == 0)
            return 0;
    }
    return -1;
}

/// @brief Read ramdisk superblock (never under rd_lock).
static int rd_read_super(RdSuper &out) {
    uint8_t blk[RD_BLOCK_SIZE] = {};
    if (rd_block_op(RDBLK_SUPER, blk, false) < 0)
        return -1;
    __builtin_memcpy(&out, blk, sizeof(RdSuper));
    return 0;
}

/// @brief Serialize one 512 B dir-region block from the entry array.  Handles
/// entries that straddle a block boundary (a 96 B entry does not divide 512):
/// each block receives the portion [max(off,base), min(off+96,end)).
static void rd_serialize_dir_block(const RdDirEntry *dir, uint64_t dir_blk,
                                   uint8_t *blk) {
    __builtin_memset(blk, 0, RD_BLOCK_SIZE);
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(dir);
    uint64_t base = dir_blk * RD_BLOCK_SIZE;
    uint64_t end = base + RD_BLOCK_SIZE;
    for (uint64_t i = 0; i < RDF_MAX_FILES; ++i) {
        uint64_t off = i * sizeof(RdDirEntry);
        uint64_t eend = off + sizeof(RdDirEntry);
        if (eend <= base || off >= end)
            continue;
        uint64_t lo = off > base ? off : base;
        uint64_t hi = eend < end ? eend : end;
        __builtin_memcpy(blk + (lo - base), raw + lo, hi - lo);
    }
}

/// @brief Persist superblock + full dir region (never under rd_lock).  The
/// superblock's `dir_crc32` covers exactly the serialized dir-region bytes.
static int rd_store_all(const RdSuper &sup_in, const RdDirEntry *dir) {
    uint8_t blk[RD_BLOCK_SIZE] = {};
    CRC32::init();
    uint32_t crc = CRC32::INITIAL;
    for (uint64_t dir_blk = 0; dir_blk < RDBLK_DIR_COUNT; ++dir_blk) {
        rd_serialize_dir_block(dir, dir_blk, blk);
        crc = CRC32::update(crc, blk, RD_BLOCK_SIZE);
    }
    RdSuper sup = sup_in;
    sup.dir_crc32 = CRC32::finalize(crc);
    __builtin_memset(blk, 0, RD_BLOCK_SIZE);
    __builtin_memcpy(blk, &sup, sizeof(RdSuper));
    if (rd_block_op(RDBLK_SUPER, blk, true) < 0)
        return -1;
    for (uint64_t dir_blk = 0; dir_blk < RDBLK_DIR_COUNT; ++dir_blk) {
        rd_serialize_dir_block(dir, dir_blk, blk);
        if (rd_block_op(RDBLK_DIR_START + dir_blk, blk, true) < 0)
            return -1;
    }
    return 0;
}

/// @brief Load superblock + dir region into a caller buffer (no lock).
/// @return 0 ok; -1 unformatted/bad magic/IO; -2 legacy version (fail
///         closed — no free list means a v1 image is not resumable);
///         -3 metadata-region CRC mismatch (directory corrupt).
static int rd_load_all(RdSuper &sup, RdDirEntry *dir) {
    if (rd_read_super(sup) < 0)
        return -1;
    if (sup.magic != RDSB_MAGIC)
        return -1;
    if (sup.version != RDSB_VERSION)
        return -2;
    // Read the whole dir region in one pass and verify its CRC before
    // trusting any entry (issue #318 metadata integrity).
    uint8_t region[RD_DIR_REGION_BYTES] = {};
    CRC32::init();
    uint32_t crc = CRC32::INITIAL;
    for (uint64_t dir_blk = 0; dir_blk < RDBLK_DIR_COUNT; ++dir_blk) {
        if (rd_block_op(RDBLK_DIR_START + dir_blk,
                        region + dir_blk * RD_BLOCK_SIZE, false) < 0)
            return -1;
        crc = CRC32::update(crc, region + dir_blk * RD_BLOCK_SIZE,
                            RD_BLOCK_SIZE);
    }
    if (CRC32::finalize(crc) != sup.dir_crc32)
        return -3;
    __builtin_memcpy(dir, region, RDF_MAX_FILES * sizeof(RdDirEntry));
    return 0;
}

/// @brief Compute the finalized CRC32 over a file's LIVE byte range (block
/// padding excluded).  Never under rd_lock (does block IPC).
/// @return 0 on success ( @p out filled), -1 on IO failure.
static int rd_compute_crc(uint64_t start_block, uint64_t size, uint32_t &out) {
    CRC32::init();
    uint32_t crc = CRC32::INITIAL;
    uint64_t blocks = (size + RD_BLOCK_SIZE - 1) / RD_BLOCK_SIZE;
    uint8_t blk[RD_BLOCK_SIZE] = {};
    for (uint64_t i = 0; i < blocks; ++i) {
        if (rd_block_op_retry(start_block + i, blk, false) < 0)
            return -1;
        uint64_t off = i * RD_BLOCK_SIZE;
        uint64_t n = size - off;
        if (n > RD_BLOCK_SIZE)
            n = RD_BLOCK_SIZE;
        crc = CRC32::update(crc, blk, n);
    }
    out = CRC32::finalize(crc);
    return 0;
}

/// @brief Verify every VALID entry's stored CRC against the blocks on ramdisk
/// in a caller-provided entry array; clear VALID (fail-closed) on any mismatch.
/// Called on the freshly loaded copy BEFORE it is published to the shared
/// cache, so `rd_cached == true` always implies "every visible entry is
/// CRC-verified" and a concurrent lookup can never observe an unverified
/// (possibly corrupt) entry during the fill window (issue #318).  Never under
/// rd_lock (does block IPC).
static void rd_verify_entries(RdDirEntry *dir) {
    for (uint64_t i = 0; i < RDF_MAX_FILES; ++i) {
        if (dir[i].name[0] == '\0' || !(dir[i].flags & RD_ENTRY_VALID))
            continue;
        uint32_t calc = 0;
        bool ok = (rd_compute_crc(dir[i].start_block, dir[i].size, calc) == 0) &&
                  (calc == dir[i].crc32);
        if (!ok) {
            dir[i].flags &= ~RD_ENTRY_VALID;
            Logger::warn("ramdiskfs: CRC mismatch slot=%u (fail-closed)",
                         (unsigned)i);
        }
    }
}

/// @brief Recompute + store a file's CRC, mark it VALID, persist the dir
/// (close-flush; never under rd_lock).  Recompute-per-close, not a byte
/// append: an overwrite at offset 0 makes incremental accumulation wrong.
/// On IO failure the entry stays VALID-cleared (fail-closed).
static void rd_flush_entry(uint64_t dir_index) {
    RdDirEntry e{};
    rd_lock.lock();
    e = rd_dir[dir_index];
    rd_lock.unlock();
    if (e.name[0] == '\0')
        return;
    uint32_t calc = 0;
    if (rd_compute_crc(e.start_block, e.size, calc) < 0)
        return;
    rd_lock.lock();
    if (rd_dir[dir_index].name[0] != '\0' &&
        rd_dir[dir_index].start_block == e.start_block &&
        rd_dir[dir_index].size == e.size) {
        rd_dir[dir_index].crc32 = calc;
        rd_dir[dir_index].flags |= RD_ENTRY_VALID;
    }
    rd_lock.unlock();
    for (uint64_t pt = 0; pt < RD_MUTATE_RETRIES; ++pt) {
        if (rd_persist_slot(dir_index) == 0)
            break;
    }
}

/// @brief Ensure the in-memory cache is populated (no lock held on return).
/// Unformatted stores yield an empty view; formatting is lazy on first
/// create (keeps boot-time mount free of hundreds of block IPCs). The
/// cache is keyed on the daemon pid: a daemon restart (new frames) silently
/// invalidates it, so post-restart reads re-fill instead of serving the
/// dead generation's layout.
static int rd_ensure_cached() {
    uint64_t live_pid = ramdiskd::get_ramdiskd_pid();
    rd_lock.lock();
    bool have = rd_cached && rd_cached_pid == live_pid;
    if (!have)
        rd_cached = false;
    rd_lock.unlock();
    if (have)
        return 0;
    RdSuper sup{};
    RdDirEntry dir[RDF_MAX_FILES]{};
    int loaded = rd_load_all(sup, dir);
    if (loaded < 0) {
        if (loaded == -2)
            Logger::warn("ramdiskfs: legacy layout discarded (fail-closed)");
        else if (loaded == -3)
            Logger::warn("ramdiskfs: directory CRC mismatch (fail-closed)");
        // Ungranted, range error, unformatted, or corrupt: install an empty
        // view.  Formatting happens lazily on first create (fail-closed: all
        // lookups miss until then).
        rd_lock.lock();
        if (!rd_cached) {
            __builtin_memset(&rd_super, 0, sizeof(rd_super));
            __builtin_memset(rd_dir, 0, sizeof(rd_dir));
            rd_super.magic = RDSB_MAGIC;
            rd_super.version = RDSB_VERSION;
            rd_super.next_data_block = RDBLK_DATA_START;
            rd_cached_pid = live_pid;
            rd_cached = true;
        }
        rd_lock.unlock();
        return 0;
    }
    // Verify per-file CRCs on the local copy BEFORE publishing it (issue #318
    // boot check): publishing rd_cached=true only after verification closes the
    // fill/verify race that would otherwise expose an unverified entry to a
    // concurrent lookup.  Concurrent cold callers each re-load + re-verify
    // (bounded, idempotent) and install a fully verified view.
    rd_verify_entries(dir);
    rd_lock.lock();
    if (!rd_cached) {
        rd_super = sup;
        for (uint64_t i = 0; i < RDF_MAX_FILES; ++i)
            rd_dir[i] = dir[i];
        rd_cached_pid = live_pid;
        rd_cached = true;
    }
    rd_lock.unlock();
    return 0;
}

/// @brief Find a directory slot by name in the CACHED dir (lock held).
/// @return Slot index, or -1.
static int64_t rd_find_cached(const char *name) {
    for (uint64_t i = 0; i < RDF_MAX_FILES; ++i) {
        if (rd_dir[i].name[0] == '\0')
            continue;
        if (strcmp(rd_dir[i].name, name) == 0)
            return static_cast<int64_t>(i);
    }
    return -1;
}

/// @brief Validate an upload filename (flat store: no separators).
static bool rd_name_ok(const char *name) {
    if (name == nullptr || name[0] == '\0')
        return false;
    uint64_t len = 0;
    while (name[len] != '\0') {
        if (name[len] == '/')
            return false;
        ++len;
        if (len > RDF_NAME_LEN)
            return false;
    }
    return true;
}

static int rd_file_open(Vnode &, uint64_t) {
    return 0;
}

/// @brief Close a dynamic ramdisk_fs vnode (initrd owned-node pattern).
/// Flushes a dirty size to ramdisk first (best-effort: close carries no
/// error channel; a failed flush leaves the entry stale-sized on disk
/// but coherent in cache — the next mutation re-persists it).
static void rd_file_close(Vnode &self) {
    auto *ext = static_cast<RdExtent *>(self.private_data);
    if (ext != nullptr && ext->dirty) {
        ext->dirty = false;
        // Recompute + persist the entry CRC on close (issue #318).
        rd_flush_entry(ext->dir_index);
    }
    if (self.private_data != nullptr)
        MemPool::free(self.private_data);
    MemPool::free(&self);
}

static int rd_fstat(Vnode &self, VfsStat &st) {
    st.st_size = self.size;
    st.st_mode = self.mode;
    return 0;
}

static int64_t rd_file_lseek(Vnode &, int64_t, int, uint64_t *) {
    return 0;
}

/// @brief Read file data through ramdiskd block IPC (never under rd_lock).
static int64_t rd_file_read(Vnode &self, uint8_t *buffer, uint64_t count,
                            uint64_t offset) {
    auto *ext = static_cast<RdExtent *>(self.private_data);
    if (ext == nullptr || buffer == nullptr)
        return VFS_INVALID;
    // Revalidate the cache generation (daemon restart invalidates it;
    // warm cache costs one lock pair, no IPC).
    if (rd_ensure_cached() < 0)
        return VFS_INVALID;
    rd_lock.lock();
    uint64_t size = self.size;
    uint64_t start = ext->start_block;
    uint64_t blocks = ext->block_count;
    rd_lock.unlock();
    if (offset >= size)
        return 0;
    uint64_t avail = size - offset;
    if (count > avail)
        count = avail;
    uint64_t done = 0;
    while (done < count) {
        uint64_t foff = offset + done;
        uint64_t blk_idx = foff / RD_BLOCK_SIZE;
        uint64_t boff = foff % RD_BLOCK_SIZE;
        if (blk_idx >= blocks)
            return VFS_INVALID;
        uint64_t take = count - done;
        uint64_t room = RD_BLOCK_SIZE - boff;
        if (take > room)
            take = room;
        uint8_t blk[RD_BLOCK_SIZE] = {};
        if (rd_block_op_retry(start + blk_idx, blk, false) < 0)
            return VFS_INVALID;
        __builtin_memcpy(buffer + done, blk + boff, take);
        done += take;
    }
    return static_cast<int64_t>(done);
}

/// @brief Write file data (grows size; persists entry on close).
static int64_t rd_file_write(Vnode &self, const uint8_t *buffer,
                             uint64_t count, uint64_t offset) {
    auto *ext = static_cast<RdExtent *>(self.private_data);
    if (ext == nullptr || buffer == nullptr)
        return VFS_INVALID;
    uint64_t needed = offset + count;
    if (needed > RDF_MAX_FILE_SIZE)
        return VFS_INVALID;
    // Revalidate the cache generation (see rd_file_read).
    if (rd_ensure_cached() < 0)
        return VFS_INVALID;
    // Snapshot extent + reserve growth under the lock (no IPC held).
    rd_lock.lock();
    // Sparse writes (offset past end) are refused fail-closed: the store
    // never zero-fills gaps, so accepting them would expose stale block
    // content in the hole. All in-tree writers are sequential.
    if (offset > rd_dir[ext->dir_index].size) {
        rd_lock.unlock();
        return VFS_INVALID;
    }
    uint64_t start = ext->start_block;
    uint64_t have_blocks = ext->block_count;
    uint64_t dir_index = ext->dir_index;
    uint64_t want_blocks = (needed + RD_BLOCK_SIZE - 1) / RD_BLOCK_SIZE;
    if (needed == 0)
        want_blocks = 0;
    if (want_blocks > have_blocks) {
        uint64_t add = want_blocks - have_blocks;
        if (rd_super.next_data_block + add >
            ramdiskd::RAMDISK_BLOCKS) {
            rd_lock.unlock();
            return VFS_INVALID;
        }
        // New files always start at next_data_block (bump); existing
        // files extend contiguously (single-writer uploader discipline —
        // concurrent same-file writers are NOT supported, see header).
        if (have_blocks == 0)
            start = rd_super.next_data_block;
        else if (start + have_blocks != rd_super.next_data_block) {
            rd_lock.unlock();
            return VFS_INVALID;
        }
        rd_super.next_data_block += add;
        ext->start_block = start;
        ext->block_count = want_blocks;
        rd_dir[dir_index].start_block = start;
        rd_dir[dir_index].block_count = want_blocks;
    }
    rd_lock.unlock();
    // Data transfer without the lock (partial edge blocks read-modify-write).
    uint64_t done = 0;
    while (done < count) {
        uint64_t foff = offset + done;
        uint64_t blk_idx = foff / RD_BLOCK_SIZE;
        uint64_t boff = foff % RD_BLOCK_SIZE;
        uint64_t take = count - done;
        uint64_t room = RD_BLOCK_SIZE - boff;
        if (take > room)
            take = room;
        uint8_t blk[RD_BLOCK_SIZE] = {};
        if (boff != 0 || take != RD_BLOCK_SIZE) {
            if (rd_block_op_retry(start + blk_idx, blk, false) < 0)
                return VFS_INVALID;
        }
        __builtin_memcpy(blk + boff, buffer + done, take);
        if (rd_block_op_retry(start + blk_idx, blk, true) < 0)
            return VFS_INVALID;
        done += take;
    }
    // Publish the new size in the cache.  Any content write marks the entry
    // in-flight (VALID cleared) until close recomputes + re-persists its CRC
    // (issue #318); a torn/incomplete write is therefore never exposed.
    rd_lock.lock();
    if (needed > self.size)
        self.size = needed;
    if (needed > rd_dir[dir_index].size)
        rd_dir[dir_index].size = needed;
    if (count > 0) {
        ext->dirty = true;
        rd_dir[dir_index].flags &= ~RD_ENTRY_VALID;
    }
    rd_lock.unlock();
    return static_cast<int64_t>(done);
}

/// @brief Look up a child by name (fresh owned vnode, nullptr on miss).
/// File vnodes carry the FILE ops table (the root table has null
/// read/write — assigning it would null-call on use).
static const VnodeOps rd_file_ops = {
    rd_file_read,  rd_file_write, rd_file_open,
    rd_file_close, rd_file_lseek, rd_fstat,
    nullptr, // ioctl
    nullptr, // readdir
    nullptr, // lookup
    nullptr, // mkdir
    nullptr, // unlink
    nullptr, // create
};

static Vnode *rd_lookup(Vnode &self, const char *name) {
    if (!(self.mode & S_IFDIR))
        return nullptr;
    if (rd_ensure_cached() < 0)
        return nullptr;
    rd_lock.lock();
    int64_t slot = rd_find_cached(name);
    if (slot < 0) {
        rd_lock.unlock();
        return nullptr;
    }
    RdDirEntry entry = rd_dir[slot];
    rd_lock.unlock();
    // Fail closed: only expose an entry whose CRC was verified (VALID).
    if (!(entry.flags & RD_ENTRY_VALID))
        return nullptr;
    auto *ext = static_cast<RdExtent *>(MemPool::alloc(sizeof(RdExtent)));
    if (ext == nullptr)
        return nullptr;
    auto *vn = static_cast<Vnode *>(MemPool::alloc(sizeof(Vnode)));
    if (vn == nullptr) {
        MemPool::free(ext);
        return nullptr;
    }
    ext->start_block = entry.start_block;
    ext->block_count = entry.block_count;
    ext->dir_index = static_cast<uint64_t>(slot);
    ext->dirty = false;
    vn->ops = &rd_file_ops;
    rd_lock.lock();
    vn->ino = rd_next_ino++;
    rd_lock.unlock();
    vn->size = entry.size;
    vn->mode = S_IFREG;
    vn->private_data = ext;
    vn->refcount = 1;
    vn->parent = &self;
    return vn;
}

/// @brief Read a directory entry (cache scan under lock).
static int rd_readdir(Vnode &self, uint64_t &pos, Dirent &dent) {
    if (!(self.mode & S_IFDIR))
        return VFS_INVALID;
    if (rd_ensure_cached() < 0)
        return VFS_INVALID;
    rd_lock.lock();
    uint64_t idx = 0;
    for (uint64_t i = 0; i < RDF_MAX_FILES; ++i) {
        if (rd_dir[i].name[0] == '\0' ||
            !(rd_dir[i].flags & RD_ENTRY_VALID))
            continue;
        if (idx == pos) {
            uint64_t k = 0;
            while (rd_dir[i].name[k] != '\0' && k < sizeof(dent.d_name) - 1) {
                dent.d_name[k] = rd_dir[i].name[k];
                ++k;
            }
            dent.d_name[k] = '\0';
            dent.d_ino = i + 1;
            ++pos;
            rd_lock.unlock();
            return 0;
        }
        ++idx;
    }
    rd_lock.unlock();
    return VFS_INVALID;
}

/// @brief Persist one dir slot + superblock (never under rd_lock).
static int rd_persist_slot(uint64_t slot) {
    RdSuper sup{};
    RdDirEntry dir[RDF_MAX_FILES]{};
    rd_lock.lock();
    sup = rd_super;
    for (uint64_t i = 0; i < RDF_MAX_FILES; ++i)
        dir[i] = rd_dir[i];
    rd_lock.unlock();
    (void)slot;
    return rd_store_all(sup, dir);
}

static int rd_unlink(Vnode &self, const char *name);
static int rd_create(Vnode &self, const char *name, uint16_t mode);

/// @brief Create a regular file (visible immediately — see header on the
/// torn-image safety chain: uploader verify + loader validation).
static int rd_create(Vnode &self, const char *name, uint16_t) {
    if (!(self.mode & S_IFDIR))
        return VFS_INVALID;
    if (!rd_name_ok(name))
        return VFS_INVALID;
    for (uint64_t attempt = 0; attempt < RD_MUTATE_RETRIES; ++attempt) {
        if (rd_ensure_cached() < 0)
            return VFS_INVALID;
        rd_lock.lock();
        uint64_t epoch = rd_epoch;
        if (rd_find_cached(name) >= 0) {
            rd_lock.unlock();
            return VFS_INVALID;
        }
        int64_t free_slot = -1;
        for (uint64_t i = 0; i < RDF_MAX_FILES; ++i) {
            if (rd_dir[i].name[0] == '\0') {
                free_slot = static_cast<int64_t>(i);
                break;
            }
        }
        if (free_slot < 0) {
            rd_lock.unlock();
            return VFS_INVALID;
        }
        uint64_t slot = static_cast<uint64_t>(free_slot);
        __builtin_memset(&rd_dir[slot], 0, sizeof(RdDirEntry));
        uint64_t k = 0;
        while (name[k] != '\0' && k < sizeof(rd_dir[slot].name) - 1) {
            rd_dir[slot].name[k] = name[k];
            ++k;
        }
        rd_dir[slot].name[k] = '\0';
        rd_dir[slot].size = 0;
        rd_dir[slot].start_block = rd_super.next_data_block;
        rd_dir[slot].block_count = 0;
        // Empty file: CRC of zero bytes is 0 (finalize(INITIAL)); VALID so
        // it is immediately resolvable (issue #318).
        rd_dir[slot].crc32 = CRC32::finalize(CRC32::INITIAL);
        rd_dir[slot].flags = RD_ENTRY_VALID;
        rd_super.file_count += 1;
        rd_lock.unlock();
        // Persist outside the lock (same content, bounded retries: the
        // first persist may race daemon-side cold state, e.g. the first
        // lazy segment map under TCG; the mapping persists, so a retry
        // with identical bytes converges).
        bool stored = false;
        for (uint64_t pt = 0; pt < RD_MUTATE_RETRIES; ++pt) {
            if (rd_persist_slot(slot) == 0) {
                stored = true;
                break;
            }
        }
        if (!stored) {
            rd_lock.lock();
            rd_cached = false;
            rd_lock.unlock();
            continue;
        }
        rd_lock.lock();
        if (rd_epoch != epoch) {
            // Lost a race: our entry is on ramdisk; invalidate the cache
            // so the retry re-reads (and then reports EXISTS for it).
            rd_cached = false;
            rd_lock.unlock();
            continue;
        }
        rd_epoch = epoch + 1;
        rd_lock.unlock();
        return 0;
    }
    return VFS_INVALID;
}

/// @brief Remove a file (clears the slot; data blocks are NOT reclaimed —
/// no free list by design, documented in the header).
static int rd_unlink(Vnode &self, const char *name) {
    if (!(self.mode & S_IFDIR))
        return VFS_INVALID;
    if (!rd_name_ok(name))
        return VFS_INVALID;
    for (uint64_t attempt = 0; attempt < RD_MUTATE_RETRIES; ++attempt) {
        if (rd_ensure_cached() < 0)
            return VFS_INVALID;
        rd_lock.lock();
        uint64_t epoch = rd_epoch;
        int64_t slot = rd_find_cached(name);
        if (slot < 0) {
            rd_lock.unlock();
            return VFS_INVALID;
        }
        uint64_t s = static_cast<uint64_t>(slot);
        __builtin_memset(&rd_dir[s], 0, sizeof(RdDirEntry));
        if (rd_super.file_count > 0)
            rd_super.file_count -= 1;
        rd_lock.unlock();
        bool stored = false;
        for (uint64_t pt = 0; pt < RD_MUTATE_RETRIES; ++pt) {
            if (rd_persist_slot(s) == 0) {
                stored = true;
                break;
            }
        }
        if (!stored) {
            rd_lock.lock();
            rd_cached = false;
            rd_lock.unlock();
            continue;
        }
        rd_lock.lock();
        if (rd_epoch != epoch) {
            rd_cached = false;
            rd_lock.unlock();
            continue;
        }
        rd_epoch = epoch + 1;
        rd_lock.unlock();
        return 0;
    }
    return VFS_INVALID;
}

/// @brief Flat store rejects subdirectories.
static int rd_mkdir(Vnode &, const char *, uint16_t) {
    return VFS_INVALID;
}

static const VnodeOps rd_root_ops = {
    nullptr, // read
    nullptr, // write
    nullptr, // open
    nullptr, // close (static root: close is a no-op by ownership rules)
    nullptr, // lseek
    rd_fstat,
    nullptr, // ioctl
    rd_readdir, rd_lookup, rd_mkdir, rd_unlink, rd_create,
};

/// @brief Get the ramdisk_fs root vnode (static, borrowed).
static Vnode *rd_get_root() {
    static bool inited = false;
    if (!inited) {
        rd_root.ops = &rd_root_ops;
        rd_root.ino = 0;
        rd_root.size = 0;
        rd_root.mode = S_IFDIR;
        rd_root.private_data = nullptr;
        rd_root.refcount = 0;
        rd_root.parent = nullptr;
        inited = true;
    }
    return &rd_root;
}

/// @brief Verify every live file's CRC against ramdisk (test seam — the same
/// per-file check the boot cache-fill runs).  @return mismatch count (0 clean;
/// 0 also when the store is unavailable, which presents an empty view).
int ramdisk_fs_verify_all() {
    if (rd_ensure_cached() < 0)
        return -1;
    int mismatches = 0;
    for (uint64_t i = 0; i < RDF_MAX_FILES; ++i) {
        RdDirEntry e{};
        rd_lock.lock();
        e = rd_dir[i];
        rd_lock.unlock();
        if (e.name[0] == '\0' || !(e.flags & RD_ENTRY_VALID))
            continue;
        uint32_t calc = 0;
        if (rd_compute_crc(e.start_block, e.size, calc) < 0 ||
            calc != e.crc32) {
            ++mismatches;
            // Fail closed: clear VALID in the cache so subsequent resolves
            // refuse the entry (issue #318).  Persisted copy is untouched and
            // re-verified on the next cache fill.
            rd_lock.lock();
            if (rd_dir[i].name[0] != '\0' &&
                rd_dir[i].start_block == e.start_block &&
                rd_dir[i].size == e.size)
                rd_dir[i].flags &= ~RD_ENTRY_VALID;
            rd_lock.unlock();
        }
    }
    return mismatches;
}

/// @brief Test seam: fetch a file's extent (start block, block count, stored
/// CRC).  @return true when a live entry with that name exists.
bool ramdisk_fs_extent(const char *name, uint64_t &start_block,
                       uint64_t &block_count, uint32_t &crc) {
    if (name == nullptr || rd_ensure_cached() < 0)
        return false;
    rd_lock.lock();
    int64_t slot = rd_find_cached(name);
    if (slot < 0) {
        rd_lock.unlock();
        return false;
    }
    start_block = rd_dir[slot].start_block;
    block_count = rd_dir[slot].block_count;
    crc = rd_dir[slot].crc32;
    rd_lock.unlock();
    return true;
}

int ramdisk_fs_try_mount() {
    // Grant check FIRST: an already-mounted store with a dead/cleared
    // grant must report failure (fail-closed degraded), not success.
    if (ramdiskd::get_ramdiskd_pid() == 0)
        return VFS_INVALID;
    if (find_fs("ramdiskfs") != nullptr)
        return 0;
    if (mount(ramdisk_fs, "/mnt/ramdisk") != 0)
        return VFS_INVALID;
    return 0;
}

Filesystem ramdisk_fs = {
    "ramdiskfs",
    rd_get_root,
};

} // namespace vfs
} // namespace kernel
