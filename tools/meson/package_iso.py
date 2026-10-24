#!/usr/bin/env python3
"""Build a bootable GRUB ISO from the Meson kernel ELF (issue #170).

Mirrors the Make ISO recipe: iso-root with /boot/kernel.elf +
/boot/grub/grub.cfg, packed with grub-mkrescue. The grub.cfg is the
tracked source-tree file (multiboot2 entry); the kernel is the
post-link (CRC-patched) builddir ELF.

Usage:
    package_iso.py --kernel ELF --grub-cfg CFG --out ISO [--legacy PATH]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    """Stage the ISO root and run grub-mkrescue."""
    parser = argparse.ArgumentParser(description="Build the boot ISO.")
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--grub-cfg", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--legacy", default="")
    args = parser.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="iso-root-"))
    try:
        boot = work / "boot"
        (boot / "grub").mkdir(parents=True)
        shutil.copy2(args.kernel, boot / "kernel.elf")
        shutil.copy2(args.grub_cfg, boot / "grub" / "grub.cfg")
        subprocess.run(["grub-mkrescue", "-o", str(out), str(work)],
                       check=True, capture_output=True, text=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if args.legacy:
        legacy = Path(args.legacy)
        legacy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(out, legacy)
    print("iso: %s (%d bytes)" % (out, out.stat().st_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
