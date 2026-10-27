#!/usr/bin/env python3
"""Generate uniform Ring-3 syscall stubs from src/libc/syscall.h.

Parses the `#define SYS_<NAME> <num>` table (the single source of truth,
spec syscall-abi-picolibc.md section 3) and emits one stub per defined
number into src/libc/gen/nexios_syscalls.{h,c}. Every stub routes its
four arguments through `__syscall5r` with an explicit zero fifth
register (deterministic trap state on all arches; the P1 crt0 zeroed-arg
discipline extended to stubs) and maps failures through the section 2
error predicate (`errno = -ret, return -1` for -4095..-1, verbatim from
picolib_stubs.c `map_err`).

Excluded numbers (with reasons, review-gated — SIL 3: a wrong stub
forges syscalls):
  SYS_MAX           — a table count, not a syscall number.
  74/75/76           — SEND_FAST / RECV_FAST / SEND_SYNC_FAST use the
                      x86-only register-passing layout (payload in
                      rsi/rdi/r8-r11, ipc-fastpath section 3.2), not the
                      standard section 2 (number, arg0..arg3) layout, so
                      a `__syscall5r` stub would forge the call. Their
                      hand-written stubs in syscall.h stay canonical.

Numbers without a SYS_* define (today 86-90, the kernel TASK_DEBUG
band) get no stub: the generator cannot invent names, and calling an
unnamed number through a stub would bypass the table. That gap is
table completeness, owned by the syscall_abi drift test — not by this
generator.

Output is byte-deterministic: entries sorted by number, fixed
template, no timestamps, no absolute paths. Re-running on an
unchanged table must produce byte-identical files (`--verify` enforces
this against the checked-in copies).
"""

import argparse
import difflib
import os
import re
import sys

RE_DEFINE = re.compile(r"^#define\s+(SYS_[A-Z0-9_]+)\s+(\d+)\s*$")

EXCLUDED_NUMBERS = {
    74: "SEND_FAST uses the x86-only register-passing layout, not section 2",
    75: "RECV_FAST uses the x86-only register-passing layout, not section 2",
    76: "SEND_SYNC_FAST uses the x86-only register layout, not section 2",
}

EXCLUDED_NAMES = {
    "SYS_MAX": "table count, not a syscall number",
}

GPL_HEADER = """/*
 * NexIOS RTOS — Generated syscall stubs (issue #171 P2).
 *
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
"""


def parse_table(header_path):
    """Parse SYS_* number defines. Returns [(number, name)]."""
    entries = []
    try:
        with open(header_path, "r", encoding="utf-8") as handle:
            text = handle.read().splitlines()
    except OSError as exc:
        print(f"error: cannot read {header_path}: {exc}", file=sys.stderr)
        sys.exit(1)
    for line in text:
        match = RE_DEFINE.match(line.strip())
        if match is None:
            continue
        name, number = match.group(1), int(match.group(2))
        if name in EXCLUDED_NAMES:
            continue
        entries.append((number, name))
    return entries


def check_table(entries):
    """Refuse ambiguous input: duplicate numbers or names. Returns kept."""
    seen_numbers = {}
    seen_names = set()
    kept = []
    failed = False
    for number, name in entries:
        if name in seen_names:
            print(f"error: duplicate define {name}", file=sys.stderr)
            failed = True
            continue
        seen_names.add(name)
        if number in seen_numbers:
            print(
                f"error: number {number} defined twice "
                f"({seen_numbers[number]} and {name})",
                file=sys.stderr,
            )
            failed = True
            continue
        seen_numbers[number] = name
        if number in EXCLUDED_NUMBERS:
            continue
        kept.append((number, name))
    if failed:
        sys.exit(1)
    kept.sort(key=lambda entry: entry[0])
    return kept


def report_gaps(kept):
    """List numbers with no SYS_* define (informational only)."""
    if not kept:
        return
    numbers = {number for number, _ in kept}
    missing = [
        number
        for number in range(kept[0][0], kept[-1][0] + 1)
        if number not in numbers
    ]
    for number in missing:
        reason = EXCLUDED_NUMBERS.get(number, "no SYS_* define in syscall.h")
        print(f"note: no stub for {number} ({reason})")


def render_header(kept, header_rel):
    """Render the generated public header text."""
    lines = []
    lines.append(GPL_HEADER.rstrip("\n"))
    lines.append("")
    lines.append("/// @file gen/nexios_syscalls.h")
    lines.append("/// @brief Generated uniform Ring-3 syscall stubs (issue #171 P2).")
    lines.append("/// @details GENERATED FILE — do not edit. Regenerate with")
    lines.append("/// `tools/gen_syscall_stubs.py`; see that script for the")
    lines.append("/// exclusion list and determinism contract.")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("/// @brief Section 2 error predicate shared by every stub.")
    lines.append("/// @param ret raw syscall return value.")
    lines.append("/// @return -1 with errno set for -4095..-1, else ret verbatim.")
    lines.append("long nexios_stub_map_err(long ret);")
    lines.append("")
    for number, name in kept:
        lower = name[len("SYS_"):].lower()
        lines.append(f"/// @brief Generated stub for {name} ({number}).")
        lines.append("/// @param arg0 first syscall argument.")
        lines.append("/// @param arg1 second syscall argument.")
        lines.append("/// @param arg2 third syscall argument.")
        lines.append("/// @param arg3 fourth syscall argument.")
        lines.append("/// @return raw success value, or -1 with errno set.")
        lines.append(f"long nexios_sys_{lower}(")
        lines.append("    long arg0, long arg1, long arg2, long arg3);")
    lines.append("")
    _ = header_rel
    return "\n".join(lines) + "\n"


def render_source(kept):
    """Render the generated stub definitions text."""
    lines = []
    lines.append(GPL_HEADER.rstrip("\n"))
    lines.append("")
    lines.append("/// @file gen/nexios_syscalls.c")
    lines.append("/// @brief Generated uniform Ring-3 syscall stubs (issue #171 P2).")
    lines.append("/// @details GENERATED FILE — do not edit. Regenerate with")
    lines.append("/// `tools/gen_syscall_stubs.py`. Every stub routes")
    lines.append("/// (arg0..arg3) through `__syscall5r` with an explicit zero")
    lines.append("/// fifth register: deterministic trap state on all arches, and")
    lines.append("/// the kernel documents reply_buf == 0 as no-reply for the one")
    lines.append("/// reader (SEND_SYNC). Failures map through the section 2")
    lines.append("/// predicate; -4095 maps to errno, -4097 stays a success value.")
    lines.append("")
    lines.append('#include "nexios_syscalls.h"')
    lines.append("#include <errno.h>")
    lines.append("#include <syscall.h>")
    lines.append("")
    lines.append("long nexios_stub_map_err(long ret) {")
    lines.append("    if (ret < 0 && (unsigned long)ret > -4096UL) {")
    lines.append("        errno = (int)(-ret);")
    lines.append("        return -1;")
    lines.append("    }")
    lines.append("    return ret;")
    lines.append("}")
    lines.append("")
    for number, name in kept:
        lower = name[len("SYS_"):].lower()
        lines.append(f"long nexios_sys_{lower}(")
        lines.append("    long arg0, long arg1, long arg2, long arg3) {")
        lines.append("    return nexios_stub_map_err(__syscall5r(")
        lines.append(f"        {name}, arg0, arg1, arg2, arg3, 0));")
        lines.append("}")
        lines.append("")
    return "\n".join(lines) + "\n"


def write_or_verify(path, content, verify):
    """Write content, or diff against the checked-in file in verify mode."""
    if not verify:
        try:
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(content)
        except OSError as exc:
            print(f"error: cannot write {path}: {exc}", file=sys.stderr)
            sys.exit(1)
        return
    try:
        with open(path, "r", encoding="utf-8") as handle:
            current = handle.read()
    except OSError as exc:
        print(f"error: cannot read {path}: {exc}", file=sys.stderr)
        sys.exit(1)
    if current != content:
        diff = difflib.unified_diff(
            current.splitlines(),
            content.splitlines(),
            fromfile=f"{path} (checked in)",
            tofile=f"{path} (regenerated)",
            lineterm="",
        )
        print("\n".join(diff))
        print(f"error: {path} differs from regenerated output", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="Generate uniform Ring-3 syscall stubs from syscall.h."
    )
    parser.add_argument(
        "--header",
        default="src/libc/syscall.h",
        help="Input syscall.h table (repo-relative or absolute).",
    )
    parser.add_argument(
        "--out-header",
        default="src/libc/gen/nexios_syscalls.h",
        help="Output path for the generated header.",
    )
    parser.add_argument(
        "--out-source",
        default="src/libc/gen/nexios_syscalls.c",
        help="Output path for the generated source.",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="Regenerate and diff against the checked-in files instead "
             "of writing; exit nonzero on any difference (determinism / "
             "drift guard).",
    )
    args = parser.parse_args()

    entries = parse_table(args.header)
    kept = check_table(entries)
    report_gaps(kept)

    header_text = render_header(kept, args.out_header)
    source_text = render_source(kept)
    write_or_verify(args.out_header, header_text, args.verify)
    write_or_verify(args.out_source, source_text, args.verify)
    if not args.verify:
        print(f"generated {args.out_header}: {len(kept)} stubs")
        print(f"generated {args.out_source}: {len(kept)} stubs")
    else:
        print(f"verified {args.out_header} + {args.out_source}: "
              f"{len(kept)} stubs identical")


if __name__ == "__main__":
    main()
