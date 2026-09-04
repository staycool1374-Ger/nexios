/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_initrd_parser.cpp
/// @brief initrd cpio-newc parser tests (milestone v0.4.3 issue #112):
///        synthetic in-memory archives driving find()/readdir() through
///        corrupt magic, truncation, TRAILER handling, path normalization
///        and enumeration ordering.
/// @note  Every test installs its synthetic archive via initrd::init(),
///        captures results into locals, RESTORES the boot initrd
///        (linker-provided _binary_initrd_cpio_start/end) and only then
///        asserts — assertion early-return can never leave a poisoned
///        initrd for later tests.

#include <test.hpp>
#include <logger.hpp>
#include <initrd/initrd.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Boot-time initrd boundaries (linker symbols from the embedded
/// cpio object; kernel.cpp uses the same pair at boot).
extern "C" uint8_t _binary_initrd_cpio_start[];
extern "C" uint8_t _binary_initrd_cpio_end[];

constexpr size_t SYNTH_CAP = 4096;
uint8_t synth_archive[SYNTH_CAP];

/// @brief Appends cpio-newc entries into synth_archive.
struct CpioBuilder {
    size_t pos = 0;
    bool overflowed = false;

    void reset() { pos = 0; }

    static void hex8(char *dst, uint32_t value) {
        static const char digits[] = "0123456789ABCDEF";
        for (int i = 7; i >= 0; --i) {
            dst[i] = digits[value & 0xF];
            value >>= 4;
        }
    }

    static uint32_t align4(uint32_t v) { return (v + 3) & ~3U; }

    /// Appends one newc entry; name is used verbatim (callers pass the
    /// "./"-prefixed form to mirror the boot archive).  Returns the byte
    /// offset of the entry header.
    size_t add(const char *name, const void *data, size_t size,
               uint32_t mode) {
        size_t name_len = strlen(name) + 1; // includes NUL, per newc spec
        size_t name_off = pos + 110;
        size_t data_off = align4(static_cast<uint32_t>(name_off + name_len));
        size_t end = align4(static_cast<uint32_t>(data_off + size));
        if (end > SYNTH_CAP) {
            overflowed = true;
            return 0;
        }

        char hdr[110];
        memcpy(hdr, "070701", 6);
        hex8(hdr + 6, 0);        // ino
        hex8(hdr + 14, mode);    // mode
        hex8(hdr + 22, 0);       // uid
        hex8(hdr + 30, 0);       // gid
        hex8(hdr + 38, 1);       // nlink
        hex8(hdr + 46, 0);       // mtime
        hex8(hdr + 54, static_cast<uint32_t>(size)); // filesize
        hex8(hdr + 62, 0);       // devmajor
        hex8(hdr + 70, 0);       // devminor
        hex8(hdr + 78, 0);       // rdevmajor
        hex8(hdr + 86, 0);       // rdevminor
        hex8(hdr + 94, static_cast<uint32_t>(name_len)); // namesize
        hex8(hdr + 102, 0);      // check
        memcpy(&synth_archive[pos], hdr, 110);
        memcpy(&synth_archive[name_off], name, name_len);
        for (size_t i = name_off + name_len; i < data_off; ++i)
            synth_archive[i] = 0;
        if (size && data)
            memcpy(&synth_archive[data_off], data, size);
        for (size_t i = data_off + size; i < end; ++i)
            synth_archive[i] = 0;
        pos = end;
        return name_off - 110;
    }

    void add_trailer() { add("TRAILER!!!", nullptr, 0, 0); }

    /// Overwrites the cpio magic of the entry at byte offset to simulate
    /// archive corruption.
    void corrupt_magic_at(size_t entry_off) {
        synth_archive[entry_off] = '7';
        synth_archive[entry_off + 1] = 'X';
    }
};

/// @brief Installs the synthetic archive as the initrd.
void install_synth(const CpioBuilder &b) {
    JARVIS_ASSERT_FMT(!b.overflowed, "synthetic archive overflow");
    initrd::init(synth_archive, synth_archive + b.pos);
}

/// @brief Restores the boot initrd — MUST run before any assertion.
void restore_boot_initrd() {
    initrd::init(_binary_initrd_cpio_start, _binary_initrd_cpio_end);
}

} // namespace

// Runmode: kernel
// Testidea: find() resolves an existing entry with and without the "./"
// prefix on BOTH sides (the boot archive stores "./name"), returning a
// data pointer inside the archive and the exact file size.
// Input: Archive with "./prime.c.elf" (64 bytes of pattern data).
// Expect: find("prime.c.elf") and find("./prime.c.elf") both return
//         size 64 and data pointing into the synthetic buffer.
// Depends: initrd::find
JARVIS_TEST(initrd_find_dotted_and_bare, "PRE: iocd | POST: none") {
    CpioBuilder b;
    uint8_t payload[64];
    memset(payload, 0x5A, sizeof(payload));
    b.add("./prime.c.elf", payload, sizeof(payload), 0100644);
    install_synth(b);

    initrd::InitrdFile bare = initrd::find("prime.c.elf");
    initrd::InitrdFile dotted = initrd::find("./prime.c.elf");
    restore_boot_initrd();

    JARVIS_ASSERT(bare.data != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(64), bare.size);
    JARVIS_ASSERT(bare.data >= synth_archive &&
                  bare.data < synth_archive + SYNTH_CAP);
    JARVIS_ASSERT_EQ(0x5A, bare.data[0]);
    JARVIS_ASSERT_EQ(0x5A, bare.data[63]);
    JARVIS_ASSERT(dotted.data != nullptr);
    JARVIS_ASSERT_EQ(bare.data, dotted.data);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: find() on a non-existent name returns the not-found sentinel
// {nullptr, 0} — no stale pointer, no garbage size.
// Input: Archive with one unrelated file; find("missing.bin").
// Expect: data == nullptr and size == 0.
// Depends: initrd::find
JARVIS_TEST(initrd_find_missing, "PRE: iocd | POST: none") {
    CpioBuilder b;
    b.add("./etc/fstab", "tmpfs /tmp\n", 11, 0100644);
    install_synth(b);

    initrd::InitrdFile f = initrd::find("missing.bin");
    restore_boot_initrd();

    JARVIS_ASSERT(f.data == nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), f.size);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A corrupt magic mid-archive stops the scan immediately —
// entries AFTER the corruption are unreachable (fail-closed) and the
// scan never reads past the containing buffer.
// Input: Valid "aaa", corrupted second header, "target" placed after it.
// Expect: find("target") not found; find("aaa") still resolves.
// Depends: initrd::find
JARVIS_TEST(initrd_corrupt_magic_stops_scan, "PRE: iocd | POST: none") {
    CpioBuilder b;
    size_t off_a = b.add("./aaa", "A", 1, 0100644);
    size_t off_b = b.add("./bbb", "B", 1, 0100644);
    b.add("./target", "T", 1, 0100644);
    b.corrupt_magic_at(off_b);
    install_synth(b);

    initrd::InitrdFile before = initrd::find("aaa");
    initrd::InitrdFile target = initrd::find("target");
    restore_boot_initrd();

    JARVIS_ASSERT(before.data != nullptr);
    JARVIS_ASSERT(target.data == nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), target.size);
    JARVIS_ASSERT(off_a < off_b);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Truncated archives are safe — an archive that ends between
// entries (exact boundary) yields not-found for anything beyond it, and
// an end-pointer landing mid-entry with a corrupt header magic breaks
// the scan without faulting (bounds are checked per iteration; all reads
// stay inside the mapped containing buffer).
// Input: (a) end exactly after entry 1; (b) end 50 bytes into a CORRUPTED
//        second header.
// Expect: find returns not-found in both cases; no fault.
// Depends: initrd::find
JARVIS_TEST(initrd_truncated_archive_safe, "PRE: iocd | POST: none") {
    // (a) exact boundary truncation
    CpioBuilder b1;
    b1.add("./first", "F", 1, 0100644);
    size_t cut = b1.pos;
    b1.add("./second", "S", 1, 0100644);
    initrd::init(synth_archive, synth_archive + cut);
    initrd::InitrdFile a = initrd::find("second");
    restore_boot_initrd();

    // (b) mid-entry truncation with corrupted magic: the scan reads the
    // garbage magic and breaks immediately.
    CpioBuilder b2;
    b2.add("./first", "F", 1, 0100644);
    size_t second_hdr = b2.pos;
    b2.add("./second", "S", 1, 0100644);
    b2.corrupt_magic_at(second_hdr);
    initrd::init(synth_archive, synth_archive + second_hdr + 50);
    initrd::InitrdFile bb = initrd::find("second");
    restore_boot_initrd();

    JARVIS_ASSERT(a.data == nullptr);
    JARVIS_ASSERT(bb.data == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The "TRAILER!!!" record terminates both enumeration and
// lookup — entries placed after the trailer are invisible.
// Input: "one", "two", TRAILER, "hidden".
// Expect: readdir lists exactly one/two; find("hidden") not found;
//         find("two") resolves.
// Depends: initrd::readdir, initrd::find
JARVIS_TEST(initrd_trailer_ends_enumeration, "PRE: iocd | POST: none") {
    CpioBuilder b;
    b.add("./one", "1", 1, 0100644);
    b.add("./two", "2", 1, 0100644);
    b.add_trailer();
    b.add("./hidden", "H", 1, 0100644);
    install_synth(b);

    initrd::InitrdEntry entries[8] = {};
    uint64_t pos = 0;
    int count = 0;
    while (count < 8 && initrd::readdir(&pos, &entries[count]))
        ++count;
    initrd::InitrdFile hidden = initrd::find("hidden");
    initrd::InitrdFile two = initrd::find("two");
    restore_boot_initrd();

    JARVIS_ASSERT_EQ(2, count);
    JARVIS_ASSERT(memcmp(entries[0].name, "./one", 6) == 0);
    JARVIS_ASSERT(memcmp(entries[1].name, "./two", 6) == 0);
    JARVIS_ASSERT(hidden.data == nullptr);
    JARVIS_ASSERT(two.data != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: readdir enumerates multi-file archives in archive order with
// the correct name, size and is_dir flag, and returns false past the end.
// Input: dir "etc" (mode 040755), file "etc/motd" (12 bytes), file
//        "bin/sh" (30 bytes).
// Expect: Three entries in order; sizes and dir flags match; 4th call
//         returns false.
// Depends: initrd::readdir
JARVIS_TEST(initrd_readdir_order_and_metadata, "PRE: iocd | POST: none") {
    CpioBuilder b;
    b.add("./etc/", nullptr, 0, 040755);
    b.add("./etc/motd", "hello world\n", 12, 0100644);
    b.add("./bin/sh", "000000000000000000000000000000", 30, 0100755);
    b.add_trailer();
    install_synth(b);

    initrd::InitrdEntry e[4] = {};
    uint64_t pos = 0;
    bool got0 = initrd::readdir(&pos, &e[0]);
    bool got1 = initrd::readdir(&pos, &e[1]);
    bool got2 = initrd::readdir(&pos, &e[2]);
    bool got3 = initrd::readdir(&pos, &e[3]);
    restore_boot_initrd();

    JARVIS_ASSERT(got0 && got1 && got2);
    JARVIS_ASSERT(!got3);
    JARVIS_ASSERT(memcmp(e[0].name, "./etc/", 7) == 0);
    JARVIS_ASSERT(e[0].is_dir);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), e[0].size);
    JARVIS_ASSERT(memcmp(e[1].name, "./etc/motd", 11) == 0);
    JARVIS_ASSERT(!e[1].is_dir);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(12), e[1].size);
    JARVIS_ASSERT(memcmp(e[2].name, "./bin/sh", 9) == 0);
    JARVIS_ASSERT(!e[2].is_dir);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(30), e[2].size);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: "./"-prefix normalization covers nested paths — a bare lookup
// of a nested name matches its "./"-prefixed archive entry.
// Input: Archive entry "./deep/file.bin".
// Expect: find("deep/file.bin") and find("./deep/file.bin") both resolve
//         to the same data.
// Depends: initrd::find
JARVIS_TEST(initrd_nested_path_normalization, "PRE: iocd | POST: none") {
    CpioBuilder b;
    b.add("./deep/file.bin", "DATA", 4, 0100644);
    install_synth(b);

    initrd::InitrdFile bare = initrd::find("deep/file.bin");
    initrd::InitrdFile dotted = initrd::find("./deep/file.bin");
    restore_boot_initrd();

    JARVIS_ASSERT(bare.data != nullptr);
    JARVIS_ASSERT(bare.data == dotted.data);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(4), bare.size);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Zero-length files resolve with a valid (non-null) data
// pointer at the aligned data offset and size 0.
// Input: Archive with "./empty" of size 0.
// Expect: find("empty").size == 0 and data != nullptr.
// Depends: initrd::find
JARVIS_TEST(initrd_zero_length_file, "PRE: iocd | POST: none") {
    CpioBuilder b;
    b.add("./empty", nullptr, 0, 0100644);
    install_synth(b);

    initrd::InitrdFile f = initrd::find("empty");
    restore_boot_initrd();

    JARVIS_ASSERT(f.data != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), f.size);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The boot initrd restore contract holds — after the synthetic
// games, find() resolves a file from the REAL embedded archive again
// (guards every other test that depends on initrd::find).
// Input: install synthetic, restore, find("./etc/fstab") on the boot
//        archive (built from initrd_root/etc/fstab).
// Expect: data != nullptr, size > 0 after restore.
// Depends: initrd::find, initrd::init
JARVIS_TEST(initrd_boot_archive_restored, "PRE: iocd | POST: none") {
    CpioBuilder b;
    b.add("./junk", "J", 1, 0100644);
    install_synth(b);
    initrd::InitrdFile junk = initrd::find("junk");
    restore_boot_initrd();
    initrd::InitrdFile fstab = initrd::find("./etc/fstab");

    JARVIS_ASSERT(junk.data != nullptr);       // synthetic was live
    JARVIS_ASSERT(fstab.data != nullptr);      // real archive is back
    JARVIS_ASSERT(fstab.size > 0);
    JARVIS_ASSERT(memcmp(fstab.data, "tmpfs", 5) == 0);
    JARVIS_TEST_PASS();
}

void register_initrd_parser_tests() {
    Logger::info("Registering initrd parser tests");
    JARVIS_REGISTER_TEST(initrd_find_dotted_and_bare);
    JARVIS_REGISTER_TEST(initrd_find_missing);
    JARVIS_REGISTER_TEST(initrd_corrupt_magic_stops_scan);
    JARVIS_REGISTER_TEST(initrd_truncated_archive_safe);
    JARVIS_REGISTER_TEST(initrd_trailer_ends_enumeration);
    JARVIS_REGISTER_TEST(initrd_readdir_order_and_metadata);
    JARVIS_REGISTER_TEST(initrd_nested_path_normalization);
    JARVIS_REGISTER_TEST(initrd_zero_length_file);
    JARVIS_REGISTER_TEST(initrd_boot_archive_restored);
}
