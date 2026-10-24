#!/usr/bin/env python3
"""Stage the initrd tree and pack initrd.cpio (issue #170).

Mirrors the Makefile $(INITRD_CPIO) recipe byte-for-content: same files,
same fixed file contents. Archive order is LC_ALL=C sorted (deterministic;
the Make order is readdir-dependent). Layout contract (issue #274):
demo programs at the root, everything else under /bin.

Usage:
    stage_initrd.py --out initrd.cpio --test-config FILE
        --root a.c.elf [b.c.elf ...] --bin c.c.elf [...]
        --user-dir userspace --tools-dir tools
        [--rust-hello path] [--libc-verify path] [--rust-sources dir]
"""

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

SH310 = "loadelf /bin/sh.c.elf\nsleep 3\nrunelf\nsleep 300\n"


def copy_if_present(src, dst):
    """Copy src to dst when src exists (graceful absent-toolchain rule)."""
    if src is not None and Path(src).exists():
        shutil.copy2(src, dst)
        return True
    return False


def main():
    """Build the initrd tree and pack it with cpio newc."""
    parser = argparse.ArgumentParser(description="Stage initrd.cpio.")
    parser.add_argument("--out", required=True)
    parser.add_argument("--test-config", required=True)
    parser.add_argument("--root", nargs="*", default=[])
    parser.add_argument("--bin", nargs="*", default=[])
    parser.add_argument("--user-dir", required=True)
    parser.add_argument("--tools-dir", required=True)
    parser.add_argument("--rust-hello", default=None)
    parser.add_argument("--rust-targets", default=None,
                        help="rust-targets dir holding x86_64-nexios.json")
    parser.add_argument("--libc-verify", default=None)
    parser.add_argument("--rust-sources", default=None)
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix="initrd_root_"))
    try:
        root = work / "initrd_root"
        (root / "bin").mkdir(parents=True)
        (root / "etc").mkdir(parents=True)
        (root / "tmp").mkdir(parents=True)
        (root / "tests").mkdir(parents=True)
        (root / "etc" / "fstab").write_text("tmpfs /tmp\n", encoding="utf-8")
        (root / "etc" / "rc").write_text("#!/bin/sh\n# Init script\n",
                                         encoding="utf-8")
        for elf in args.root:
            shutil.copy2(elf, root / Path(elf).name)
        for elf in args.bin:
            shutil.copy2(elf, root / "bin" / Path(elf).name)
        copy_if_present(args.rust_hello, root / "bin" / "rust-hello.elf")
        if args.rust_sources is not None:
            src_dir = Path(args.rust_sources)
            if (src_dir / "src" / "main.rs").exists():
                dest = root / "rust-hello"
                (dest / "src").mkdir(parents=True)
                for name in ["Cargo.toml", "Cargo.lock",
                             "rust-toolchain.toml"]:
                    copy_if_present(src_dir / name, dest / name)
                copy_if_present(src_dir / "src" / "main.rs",
                                dest / "src" / "main.rs")
                # Absolute source path (custom_target commands run with
                # cwd=build root, so a CWD-relative literal silently
                # misses). Flat dest under rust-hello/, mirroring the
                # Makefile cp.
                if args.rust_targets is not None:
                    copy_if_present(
                        Path(args.rust_targets) / "x86_64-nexios.json",
                        dest / "x86_64-nexios.json")
        if args.libc_verify is not None:
            copy_if_present(args.libc_verify,
                            root / Path(args.libc_verify).name)
        user_dir = Path(args.user_dir)
        for name in ["hey.c", "nullderef.c", "prime.c"]:
            shutil.copy2(user_dir / name, root / name)
        shutil.copy2(args.test_config, root / "tests" / "test-config.txt")
        (root / "tests" / "sh310.txt").write_text(SH310, encoding="utf-8")
        shutil.copy2(Path(args.tools_dir) / "xmodem276.txt",
                     root / "tests" / "xmodem276.txt")
        file_list = subprocess.run(
            ["find", ".", "-print0"], cwd=root, capture_output=True,
            check=True,
        ).stdout.split(b"\0")
        names = sorted(n for n in file_list if n and n != b".")
        cpio_input = b"\0".join(names) + b"\0"
        with open(args.out, "wb") as out:
            subprocess.run(
                ["cpio", "-o", "-H", "newc", "-0", "--quiet"],
                input=cpio_input, cwd=root, stdout=out, check=True,
            )
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
