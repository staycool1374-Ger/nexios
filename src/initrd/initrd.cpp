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

#include <initrd/initrd.hpp>
#include <string.hpp>

namespace initrd {

static const uint8_t* initrd_start = nullptr;
static const uint8_t* initrd_end = nullptr;

struct CpioNewcHeader {
    char    c_magic[6];
    char    c_ino[8];
    char    c_mode[8];
    char    c_uid[8];
    char    c_gid[8];
    char    c_nlink[8];
    char    c_mtime[8];
    char    c_filesize[8];
    char    c_devmajor[8];
    char    c_devminor[8];
    char    c_rdevmajor[8];
    char    c_rdevminor[8];
    char    c_namesize[8];
    char    c_check[8];
} __attribute__((packed));

static uint32_t parse_hex8(const char* s) {
    uint32_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9')      val |= c - '0';
        else if (c >= 'a' && c <= 'f') val |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val |= c - 'A' + 10;
    }
    return val;
}

static uint32_t align4(uint32_t val) {
    return (val + 3) & ~3U;
}

void init(const uint8_t* start, const uint8_t* end) {
    initrd_start = start;
    initrd_end = end;
}

static bool is_trailer(const CpioNewcHeader*, const uint8_t* name_ptr) {
    return name_ptr[0] == 'T' && name_ptr[1] == 'R' && name_ptr[2] == 'A' &&
           name_ptr[3] == 'I' && name_ptr[4] == 'L' && name_ptr[5] == 'E' &&
           name_ptr[6] == 'R' && name_ptr[7] == '!' && name_ptr[8] == '!' &&
           name_ptr[9] == '!' && name_ptr[10] == '\0';
}

InitrdFile find(const char* path) {
    InitrdFile result = {nullptr, 0};
    if (!initrd_start) return result;

    const uint8_t* ptr = initrd_start;
    while (ptr < initrd_end) {
        auto* hdr = reinterpret_cast<const CpioNewcHeader*>(ptr);
        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            break;
        }

        uint32_t namesize = parse_hex8(hdr->c_namesize);
        uint32_t filesize = parse_hex8(hdr->c_filesize);
        uint32_t hdr_size = 110;
        uint32_t name_offset = hdr_size;

        if (namesize == 11 && is_trailer(hdr, ptr + name_offset)) break;

        uint32_t data_offset = align4(hdr_size + namesize);
        const uint8_t* fname = ptr + name_offset;

        // The cpio archive is produced by `find . -print0`, so every entry
        // name is prefixed with "./" (e.g. "./prime.c.elf").  Normalize on
        // BOTH sides so a lookup of "prime.c.elf" or "./prime.c.elf" matches
        // "./prime.c.elf" (and the boot-time "./etc/fstab" lookups keep
        // working).
        const char* p = path;
        const uint8_t* n = fname;
        if (p[0] == '.' && p[1] == '/') p += 2;
        if (n[0] == '.' && n[1] == '/') n += 2;
        bool match = true;
        while (*p && *n && *p == static_cast<char>(*n)) { ++p; ++n; }
        if (*p == '\0' && *n == '\0') match = true;
        else match = false;

        if (match) {
            result.data = ptr + data_offset;
            result.size = filesize;
            return result;
        }

        uint32_t entry_size = align4(data_offset + filesize);
        ptr += entry_size;
    }

    return result;
}

bool readdir(uint64_t* pos, InitrdEntry* entry) {
    if (!initrd_start || !pos || !entry) return false;

    const uint8_t* ptr = initrd_start;
    uint64_t current = 0;

    while (ptr < initrd_end) {
        auto* hdr = reinterpret_cast<const CpioNewcHeader*>(ptr);
        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            break;
        }

        uint32_t namesize = parse_hex8(hdr->c_namesize);
        uint32_t filesize = parse_hex8(hdr->c_filesize);
        uint32_t hdr_size = 110;
        uint32_t name_offset = hdr_size;
        uint32_t data_offset = align4(hdr_size + namesize);

        if (namesize == 11 && is_trailer(hdr, ptr + name_offset)) break;

        if (current == *pos) {
            size_t i = 0;
            const uint8_t* fname = ptr + name_offset;
            while (fname[i] && i < 255) { entry->name[i] = static_cast<char>(fname[i]); ++i; }
            entry->name[i] = '\0';
            entry->size = filesize;
            uint32_t mode = parse_hex8(hdr->c_mode);
            entry->is_dir = (mode & 040000) != 0;
            ++(*pos);
            return true;
        }

        uint32_t entry_size = align4(data_offset + filesize);
        ptr += entry_size;
        ++current;
    }

    return false;
}

} // namespace initrd
