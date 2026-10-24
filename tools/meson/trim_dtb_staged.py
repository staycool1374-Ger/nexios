#!/usr/bin/env python3
"""Trim a QEMU-dumped DTB and stage the assembler-visible copy (issue #170).

Runs tools/trim_dtb.py (input -> output), then copies the trimmed blob to
the assembler-visible path (<builddir>/build/virt.trimmed.dtb, which is
what virt_dtb.S `.incbin`s relative to the build root). One command, so
the staged copy can never go stale independently of the tracked output.

Usage:
    trim_dtb_staged.py <dumped> <output> <staged-copy>
"""

import shutil
import subprocess
import sys
from pathlib import Path


def main():
    """Trim the DTB and stage the assembler-visible copy."""
    if len(sys.argv) != 4:
        print("usage: trim_dtb_staged.py <dumped> <output> <staged-copy>")
        return 2
    _dumped, output, staged = sys.argv[1], sys.argv[2], sys.argv[3]
    repo = Path(__file__).resolve().parent.parent.parent
    subprocess.run(
        [sys.executable, str(repo / "tools" / "trim_dtb.py"),
         _dumped, output],
        check=True,
    )
    staged_path = Path(staged)
    staged_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(output, staged_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
