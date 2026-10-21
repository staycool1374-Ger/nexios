#!/usr/bin/env python3
"""Trim QEMU `dumpdtb` padding (issue #354, Q3 DTB handoff).

QEMU's `-machine virt,dumpdtb=` writes a 1 MiB file whose header `totalsize`
covers padding, not content. The real blob ends at
`off_dt_strings + size_dt_strings`. This script trims to that size and patches
the header `totalsize` so the embedded copy is header-consistent
(`fdt_check_header`/`fdt_check_layout` keep passing) and minimal (~8 KiB
instead of 1 MiB).
"""
import struct
import sys


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: trim_dtb.py <dumpdtb> <trimmed>")
    src, dst = sys.argv[1], sys.argv[2]
    blob = bytearray(open(src, "rb").read())
    if blob[:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit("not a device-tree blob: %s" % src)
    off_struct, off_strings = struct.unpack(">II", blob[8:16])
    # FDT header order (src/lib/fdt/fdt.h:30-31): size_dt_strings at +32,
    # size_dt_struct at +36. Unpack names must match, else the trim point
    # (off_strings + size_strings) lands on the wrong block end.
    size_strings, size_struct = struct.unpack(">II", blob[32:40])
    version = struct.unpack(">I", blob[20:24])[0]
    if version < 16:
        raise SystemExit("unsupported FDT version %d" % version)
    if off_struct != 0x40 or off_strings < off_struct + 8:
        raise SystemExit("implausible struct/strings offsets 0x%x/0x%x" %
                         (off_struct, off_strings))
    total = off_strings + size_strings
    if total > len(blob) or total < 64:
        raise SystemExit("implausible DTB size 0x%x" % total)
    del blob[total:]
    blob[4:8] = struct.pack(">I", total)
    open(dst, "wb").write(bytes(blob))
    print("trim_dtb: %s -> %s (%d bytes)" % (src, dst, total))


if __name__ == "__main__":
    main()
