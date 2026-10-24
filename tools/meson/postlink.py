#!/usr/bin/env python3
"""Post-link: CRC patch, identity asserts, markers, legacy staging.

Mirrors the KERNEL_DEBUG link tail (mk/rules.mk:411-417 + Makefile
debug/release staging): copy, patch_code_crc, then:
  - readelf e_machine assert (fail-closed: never link/boot a wrong-arch ELF)
  - .lto-mode marker (A2: wipe-on-mismatch is enforced by build.sh/qemu_test.py)
  - .baked-test-config[-<arch>] equals the requested test class (A3 gate input)
  - legacy staging into the source-tree build/ (compat layer)
  - riscv64 raw BIN via objcopy (Make rule, same flags)

Usage:
    postlink.py --arch ARCH --buildtype BUILD --lto on|off
        --readelf READELF --test-class CLASS --input IN --output OUT
        [--riscv-bin OUT] [--legacy-dir DIR] [--objcopy OBJCOPY]
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

MACHINE = {"x86_64": "X86-64", "aarch64": "AArch64", "riscv64": "RISC-V"}

HERE = Path(__file__).resolve()
PATCH_CRC = HERE.parent.parent.parent / "tools" / "patch_code_crc.py"


def main():
    """Copy, patch, assert, mark, stage."""
    parser = argparse.ArgumentParser(description="Post-link staging.")
    parser.add_argument("--arch", required=True)
    parser.add_argument("--buildtype", required=True)
    parser.add_argument("--lto", required=True, choices=["on", "off"])
    parser.add_argument("--readelf", required=True)
    parser.add_argument("--test-class", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--baked", required=True)
    parser.add_argument("--baked-extra", default="")
    parser.add_argument("--riscv-bin", default=None)
    parser.add_argument("--legacy-dir", required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--map", default="")
    args = parser.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.input, out)
    subprocess.run([sys.executable, str(PATCH_CRC), str(out)], check=True)

    if args.arch not in MACHINE:
        print("unknown arch %s" % args.arch)
        return 2
    readelf = subprocess.run(
        [args.readelf, "-h", str(out)], capture_output=True, text=True,
        check=True,
    )
    if MACHINE[args.arch] not in readelf.stdout:
        print("arch mismatch: expected %s in %s" % (MACHINE[args.arch], out))
        print(readelf.stdout)
        return 1

    marker = Path(args.marker)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text("lto=%s buildtype=%s\n" % (args.lto, args.buildtype),
                      encoding="utf-8")
    baked = Path(args.baked)
    baked.parent.mkdir(parents=True, exist_ok=True)
    baked.write_text(args.test_class + "\n", encoding="utf-8")
    if args.baked_extra:
        extra = Path(args.baked_extra)
        extra.parent.mkdir(parents=True, exist_ok=True)
        extra.write_text(args.test_class + "\n", encoding="utf-8")

    legacy = Path(args.legacy_dir)
    legacy.mkdir(parents=True, exist_ok=True)
    shutil.copy2(out, legacy / "kernel-debug.elf")
    shutil.copy2(out, legacy / "kernel.elf")
    shutil.copy2(baked, legacy / baked.name)
    if args.map:
        shutil.copy2(args.map, legacy / "kernel.map")

    if args.arch == "riscv64" and args.riscv_bin:
        riscv_bin = Path(args.riscv_bin)
        subprocess.run(
            [args.objcopy, "-O", "binary", str(out), str(riscv_bin)],
            check=True,
        )
        # Make keeps debug/release BINs apart
        # (build/kernel-riscv64{,-release}.bin, Makefile:833/842);
        # collapsing them leaves release bits at the debug-ABI legacy
        # path after a release build.
        legacy_bin = ("kernel-riscv64-release.bin"
                      if args.buildtype == "release"
                      else "kernel-riscv64.bin")
        shutil.copy2(riscv_bin, legacy / legacy_bin)
    print("postlink OK: %s (%s, lto=%s, baked=%s)"
          % (out, args.arch, args.lto, args.test_class))
    return 0


if __name__ == "__main__":
    sys.exit(main())
