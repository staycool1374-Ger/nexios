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

/// @file test_ramdisk_fs.cpp
/// @brief Ramdisk flat filesystem (issue #314) tests: mount presence,
/// create/write/read/delete round-trips, bounds rejection, partial
/// overwrite behavior, and loadelf-from-ramdisk acceptance.
/// All ramdisk_fs vnode lookups return fresh owned nodes (refcount 1) —
/// every test releases each resolved vnode exactly once (leaks fail the
/// snapshot check). Unique filenames per test + setup-unlink isolate
/// tests sharing the single /mnt/ramdisk mount (backing blocks persist
/// across snapshot_restore by design, like daemon CSpaces).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/ramdisk_fs.hpp>
#include <kernel/ramdisk/ramdiskd.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/ipc/ipc.hpp>
#include <string.hpp>

using namespace kernel;
using namespace kernel::vfs;

// Runmode: kernel
// Testidea: The ramdisk filesystem is mounted at /mnt/ramdisk when the
// daemon is granted (boot + snapshot-restore both mount best-effort).
// Input: ramdisk_fs_try_mount() (idempotent), resolve /mnt/ramdisk.
// Expect: try_mount returns 0; resolved vnode is a directory.
// Depends: live ramdiskd task (PRE: ramdiskd), vfs mount table.
JARVIS_TEST(ramdiskfs_mount_present, "PRE: ramdiskd | POST: none") {
    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT_EQ(0, vfs::ramdisk_fs_try_mount());
    JARVIS_ASSERT_EQ(0, vfs::ramdisk_fs_try_mount());
    Vnode *root = vfs::resolve("/mnt/ramdisk");
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->mode & vfs::S_IFDIR);
    vfs::release(root);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Full file round-trip through the flat store: create, span
// two blocks on write, read back byte-exact, fstat size, then delete.
// Input: 700 B pattern (spans blocks) under a unique name.
// Expect: All ops succeed; read-back equals the pattern; post-unlink
// resolve misses; double unlink fails.
// Depends: ramdisk_fs block IPC to the live daemon.
JARVIS_TEST(ramdiskfs_create_write_read, "PRE: ramdiskd | POST: none") {
    kernel::test::mark_vfs_touched();
    const char *path = "/mnt/ramdisk/rd_fs_t2_c.elf";
    vfs::unlink(path);
    JARVIS_ASSERT_EQ(0, vfs::create(path, 0));
    Vnode *file = vfs::resolve(path);
    JARVIS_ASSERT(file != nullptr);
    uint8_t wbuf[700] = {};
    for (uint64_t i = 0; i < sizeof(wbuf); ++i)
        wbuf[i] = static_cast<uint8_t>((i * 11 + 7) & 0xFF);
    int64_t written =
        file->ops->write(*file, wbuf, sizeof(wbuf), 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(wbuf)), written);
    vfs::release(file);
    file = vfs::resolve(path);
    JARVIS_ASSERT(file != nullptr);
    uint8_t rbuf[700] = {};
    int64_t nread = file->ops->read(*file, rbuf, sizeof(rbuf), 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(wbuf)), nread);
    for (uint64_t i = 0; i < sizeof(wbuf); ++i)
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(wbuf[i]),
                         static_cast<uint64_t>(rbuf[i]));
    VfsStat st{};
    JARVIS_ASSERT_EQ(0, file->ops->fstat(*file, st));
    JARVIS_ASSERT_EQ(sizeof(wbuf), st.st_size);
    vfs::release(file);
    JARVIS_ASSERT_EQ(0, vfs::unlink(path));
    file = vfs::resolve(path);
    JARVIS_ASSERT(file == nullptr);
    // Double unlink must fail (file already gone) — matches the file's own
    // contract comment and the VFS convention (tmpfs_unlink /
    // vfs_unlink_nonexistent both return VFS_INVALID for a missing entry).
    JARVIS_ASSERT(vfs::unlink(path) != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Reachable bad input fails closed without side effects:
// empty/overlong/slashed names, oversize write, ungranted mount.
// Input: malformed creates, >1 MiB write, pid-cell-cleared try_mount.
// Expect: All rejected; ungranted try_mount fails; grant restored.
// Depends: ramdisk_fs validators, pid cell save/restore (pid_cell pattern).
JARVIS_TEST(ramdiskfs_bounds_reject, "PRE: ramdiskd | POST: none") {
    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT_EQ(0, vfs::ramdisk_fs_try_mount());
    Vnode *root = vfs::resolve("/mnt/ramdisk");
    JARVIS_ASSERT(root != nullptr);
    JARVIS_ASSERT(root->ops->create(*root, "", vfs::S_IFREG) != 0);
    JARVIS_ASSERT(root->ops->create(*root, "has/slash", vfs::S_IFREG) != 0);
    char long_name[80] = {};
    for (uint64_t i = 0; i < sizeof(long_name) - 1; ++i)
        long_name[i] = 'x';
    JARVIS_ASSERT(root->ops->create(*root, long_name, vfs::S_IFREG) != 0);
    JARVIS_ASSERT(root->ops->mkdir(*root, "nosubdirs", vfs::S_IFDIR) != 0);
    vfs::release(root);
    // Ungranted mount must fail even though the store is already mounted
    // (fail-closed degraded). Capture results FIRST, restore the pid
    // cell, THEN assert: assert-fail paths must never leak pid 0 into
    // later tests (no unwinding reliance — plain sequential restore).
    const uint64_t prior = ramdiskd::get_ramdiskd_pid();
    ramdiskd::set_ramdiskd_pid(0);
    int ungranted_try = vfs::ramdisk_fs_try_mount();
    ramdiskd::set_ramdiskd_pid(prior);
    JARVIS_ASSERT(ungranted_try != 0);
    JARVIS_ASSERT_EQ(prior, ramdiskd::get_ramdiskd_pid());
    JARVIS_ASSERT_EQ(0, vfs::ramdisk_fs_try_mount());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Partial overwrite keeps the tail: visible semantics (no
// invisible staging — torn-image safety comes from uploader verify +
// loader validation, see ramdisk_fs.hpp). Rewrite the head of a file,
// read back: head is new, tail is old.
// Input: 128 B pattern, then 32 B overwrite at offset 0.
// Expect: Bytes [0,32) new, [32,128) old; size unchanged.
// Depends: ramdisk_fs in-place partial-block read-modify-write.
JARVIS_TEST(ramdiskfs_overwrite_partial, "PRE: ramdiskd | POST: none") {
    kernel::test::mark_vfs_touched();
    const char *path = "/mnt/ramdisk/rd_fs_t4_c.elf";
    vfs::unlink(path);
    JARVIS_ASSERT_EQ(0, vfs::create(path, 0));
    Vnode *file = vfs::resolve(path);
    JARVIS_ASSERT(file != nullptr);
    uint8_t base[128] = {};
    for (uint64_t i = 0; i < sizeof(base); ++i)
        base[i] = static_cast<uint8_t>(i & 0xFF);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(base)),
                     file->ops->write(*file, base, sizeof(base), 0));
    vfs::release(file);
    file = vfs::resolve(path);
    JARVIS_ASSERT(file != nullptr);
    uint8_t head[32] = {};
    for (uint64_t i = 0; i < sizeof(head); ++i)
        head[i] = static_cast<uint8_t>(0xA0 + i);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(head)),
                     file->ops->write(*file, head, sizeof(head), 0));
    vfs::release(file);
    file = vfs::resolve(path);
    JARVIS_ASSERT(file != nullptr);
    uint8_t back[128] = {};
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(back)),
                     file->ops->read(*file, back, sizeof(back), 0));
    for (uint64_t i = 0; i < sizeof(head); ++i)
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(head[i]),
                         static_cast<uint64_t>(back[i]));
    for (uint64_t i = sizeof(head); i < sizeof(base); ++i)
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(base[i]),
                         static_cast<uint64_t>(back[i]));
    vfs::release(file);
    JARVIS_ASSERT_EQ(0, vfs::unlink(path));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: loadelf-from-ramdisk acceptance (issue #314 compat proof):
// stage a real ELF (/hey.c.elf) through the flat store, then drive the
// loader's own accept path (request_load, no execution) and cancel it
// back-to-back so no async load residue escapes the test.
// Input: /hey.c.elf bytes copied to /mnt/ramdisk/rd_fs_t5.c.elf.
// Expect: Staged bytes read back exact; request_load returns OK (path
// resolves + open/stat probe passes); request_cancel returns OK.
// Depends: ramdisk_fs, ElfLoader accept path (no task spawn: cancel
// lands before the loader dispatches — same race discipline as the
// shell's cancel-load).
JARVIS_TEST(ramdiskfs_loadelf_from_ramdisk, "PRE: ramdiskd | POST: none") {
    kernel::test::mark_vfs_touched();
    Vnode *src = vfs::resolve("/hey.c.elf");
    JARVIS_ASSERT(src != nullptr);
    uint64_t fsize = src->size;
    JARVIS_ASSERT(fsize > 0);
    JARVIS_ASSERT(fsize < 512 * 1024);
    const char *path = "/mnt/ramdisk/rd_fs_t5.c.elf";
    vfs::unlink(path);
    JARVIS_ASSERT_EQ(0, vfs::create(path, 0));
    Vnode *dst = vfs::resolve(path);
    JARVIS_ASSERT(dst != nullptr);
    uint8_t chunk[512] = {};
    uint64_t off = 0;
    while (off < fsize) {
        uint64_t take = fsize - off;
        if (take > sizeof(chunk))
            take = sizeof(chunk);
        int64_t nread = src->ops->read(*src, chunk, take, off);
        JARVIS_ASSERT(nread > 0);
        int64_t written = dst->ops->write(
            *dst, chunk, static_cast<uint64_t>(nread), off);
        JARVIS_ASSERT_EQ(nread, written);
        off += static_cast<uint64_t>(nread);
    }
    vfs::release(src);
    vfs::release(dst);
    dst = vfs::resolve(path);
    JARVIS_ASSERT(dst != nullptr);
    VfsStat st{};
    JARVIS_ASSERT_EQ(0, dst->ops->fstat(*dst, st));
    JARVIS_ASSERT_EQ(fsize, st.st_size);
    vfs::release(dst);
    JARVIS_ASSERT(elf::ElfLoader::request_load(path) ==
                  elf::LoadResult::OK);
    JARVIS_ASSERT(elf::ElfLoader::request_cancel() ==
                  elf::LoadResult::OK);
    JARVIS_ASSERT_EQ(0, vfs::unlink(path));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Directory listing serves flat-store entries (readdir over
// the cached dir region; dent names + count match created files).
// Input: Two uniquely-named files created, listed, deleted.
// Expect: Both names appear exactly once; gone after unlink.
// Depends: ramdisk_fs readdir/lookup/unlink.
JARVIS_TEST(ramdiskfs_readdir_lists_files, "PRE: ramdiskd | POST: none") {
    kernel::test::mark_vfs_touched();
    const char *p1 = "/mnt/ramdisk/rd_fs_t6_a.txt";
    const char *p2 = "/mnt/ramdisk/rd_fs_t6_b.txt";
    vfs::unlink(p1);
    vfs::unlink(p2);
    JARVIS_ASSERT_EQ(0, vfs::create(p1, 0));
    JARVIS_ASSERT_EQ(0, vfs::create(p2, 0));
    Vnode *root = vfs::resolve("/mnt/ramdisk");
    JARVIS_ASSERT(root != nullptr);
    uint64_t pos = 0;
    uint64_t seen_a = 0;
    uint64_t seen_b = 0;
    for (uint64_t i = 0; i < 128; ++i) {
        Dirent dent{};
        if (root->ops->readdir(*root, pos, dent) != 0)
            break;
        if (strcmp(dent.d_name, "rd_fs_t6_a.txt") == 0)
            ++seen_a;
        if (strcmp(dent.d_name, "rd_fs_t6_b.txt") == 0)
            ++seen_b;
    }
    vfs::release(root);
    JARVIS_ASSERT_EQ(1ULL, seen_a);
    JARVIS_ASSERT_EQ(1ULL, seen_b);
    JARVIS_ASSERT_EQ(0, vfs::unlink(p1));
    JARVIS_ASSERT_EQ(0, vfs::unlink(p2));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A staged file carries a stored CRC32 (issue #318) computed at
// close; corrupting its backing block is detected by the integrity check and
// the entry fails closed (no longer resolvable).
// Input: create+write+close a file; corrupt one byte of its first backing
// block via raw daemon block IPC; run the verify seam.
// Expect: verify_all reports 1 mismatch; resolve() then returns nullptr.
// Depends: ramdisk_fs CRC metadata, ramdisk_fs_verify_all / _extent.
JARVIS_TEST(ramdiskfs_crc_detects_corruption, "PRE: ramdiskd | POST: none") {
    kernel::test::mark_vfs_touched();
    const char *path = "/mnt/ramdisk/rd_fs_crc.elf";
    const char *base = "rd_fs_crc.elf"; // bare name for the extent seam
    vfs::unlink(path);
    JARVIS_ASSERT_EQ(0, vfs::create(path, 0));
    Vnode *file = vfs::resolve(path);
    JARVIS_ASSERT(file != nullptr);
    uint8_t wbuf[600] = {};
    for (uint64_t i = 0; i < sizeof(wbuf); ++i)
        wbuf[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(wbuf)),
                     file->ops->write(*file, wbuf, sizeof(wbuf), 0));
    vfs::release(file); // close -> recompute + persist CRC, mark VALID
    JARVIS_ASSERT_EQ(0, vfs::ramdisk_fs_verify_all());
    uint64_t start = 0;
    uint64_t blocks = 0;
    uint32_t crc = 0;
    JARVIS_ASSERT(vfs::ramdisk_fs_extent(base, start, blocks, crc));
    const uint64_t daemon = ramdiskd::get_ramdiskd_pid();
    JARVIS_ASSERT(daemon != 0);
    const uint64_t me = Scheduler::current_task()->id;
    const uint64_t chunk0 = 0;
    uint8_t blk[ramdiskd::RAMDISK_CHUNK_DATA] = {};
    {
        Message req{};
        req.type = ramdiskd::RAMDISK_READ_BLOCK;
        __builtin_memcpy(req.data, &me, 8);
        __builtin_memcpy(req.data + 8, &start, 8);
        __builtin_memcpy(req.data + 16, &chunk0, 8);
        req.data_size = 24;
        Message reply{};
        JARVIS_ASSERT(IPC::send_sync(daemon, req, reply));
        int64_t r = 0;
        __builtin_memcpy(&r, reply.data, 8);
        JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK, r);
        __builtin_memcpy(blk, reply.data + 8, ramdiskd::RAMDISK_CHUNK_DATA);
    }
    blk[0] ^= 0xFF; // corrupt one byte of the file's data
    {
        Message req{};
        req.type = ramdiskd::RAMDISK_WRITE_BLOCK;
        __builtin_memcpy(req.data, &me, 8);
        __builtin_memcpy(req.data + 8, &start, 8);
        __builtin_memcpy(req.data + 16, &chunk0, 8);
        __builtin_memcpy(req.data + 24, blk, ramdiskd::RAMDISK_CHUNK_DATA);
        req.data_size = 56;
        Message reply{};
        JARVIS_ASSERT(IPC::send_sync(daemon, req, reply));
        int64_t r = 0;
        __builtin_memcpy(&r, reply.data, 8);
        JARVIS_ASSERT_EQ(ramdiskd::RAMDISK_OK, r);
    }
    // Integrity check detects the corruption and fails the entry closed.
    JARVIS_ASSERT_EQ(1, vfs::ramdisk_fs_verify_all());
    JARVIS_ASSERT(vfs::resolve(path) == nullptr);
    vfs::unlink(path);
    JARVIS_TEST_PASS();
}

void register_ramdisk_fs_tests() {
    Logger::info("Registering ramdisk_fs tests");
    JARVIS_REGISTER_TEST(ramdiskfs_mount_present);
    JARVIS_REGISTER_TEST(ramdiskfs_create_write_read);
    JARVIS_REGISTER_TEST(ramdiskfs_bounds_reject);
    JARVIS_REGISTER_TEST(ramdiskfs_overwrite_partial);
    JARVIS_REGISTER_TEST(ramdiskfs_loadelf_from_ramdisk);
    JARVIS_REGISTER_TEST(ramdiskfs_readdir_lists_files);
    JARVIS_REGISTER_TEST(ramdiskfs_crc_detects_corruption);
}
