#!/usr/bin/env python3
"""Post-link: compute CRC32 of .text section and patch _expected_code_crc in ELF."""

import struct
import sys
import zlib

def patch_crc(elf_path):
    with open(elf_path, 'r+b') as f:
        data = f.read()

        # Parse ELF header (64-bit)
        if data[:4] != b'\x7fELF':
            print("Not a valid ELF file")
            return 1

        # 64-bit ELF header offsets
        e_shoff = struct.unpack_from('<Q', data, 0x28)[0]  # section header offset
        e_shentsize = struct.unpack_from('<H', data, 0x3A)[0]  # section header entry size
        e_shnum = struct.unpack_from('<H', data, 0x3C)[0]  # number of sections
        e_shstrndx = struct.unpack_from('<H', data, 0x3E)[0]  # section name string table index

        # Read section name string table
        shstrtab_off = e_shoff + e_shstrndx * e_shentsize
        sh_name_offset = struct.unpack_from('<I', data, shstrtab_off + 0x18)[0]
        sh_size = struct.unpack_from('<Q', data, shstrtab_off + 0x20)[0]
        shstrtab = data[sh_name_offset:sh_name_offset + sh_size]

        text_start_addr = None
        text_size = None
        text_offset = None

        # Find .text section
        for i in range(e_shnum):
            sh_off = e_shoff + i * e_shentsize
            sh_name_idx = struct.unpack_from('<I', data, sh_off)[0]
            sh_name = shstrtab[sh_name_idx:shstrtab.index(b'\0', sh_name_idx)].decode('utf-8', errors='replace')
            if sh_name == '.text':
                text_offset = struct.unpack_from('<Q', data, sh_off + 0x18)[0]  # sh_offset
                text_size = struct.unpack_from('<Q', data, sh_off + 0x20)[0]  # sh_size
                text_start_addr = struct.unpack_from('<Q', data, sh_off + 0x10)[0]  # sh_addr
                print(f"  .text: addr=0x{text_start_addr:X}, offset=0x{text_offset:X}, size=0x{text_size:X}")
                break

        if text_offset is None:
            print("ERROR: .text section not found")
            print(f"  (e_shnum={e_shnum} e_shstrndx={e_shstrndx} "
                  f"e_shoff=0x{e_shoff:X}) sections:")
            for i in range(min(e_shnum, 60)):
                sh_off = e_shoff + i * e_shentsize
                sh_name_idx = struct.unpack_from('<I', data, sh_off)[0]
                try:
                    end = shstrtab.index(b'\0', sh_name_idx)
                    sh_name = shstrtab[sh_name_idx:end].decode(
                        'utf-8', errors='replace')
                except ValueError:
                    sh_name = f"<bad idx {sh_name_idx}>"
                sh_type = struct.unpack_from('<I', data, sh_off + 4)[0]
                sh_addr = struct.unpack_from('<Q', data, sh_off + 16)[0]
                sh_offset = struct.unpack_from('<Q', data, sh_off + 24)[0]
                sh_size = struct.unpack_from('<Q', data, sh_off + 32)[0]
                print(f"    [{i}] {sh_name} type={sh_type} "
                      f"addr=0x{sh_addr:X} off=0x{sh_offset:X} "
                      f"size=0x{sh_size:X}")
            return 1

        # CRC32 of .text section (skip first 8 bytes for the start marker)
        crc_start = 8
        if text_size <= crc_start:
            print("ERROR: .text section too small")
            return 1
        raw_text = data[text_offset + crc_start:text_offset + text_size]
        computed_crc = zlib.crc32(raw_text) & 0xFFFFFFFF
        print(f"  Computed CRC32: 0x{computed_crc:08X}")

        # Find _expected_code_crc symbol via .symtab or .dynsym
        for i in range(e_shnum):
            sh_off = e_shoff + i * e_shentsize
            sh_name_idx = struct.unpack_from('<I', data, sh_off)[0]
            sh_name = shstrtab[sh_name_idx:shstrtab.index(b'\0', sh_name_idx)].decode('utf-8', errors='replace')

            if sh_name in ('.symtab',):
                sym_offset = struct.unpack_from('<Q', data, sh_off + 0x18)[0]
                sym_size = struct.unpack_from('<Q', data, sh_off + 0x20)[0]
                sym_entsize = struct.unpack_from('<Q', data, sh_off + 0x38)[0]
                if sym_entsize == 0:
                    sym_entsize = 24  # ELF64 symbol size
                # Find strtab for this symtab
                sym_link = struct.unpack_from('<I', data, sh_off + 0x28)[0]
                strtab_off = e_shoff + sym_link * e_shentsize
                strtab_offset = struct.unpack_from('<Q', data, strtab_off + 0x18)[0]
                strtab_size = struct.unpack_from('<Q', data, strtab_off + 0x20)[0]
                strtab = data[strtab_offset:strtab_offset + strtab_size]

                for j in range(sym_size // sym_entsize):
                    sym_ent_off = sym_offset + j * sym_entsize
                    sym_name_idx = struct.unpack_from('<I', data, sym_ent_off)[0]
                    sym_name = strtab[sym_name_idx:strtab.index(b'\0', sym_name_idx)].decode('utf-8', errors='replace')
                    sym_value = struct.unpack_from('<Q', data, sym_ent_off + 0x08)[0]
                    sym_size_val = struct.unpack_from('<Q', data, sym_ent_off + 0x10)[0]

                    if sym_name == '_expected_code_crc':
                        print(f"  _expected_code_crc at VA 0x{sym_value:X}")

                        # Find which section this symbol is in and convert VA to file offset
                        # Find the containing section
                        for si in range(e_shnum):
                            sh_si_off = e_shoff + si * e_shentsize
                            sh_addr = struct.unpack_from('<Q', data, sh_si_off + 0x10)[0]
                            sh_size_si = struct.unpack_from('<Q', data, sh_si_off + 0x20)[0]
                            sh_offset_si = struct.unpack_from('<Q', data, sh_si_off + 0x18)[0]

                            if sh_addr <= sym_value < sh_addr + sh_size_si:
                                file_offset = sh_offset_si + (sym_value - sh_addr)
                                # Patch the 4-byte CRC value (UINT32)
                                f.seek(file_offset)
                                f.write(struct.pack('<I', computed_crc))
                                print(f"  Patched CRC at file offset 0x{file_offset:X}")
                                return 0

                        print("  ERROR: Could not find containing section for _expected_code_crc")
                        return 1

                break

        print("ERROR: _expected_code_crc symbol not found")
        return 1

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <kernel.elf>")
        sys.exit(1)
    sys.exit(patch_crc(sys.argv[1]))