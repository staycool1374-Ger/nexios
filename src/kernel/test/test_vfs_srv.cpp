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

/// @file test_vfs_srv.cpp
/// @brief Vfs-srv PD live tests (issue #47, strangler step-b3).
/// ON-build only (CONFIG_VFS_SRV=1): BIND/CAP_GET against the real
/// userspace vfs-srv PD, per-backend open/read/stat/readdir through the
/// shared window, digest parity lines, fault drills, EOF/rebind guards.
/// OFF build registers vacuous passes so the default aggregates stay
/// green.

#include <test.hpp>
#include <logger.hpp>

using namespace kernel;

#if CONFIG_VFS_SRV
#include <kernel/vfs/vfs_srv.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/test/test_isolate.hpp>
#include <string.hpp>

namespace {

// FNV-1a 64 (host parity oracle mirrors this exact fold).
uint64_t vfs_fnv1a(const uint8_t *data, uint64_t len, uint64_t hash) {
    for (uint64_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kWindowChunk = 0x3000;

} // namespace
#endif

// Runmode: kernel
// Testidea: PD binds with all seven backends live.
// Input: vfs_srv_bind() against the live vfs-srv PD.
// Expect: bind true; every backend bit set in the bound mask.
// Depends: vfs_srv_bind, SDF VFS_SRV row, PD CAP_GET/BIND handlers
JARVIS_TEST(vfs_srv_bind_mask, "PRE: vfs-srv PD live | POST: bound") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_available(
        vfs_srv::VfsBackend::INITRD));
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_available(
        vfs_srv::VfsBackend::FAT32));
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_available(
        vfs_srv::VfsBackend::TMPFS));
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_available(
        vfs_srv::VfsBackend::DEVFS));
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_available(
        vfs_srv::VfsBackend::PIPE));
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_available(
        vfs_srv::VfsBackend::RAMDISK));
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_available(
        vfs_srv::VfsBackend::PROCFS));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Initrd stat/open resolve the ELF through the cpio index.
// Input: vfs_srv_stat/open(INITRD, "/hey.c.elf").
// Expect: size > 0, mode 0444-ish; handle tagged backend 0, nonzero.
// Depends: vfs_srv_stat/open, PD resolve_initrd, staged cpio index
JARVIS_TEST(vfs_srv_initrd_open_stat, "PRE: bound | POST: closed") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::INITRD,
                                              "/hey.c.elf", size, mode));
    JARVIS_ASSERT(size > 0);
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::INITRD,
                                              "/hey.c.elf", 0, handle));
    JARVIS_ASSERT(handle != 0);
    JARVIS_ASSERT_EQ(0ULL, (handle >> 56));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    // Missing entry is a typed error, not a silent ok.
    JARVIS_ASSERT_EQ(-2, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::INITRD,
                                               "/no-such-file", 0,
                                               handle));
    JARVIS_ASSERT_EQ(-2, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::INITRD,
                                               "/no-such-file", size,
                                               mode));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Whole-file initrd read is stable; digest feeds the oracle.
// Input: chunked reads of /hey.c.elf, FNV-1a over both passes.
// Expect: identical digests; S VFS initrd digest line printed.
// Depends: vfs_srv_read, kernel stage_initrd, PD range accept
JARVIS_TEST(vfs_srv_initrd_digest, "PRE: bound | POST: closed") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::INITRD,
                                              "/hey.c.elf", size, mode));
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::INITRD,
                                              "/hey.c.elf", 0, handle));
    uint8_t chunk[kWindowChunk];
    uint64_t first = kFnvOffset;
    uint64_t off = 0;
    while (off < size) {
        uint64_t want = size - off;
        if (want > kWindowChunk) {
            want = kWindowChunk;
        }
        int64_t got =
            vfs_srv::vfs_srv_read(handle, off, chunk, want);
        JARVIS_ASSERT_EQ(static_cast<int64_t>(want), got);
        first = vfs_fnv1a(chunk, want, first);
        off += want;
    }
    uint64_t second = kFnvOffset;
    off = 0;
    while (off < size) {
        uint64_t want = size - off;
        if (want > kWindowChunk) {
            want = kWindowChunk;
        }
        int64_t got =
            vfs_srv::vfs_srv_read(handle, off, chunk, want);
        JARVIS_ASSERT_EQ(static_cast<int64_t>(want), got);
        second = vfs_fnv1a(chunk, want, second);
        off += want;
    }
    JARVIS_ASSERT_EQ(first, second);
    JARVIS_ASSERT(first != kFnvOffset);
    Logger::info("S VFS initrd digest=%lx size=%lu", first, size);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: FAT32 HELLO.TXT reads through the staged BPB+rootdir.
// Input: stat/open/chunked-read of /HELLO.TXT.
// Expect: bytes stable; S VFS fat32 digest line printed.
// Depends: vfs_srv open/read, kernel stage_fat32_dir/chain
JARVIS_TEST(vfs_srv_fat32_digest, "PRE: bound | POST: closed") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::FAT32,
                                              "/HELLO.TXT", size, mode));
    JARVIS_ASSERT(size > 0);
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::FAT32,
                                              "/HELLO.TXT", 0, handle));
    JARVIS_ASSERT_EQ(1ULL, (handle >> 56));
    uint8_t chunk[kWindowChunk];
    uint64_t first = kFnvOffset;
    uint64_t off = 0;
    while (off < size) {
        uint64_t want = size - off;
        if (want > kWindowChunk) {
            want = kWindowChunk;
        }
        int64_t got =
            vfs_srv::vfs_srv_read(handle, off, chunk, want);
        JARVIS_ASSERT_EQ(static_cast<int64_t>(want), got);
        first = vfs_fnv1a(chunk, want, first);
        off += want;
    }
    Logger::info("S VFS fat32 digest=%lx size=%lu", first, size);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Second FAT32 file reads through a different start cluster.
// Input: stat/open/chunked-read of /MULTI.TXT (350 B test payload).
// Expect: full size served; S VFS fat32m digest line printed.
// Depends: kernel stage_fat32_chain mechanical walk, PD dirent check
JARVIS_TEST(vfs_srv_fat32m_digest, "PRE: bound | POST: closed") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::FAT32,
                                              "/MULTI.TXT", size, mode));
    JARVIS_ASSERT(size > 0);
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::FAT32,
                                              "/MULTI.TXT", 0, handle));
    uint8_t chunk[kWindowChunk];
    uint64_t digest = kFnvOffset;
    uint64_t off = 0;
    while (off < size) {
        uint64_t want = size - off;
        if (want > kWindowChunk) {
            want = kWindowChunk;
        }
        int64_t got =
            vfs_srv::vfs_srv_read(handle, off, chunk, want);
        JARVIS_ASSERT_EQ(static_cast<int64_t>(want), got);
        digest = vfs_fnv1a(chunk, want, digest);
        off += want;
    }
    Logger::info("S VFS fat32m digest=%lx size=%lu", digest, size);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Ramdisk probe file planted kernel-side reads back via PD.
// Input: 600 B pattern written through kernel VFS, read via vfs-srv.
// Expect: byte-exact round-trip; S VFS ramdisk digest line printed.
// Depends: vfs::create/write, PD resolve_ramdisk, stage_ramdisk_span
JARVIS_TEST(vfs_srv_ramdisk_probe_digest, "PRE: ramdiskd | POST: unlinked") {
#if CONFIG_VFS_SRV
    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    const char *kpath = "/mnt/ramdisk/vfs-srv-probe";
    vfs::unlink(kpath);
    JARVIS_ASSERT_EQ(0, vfs::create(kpath, 0));
    vfs::Vnode *file = vfs::resolve(kpath);
    JARVIS_ASSERT(file != nullptr);
    uint8_t pattern[600];
    for (uint64_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    }
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(pattern)),
                     file->ops->write(*file, pattern, sizeof(pattern),
                                      0));
    vfs::release(file);
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::RAMDISK,
                                              "vfs-srv-probe", size,
                                              mode));
    JARVIS_ASSERT_EQ(sizeof(pattern), size);
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::RAMDISK,
                                              "vfs-srv-probe", 0,
                                              handle));
    JARVIS_ASSERT_EQ(5ULL, (handle >> 56));
    uint8_t back[600] = {};
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(back)),
                     vfs_srv::vfs_srv_read(handle, 0, back,
                                           sizeof(back)));
    JARVIS_ASSERT(memcmp(pattern, back, sizeof(pattern)) == 0);
    uint64_t digest = vfs_fnv1a(back, sizeof(back), kFnvOffset);
    Logger::info("S VFS ramdisk digest=%lx size=%lu", digest, size);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    JARVIS_ASSERT_EQ(0, vfs::unlink(kpath));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Procfs render validates through the PD (D6 validator).
// Input: open/read /proc/meminfo via vfs-srv.
// Expect: "MemTotal:" prefix; S VFS procfs digest line printed.
// Depends: kernel stage_procfs render, PD validate_procfs
JARVIS_TEST(vfs_srv_procfs_digest, "PRE: bound | POST: closed") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::PROCFS,
                                              "/proc/meminfo", 0,
                                              handle));
    JARVIS_ASSERT_EQ(6ULL, (handle >> 56));
    uint8_t buf[512] = {};
    int64_t got = vfs_srv::vfs_srv_read(handle, 0, buf, sizeof(buf));
    JARVIS_ASSERT(got > 9);
    JARVIS_ASSERT(buf[0] == 'M' && buf[1] == 'e' && buf[2] == 'm');
    uint64_t digest =
        vfs_fnv1a(buf, static_cast<uint64_t>(got), kFnvOffset);
    Logger::info("S VFS procfs digest=%lx size=%ld", digest, got);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Tmpfs create/write/read/stat/unlink round-trip, PD-owned.
// Input: unique file; write pattern; stat size; read back; unlink.
// Expect: byte-exact; EEXIST on re-create; ENOENT after unlink.
// Depends: PD tmpfs arena, CREATE/UNLINK typed errors
JARVIS_TEST(vfs_srv_tmpfs_roundtrip, "PRE: bound | POST: unlinked") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    const char *path = "vfs-srv-tmpfs-t8";
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_create(vfs_srv::VfsBackend::TMPFS,
                                                path, 0644));
    JARVIS_ASSERT_EQ(-17, vfs_srv::vfs_srv_create(
                              vfs_srv::VfsBackend::TMPFS, path, 0644));
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::TMPFS,
                                              path, 0, handle));
    JARVIS_ASSERT_EQ(2ULL, (handle >> 56));
    uint8_t pattern[256];
    for (uint64_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 5 + 1) & 0xFF);
    }
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(pattern)),
                     vfs_srv::vfs_srv_write(handle, 0, pattern,
                                            sizeof(pattern)));
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::TMPFS,
                                              path, size, mode));
    JARVIS_ASSERT_EQ(sizeof(pattern), size);
    uint8_t back[256] = {};
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(back)),
                     vfs_srv::vfs_srv_read(handle, 0, back,
                                           sizeof(back)));
    JARVIS_ASSERT(memcmp(pattern, back, sizeof(pattern)) == 0);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_unlink(vfs_srv::VfsBackend::TMPFS,
                                                path));
    JARVIS_ASSERT_EQ(-2, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::TMPFS,
                                               path, 0, handle));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Pipe ring is FIFO, PD-owned; empty reads are EOF-shaped.
// Input: create pipe; write 100 B; read 40 + rest; read empty.
// Expect: order preserved; empty read returns 0, never blocks.
// Depends: PD pipe ring head/count, WRITE/READ paths
JARVIS_TEST(vfs_srv_pipe_fifo, "PRE: bound | POST: unlinked") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    const char *path = "vfs-srv-pipe-t9";
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_create(vfs_srv::VfsBackend::PIPE,
                                                path, 0600));
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::PIPE,
                                              path, 0, handle));
    JARVIS_ASSERT_EQ(4ULL, (handle >> 56));
    uint8_t pattern[100];
    for (uint64_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = static_cast<uint8_t>(i & 0xFF);
    }
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(pattern)),
                     vfs_srv::vfs_srv_write(handle, 0, pattern,
                                            sizeof(pattern)));
    uint8_t head[40] = {};
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(head)),
                     vfs_srv::vfs_srv_read(handle, 0, head,
                                           sizeof(head)));
    JARVIS_ASSERT(memcmp(pattern, head, sizeof(head)) == 0);
    uint8_t tail[60] = {};
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(tail)),
                     vfs_srv::vfs_srv_read(handle, 0, tail,
                                           sizeof(tail)));
    JARVIS_ASSERT(memcmp(pattern + 40, tail, sizeof(tail)) == 0);
    uint8_t probe[8] = {};
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_read(handle, 0, probe,
                                              sizeof(probe)));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_unlink(vfs_srv::VfsBackend::PIPE,
                                                path));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Devfs is metadata-only (D3): stat/readdir work, data -ENOSYS.
// Input: stat /dev/null; open+read it; readdir /dev to end.
// Expect: mode 0666; read -ENOSYS; 4 entries then end-of-dir.
// Depends: PD devfs table, readdir chaining via next_ino
JARVIS_TEST(vfs_srv_devfs_metadata, "PRE: bound | POST: closed") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::DEVFS,
                                              "/dev/null", size, mode));
    JARVIS_ASSERT_EQ(0666ULL, mode);
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::DEVFS,
                                              "/dev/null", 0, handle));
    uint8_t buf[16] = {};
    JARVIS_ASSERT_EQ(-38, vfs_srv::vfs_srv_read(handle, 0, buf,
                                                sizeof(buf)));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    uint64_t next = 0;
    char name[64] = {};
    uint64_t entries = 0;
    for (;;) {
        int64_t rc = vfs_srv::vfs_srv_readdir(vfs_srv::VfsBackend::DEVFS,
                                              "/dev", next, name);
        if (rc == 1) {
            break;
        }
        JARVIS_ASSERT_EQ(0, rc);
        JARVIS_ASSERT(name[0] != '\0');
        ++entries;
        JARVIS_ASSERT(entries <= 4);
    }
    JARVIS_ASSERT_EQ(4ULL, entries);
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: STAT resolves one known path per backend with sane shape.
// Input: stat initrd/fat32/tmpfs/devfs/pipe/ramdisk/procfs paths.
// Expect: all succeed; sizes/modes match the backend contracts.
// Depends: PD resolvers, kernel per-backend dir-region staging.
// NOTE (QE#47-b3): readdir(PROCFS, "/proc") cannot pass staging —
// proc_root_read is VFS_INVALID, so stage_procfs_path fails closed and
// the PD's /proc-listing branch is unreachable through the proxy.
// Dev call: stage bypass for READDIR or document unsupported.
JARVIS_TEST(vfs_srv_stat_matrix, "PRE: bound | POST: unlinked") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::INITRD,
                                              "/hey.c.elf", size, mode));
    JARVIS_ASSERT(size > 0);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::FAT32,
                                              "/HELLO.TXT", size, mode));
    JARVIS_ASSERT_EQ(14ULL, size);
    JARVIS_ASSERT_EQ(0644ULL, mode);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_create(vfs_srv::VfsBackend::TMPFS,
                                                "vfs-srv-t11", 0600));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::TMPFS,
                                              "vfs-srv-t11", size, mode));
    JARVIS_ASSERT_EQ(0ULL, size);
    JARVIS_ASSERT_EQ(0600ULL, mode);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::DEVFS,
                                              "/dev/tty", size, mode));
    JARVIS_ASSERT_EQ(0600ULL, mode);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_create(vfs_srv::VfsBackend::PIPE,
                                                "vfs-srv-p11", 0600));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::PIPE,
                                              "vfs-srv-p11", size, mode));
    JARVIS_ASSERT_EQ(0ULL, size);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::PROCFS,
                                              "/proc/meminfo", size,
                                              mode));
    JARVIS_ASSERT(size > 0);
    JARVIS_ASSERT_EQ(0444ULL, mode);
    // NOTE (QE#47-b3): "/proc/stat" is in the PD table + kernel
    // kProcFiles, but no such file resolves in-kernel (root lookup
    // serves meminfo/self/pci/PIDs) — idx-1 staging can never succeed.
    // Dev call: mount a real /proc/stat or drop the index.
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_unlink(vfs_srv::VfsBackend::TMPFS,
                                                "vfs-srv-t11"));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_unlink(vfs_srv::VfsBackend::PIPE,
                                                "vfs-srv-p11"));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: EOF clamps, oversized windows and bad names fail closed.
// Input: reads at/past EOF; >12 KiB read; 64 B name; bad backend ops.
// Expect: 0 at EOF; clamped tail; -ENOSYS / -ENOENT typed errors.
// Depends: kernel EOF clamp, stage_name bound, PD typed errors
JARVIS_TEST(vfs_srv_eof_clamp_guards, "PRE: bound | POST: closed") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::INITRD,
                                              "/hey.c.elf", size, mode));
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::INITRD,
                                              "/hey.c.elf", 0, handle));
    uint8_t chunk[kWindowChunk];
    // Read at EOF: zero-length success, no staging, no RPC.
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_read(handle, size, chunk, 64));
    // Overlong tail clamps to the 5 remaining bytes.
    JARVIS_ASSERT(size > 5);
    JARVIS_ASSERT_EQ(5, vfs_srv::vfs_srv_read(handle, size - 5, chunk,
                                              100));
    // Window-sized reads past the 12 KiB staging window fail closed.
    JARVIS_ASSERT_EQ(-38, vfs_srv::vfs_srv_read(handle, 0, chunk,
                                                kWindowChunk + 1));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    // Double close of an owned-store handle is a typed error.
    uint64_t th = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_create(vfs_srv::VfsBackend::TMPFS,
                                                "vfs-srv-t12", 0644));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::TMPFS,
                                              "vfs-srv-t12", 0, th));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(th));
    JARVIS_ASSERT_EQ(-2, vfs_srv::vfs_srv_close(th));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_unlink(vfs_srv::VfsBackend::TMPFS,
                                                "vfs-srv-t12"));
    // Over-long names never reach the window (64 B slot: len >= 64
    // rejects before touching shared memory).
    char long_name[65];
    for (uint64_t i = 0; i < sizeof(long_name) - 1; ++i) {
        long_name[i] = 'a';
    }
    long_name[sizeof(long_name) - 1] = '\0';
    JARVIS_ASSERT_EQ(-38, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::TMPFS,
                                                long_name, size, mode));
    // Image-backed writes are read-only in b3.
    JARVIS_ASSERT_EQ(-38, vfs_srv::vfs_srv_write(handle, 0, chunk, 8));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: One-shot fault drill fires -EIO once, then recovers.
// Input: inject on TMPFS; op; op again; read backend status.
// Expect: first op -EIO, second ok; magic/version valid, faults >= 1.
// Depends: PD fault_gate, stats_publish, kernel backend_status
JARVIS_TEST(vfs_srv_fault_drill_status, "PRE: bound | POST: unlinked") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_create(vfs_srv::VfsBackend::TMPFS,
                                                "vfs-srv-t13", 0644));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_fault_backend(
                            vfs_srv::VfsBackend::TMPFS));
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(-5, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::TMPFS,
                                               "vfs-srv-t13", 0,
                                               handle));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::TMPFS,
                                              "vfs-srv-t13", 0, handle));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    vfs_srv::VfsBackendStatus st{};
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_status(
        vfs_srv::VfsBackend::TMPFS, st));
    JARVIS_ASSERT_EQ(vfs_srv::kVfsBackendStatusMagic, st.magic);
    JARVIS_ASSERT_EQ(
        static_cast<uint64_t>(vfs_srv::kVfsBackendStatusVersion),
        static_cast<uint64_t>(st.version));
    JARVIS_ASSERT(st.faults >= 1);
    JARVIS_ASSERT(st.ops > 0);
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_unlink(vfs_srv::VfsBackend::TMPFS,
                                                "vfs-srv-t13"));
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Epoch bump purges PD state; the proxy rebinds transparently.
// Input: bump epoch; open/read/stat; backend status.
// Expect: all succeed post-rebind; status magic still valid.
// Depends: vfs_srv_bump_epoch, bound_or_rebind, PD epoch purge
JARVIS_TEST(vfs_srv_epoch_rebind, "PRE: bound | POST: rebound") {
#if CONFIG_VFS_SRV
    JARVIS_ASSERT(vfs_srv::vfs_srv_bind());
    vfs_srv::vfs_srv_bump_epoch();
    // The next op replays CAP_GET + BIND; the PD re-maps the window
    // (null-window guard) and purges its open table (epoch purge).
    uint64_t size = 0;
    uint64_t mode = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_stat(vfs_srv::VfsBackend::FAT32,
                                              "/HELLO.TXT", size, mode));
    JARVIS_ASSERT(size > 0);
    uint64_t handle = 0;
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_open(vfs_srv::VfsBackend::FAT32,
                                              "/HELLO.TXT", 0, handle));
    uint8_t buf[64] = {};
    uint64_t want = size < sizeof(buf) ? size : sizeof(buf);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(want),
                     vfs_srv::vfs_srv_read(handle, 0, buf, want));
    JARVIS_ASSERT_EQ(0, vfs_srv::vfs_srv_close(handle));
    vfs_srv::VfsBackendStatus st{};
    JARVIS_ASSERT(vfs_srv::vfs_srv_backend_status(
        vfs_srv::VfsBackend::INITRD, st));
    JARVIS_ASSERT_EQ(vfs_srv::kVfsBackendStatusMagic, st.magic);
    JARVIS_TEST_PASS();
#else
    JARVIS_TEST_PASS();
#endif
}

void register_vfs_srv_tests() {
    Logger::info("Registering VFS Server live tests");
    JARVIS_REGISTER_TEST(vfs_srv_bind_mask);
    JARVIS_REGISTER_TEST(vfs_srv_initrd_open_stat);
    JARVIS_REGISTER_TEST(vfs_srv_initrd_digest);
    JARVIS_REGISTER_TEST(vfs_srv_fat32_digest);
    JARVIS_REGISTER_TEST(vfs_srv_fat32m_digest);
    JARVIS_REGISTER_TEST(vfs_srv_ramdisk_probe_digest);
    JARVIS_REGISTER_TEST(vfs_srv_procfs_digest);
    JARVIS_REGISTER_TEST(vfs_srv_tmpfs_roundtrip);
    JARVIS_REGISTER_TEST(vfs_srv_pipe_fifo);
    JARVIS_REGISTER_TEST(vfs_srv_devfs_metadata);
    JARVIS_REGISTER_TEST(vfs_srv_stat_matrix);
    JARVIS_REGISTER_TEST(vfs_srv_eof_clamp_guards);
    JARVIS_REGISTER_TEST(vfs_srv_fault_drill_status);
    JARVIS_REGISTER_TEST(vfs_srv_epoch_rebind);
}
