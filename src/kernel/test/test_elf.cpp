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

/// @file test_elf.cpp
/// @brief ELF loader tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/memory/pmm.hpp>
#include <constants.hpp>
#include <string.hpp>
#include <kernel/vfs/vfs.hpp>

using namespace kernel;

// Native ELF machine for synthetic headers (issue #352): the validator
// gates e_machine per arch (x86_64 0x3E, AArch64 0xB7, RISC-V 0xF3), so
// good-header fixtures must use the build arch's value; 0x28 (EM_ARM)
// stays the universal bad-machine probe.
#if defined(CONFIG_ARCH_AARCH64)
constexpr uint16_t kNativeEm = 0xB7;
#elif defined(CONFIG_ARCH_RISCV64)
constexpr uint16_t kNativeEm = 0xF3;
#else
constexpr uint16_t kNativeEm = 0x3E;
#endif

// Runmode: kernel
// Testidea: Validates that elf::validate_header returns false when given a
// null pointer.
// Input: nullptr
// Expect: JARVIS_ASSERT checks that !elf::validate_header(nullptr)
// Depends: test, elf
JARVIS_TEST(elf_validate_header_null, "PRE: none | POST: none") {
    JARVIS_ASSERT(!elf::validate_header(nullptr));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that a properly constructed ELF64Header passes
// validation, and corrupting the magic number causes rejection.
// Input: A manually populated valid ELF64Header, then with ident[0] set to 0
// Expect: JARVIS_ASSERT first on true, then on false for corrupted header
// Depends: test, elf
JARVIS_TEST(elf_validate_header_magic, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    hdr.ident[0] = 0x7F;
    hdr.ident[1] = 'E';
    hdr.ident[2] = 'L';
    hdr.ident[3] = 'F';
    hdr.ident[4] = 2;
    hdr.ident[5] = 1;
    hdr.ident[6] = 1;
    hdr.type = 2;
    hdr.machine = kNativeEm;
    hdr.version = 1;
    hdr.ehsize = sizeof(elf::ELF64Header);
    hdr.phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr.phnum = 0;
    hdr.entry = 0x400000;

    JARVIS_ASSERT(elf::validate_header(&hdr));

    hdr.ident[0] = 0;
    JARVIS_ASSERT(!elf::validate_header(&hdr));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that an ELF header with an unsupported machine type
// (0x28/EM_ARM on every 64-bit arch) is rejected.
// Input: A valid ELF64Header with machine set to 0x28
// Expect: JARVIS_ASSERT checks that !elf::validate_header(&hdr)
// Depends: test, elf
JARVIS_TEST(elf_validate_header_bad_machine, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    hdr.ident[0] = 0x7F;
    hdr.ident[1] = 'E';
    hdr.ident[2] = 'L';
    hdr.ident[3] = 'F';
    hdr.ident[4] = 2;
    hdr.ident[5] = 1;
    hdr.ident[6] = 1;
    hdr.type = 2;
    hdr.machine = kNativeEm;
    hdr.version = 1;
    hdr.ehsize = sizeof(elf::ELF64Header);
    hdr.phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr.phnum = 0;
    hdr.entry = 0x400000;

    hdr.machine = 0x28;
    JARVIS_ASSERT(!elf::validate_header(&hdr));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that an ELF header with an excessive number of program
// headers (100) is rejected.
// Input: A valid ELF64Header with phnum set to 100
// Expect: JARVIS_ASSERT checks that !elf::validate_header(&hdr)
// Depends: test, elf
JARVIS_TEST(elf_validate_header_excessive_phnum, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    hdr.ident[0] = 0x7F;
    hdr.ident[1] = 'E';
    hdr.ident[2] = 'L';
    hdr.ident[3] = 'F';
    for (int i = 7; i < 16; ++i)
        hdr.ident[i] = 0;
    hdr.ident[4] = 2;
    hdr.ident[5] = 1;
    hdr.ident[6] = 1;
    hdr.type = 2;
    hdr.machine = kNativeEm;
    hdr.version = 1;
    hdr.ehsize = sizeof(elf::ELF64Header);
    hdr.phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr.phnum = 100;
    hdr.entry = 0x400000;

    JARVIS_ASSERT(!elf::validate_header(&hdr));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that an ELF header with an entry point in
// kernel-reserved memory range is rejected.
// Input: A valid ELF64Header with entry set to 0xFFFF800000000000
// Expect: JARVIS_ASSERT checks that !elf::validate_header(&hdr)
// Depends: test, elf
JARVIS_TEST(elf_validate_header_bad_entry, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    hdr.ident[0] = 0x7F;
    hdr.ident[1] = 'E';
    hdr.ident[2] = 'L';
    hdr.ident[3] = 'F';
    for (int i = 7; i < 16; ++i)
        hdr.ident[i] = 0;
    hdr.ident[4] = 2;
    hdr.ident[5] = 1;
    hdr.ident[6] = 1;
    hdr.type = 2;
    hdr.machine = kNativeEm;
    hdr.version = 1;
    hdr.ehsize = sizeof(elf::ELF64Header);
    hdr.phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr.phnum = 0;
    hdr.entry = 0xFFFF800000000000ULL;

    JARVIS_ASSERT(!elf::validate_header(&hdr));
    JARVIS_TEST_PASS();
}

static void build_minimal_elf(elf::ELF64Header *hdr,
                              elf::ELF64ProgramHeader *phdr, uint8_t *data) {
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2;
    hdr->ident[5] = 1;
    hdr->ident[6] = 1;
    for (int i = 7; i < 16; ++i)
        hdr->ident[i] = 0;
    hdr->type = elf::ET_EXEC;
    hdr->machine = kNativeEm;
    hdr->version = 1;
    hdr->entry = 0x400000;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->shoff = 0;
    hdr->flags = 0;
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 1;
    hdr->shentsize = 0;
    hdr->shnum = 0;
    hdr->shstrndx = 0;

    phdr->type = elf::PT_LOAD;
    phdr->flags = elf::PF_R | elf::PF_X;
    phdr->offset = sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader);
    phdr->vaddr = 0x400000;
    phdr->paddr = 0x400000;
    phdr->filesz = 0x1000;
    phdr->memsz = 0x1000;
    phdr->align = 0x1000;

    memcpy(data + hdr->phoff, phdr, sizeof(elf::ELF64ProgramHeader));
    uint64_t code_offset =
        sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader);
    for (size_t i = 0; i < 0x1000; ++i) {
        data[code_offset + i] = 0x90;
    }
}

// Runmode: kernel
// Testidea: Validates that loading an ELF with an invalid program segment is
// rejected.
// Input: ELF with W^X violating segment (both writable and executable)
// Expect: elf::load returns nullptr
// Depends: test, elf
JARVIS_TEST(elf_load_invalid_segment, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    elf::ELF64ProgramHeader phdr{};
    uint8_t data[8192];
    build_minimal_elf(&hdr, &phdr, data);

    auto *embedded_phdr =
        reinterpret_cast<elf::ELF64ProgramHeader *>(data + hdr.phoff);
    embedded_phdr->flags = elf::PF_W | elf::PF_X;

    auto *tcb = elf::load(&hdr, data);
    JARVIS_ASSERT(tcb == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that elf::load sets up stdin/stdout/stderr file
// descriptors pointing to /dev/tty for the new task.
// Input: A minimal ELF64 binary constructed via build_minimal_elf
// Expect: JARVIS_ASSERT checks tcb non-null, /dev/tty resolves, and fd_table
// entries 0-2 exist, are used, and point to the tty vnode
// Depends: test, elf, vfs
JARVIS_TEST(elf_load_sets_std_fds, "PRE: vfsd, iocd | POST: none") {
    elf::ELF64Header hdr{};
    elf::ELF64ProgramHeader phdr{};
    uint8_t data[8192];
    build_minimal_elf(&hdr, &phdr, data);

    auto *tcb = elf::load(&hdr, data);
    JARVIS_ASSERT(tcb != nullptr);

    auto *tty = vfs::resolve("/dev/tty");
    JARVIS_ASSERT(tty != nullptr);

    for (int fd = 0; fd < 3; ++fd) {
        auto *f = tcb->fd_table.get(fd);
        JARVIS_ASSERT(f != nullptr);
        JARVIS_ASSERT(f->used);
        JARVIS_ASSERT(f->vnode == tty);
    }

    tcb->cleanup();
    delete tcb;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates that elf::load allocates a user stack for the loaded
// task.
// Input: A minimal ELF64 binary constructed via build_minimal_elf
// Expect: JARVIS_ASSERT checks tcb->user_stack_ non-zero and
// tcb->user_stack_size_ equals mem::STACK_SIZE
// Depends: test, elf
JARVIS_TEST(elf_load_creates_user_stack, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    elf::ELF64ProgramHeader phdr{};
    uint8_t data[8192];
    build_minimal_elf(&hdr, &phdr, data);

    auto *tcb = elf::load(&hdr, data);
    JARVIS_ASSERT(tcb != nullptr);
    JARVIS_ASSERT(tcb->user_stack_ != 0);
    JARVIS_ASSERT_EQ(mem::STACK_SIZE, tcb->user_stack_size_);

    tcb->cleanup();
    delete tcb;
    JARVIS_TEST_PASS();
}

JARVIS_TEST(elf_load_from_hhdm_buffer, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    elf::ELF64ProgramHeader phdr{};
    uint8_t data[8192];
    build_minimal_elf(&hdr, &phdr, data);

    auto *tcb = elf::load(&hdr, data);
    JARVIS_ASSERT(tcb != nullptr);
    JARVIS_ASSERT(tcb->user_stack_ != 0);
    JARVIS_ASSERT(tcb->get_page_table() != 0);

    tcb->cleanup();
    delete tcb;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all ELF-related unit tests with the test framework.
// Input: (none)
// Expect: Each JARVIS_REGISTER_TEST call registers a test function for later
// execution
// Depends: test, logger, elf
void register_elf_tests() {
    Logger::info("Registering ELF tests");

    JARVIS_REGISTER_TEST(elf_validate_header_null);
    JARVIS_REGISTER_TEST(elf_validate_header_magic);
    JARVIS_REGISTER_TEST(elf_validate_header_bad_machine);
    JARVIS_REGISTER_TEST(elf_validate_header_excessive_phnum);
    JARVIS_REGISTER_TEST(elf_validate_header_bad_entry);
    JARVIS_REGISTER_TEST(elf_load_invalid_segment);
    JARVIS_REGISTER_TEST(elf_load_sets_std_fds);
    JARVIS_REGISTER_TEST(elf_load_creates_user_stack);
    JARVIS_REGISTER_TEST(elf_load_from_hhdm_buffer);
}
