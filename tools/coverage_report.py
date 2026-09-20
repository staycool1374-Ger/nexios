#!/usr/bin/env python3
"""Merge per-class dynamic coverage captures into one report (issue #122).

The kernel is built with ``-finstrument-functions``; every boot dumps the set
of functions that were actually entered (see docs/specs/coverage.md for the
frame format).  This script

  1. parses every ``build/coverage/<class>/serial.raw`` frame (checksummed),
  2. resolves the executed addresses against that class' ``kernel.elf``,
  3. unions the per-class results by (source file, function name),
  4. builds the function universe from the ELF symbol table and attributes it
     to source files with ``addr2line``,
  5. reports coverage per area and per file, worst first.

Itanium ABI C1/C2 constructor and D0/D1/D2 destructor twins are collapsed
to one entry (C1/D1) before unioning, so never-enterable base-object
variants do not inflate the denominator (issue #142).

Usage:
    python3 tools/coverage_report.py --dir build/coverage --out build/coverage
"""
import argparse
import bisect
import html
import os
import re
import struct
import subprocess
import sys

BEGIN = b"@@COVBEGIN@@\n"
END = b"\n@@COVEND@@\n"
ABORT = b"@@COVABORT@@"
NM = "x86_64-elf-nm"
ADDR2LINE = "x86_64-elf-addr2line"
FUNC_TYPES = ("T", "t", "W", "w")
# Cap for the per-area "never entered" function listing in the markdown report
# (the full set is always counted in the tables above).
UNCOVERED_PER_AREA = 600

# Itanium C++ ABI duplicate variants (issue #142): every non-trivial
# constructor is emitted twice (C1 = complete-object, C2 = base-object) and
# every destructor up to three times (D0 = deleting, D1 = complete-object,
# D2 = base-object). With -fno-inline (COVERAGE_FLAGS) the out-of-line copies
# survive as distinct weak symbols, but only the C1/D1 variant can ever be
# entered for a class that is never used as a base (e.g. CheckedPtr<T>).
# Counting each twin inflates the denominator with permanently uncoverable
# entries, so the twins are collapsed to one canonical entry (C1/D1).
# Ctor/dtor special names are the only non-length-prefixed components in a
# mangled nested name, hence the "not preceded by a digit" guard: a class
# literally named "C1" mangles as "2C1" and must not match (such twins are
# conservatively left uncollapsed).
ABI_VARIANT_RE = re.compile(r"(?<!\d)(C[12]|D[012])E")


def canonicalize_abi_variant(mangled):
    """Fold Itanium C2 -> C1 and D0/D2 -> D1 in a mangled symbol name."""
    def _fold(match):
        return ("C1" if match.group(1).startswith("C") else "D1") + "E"
    return ABI_VARIANT_RE.sub(_fold, mangled)


class Frame:
    """One parsed coverage frame."""

    def __init__(self, path, addrs, registered, dropped, valid, note):
        self.path = path
        self.name = os.path.basename(os.path.dirname(path))
        self.elf = ""
        self.addrs = addrs
        self.registered = registered
        self.dropped = dropped
        self.valid = valid
        self.note = note


def parse_frame(path, blob):
    """Extract the executed-address set from one serial capture."""
    start = blob.find(BEGIN)
    if start < 0:
        return Frame(path, set(), 0, 0, False, "no @@COVBEGIN@@ sentinel")
    if blob.find(ABORT) >= 0:
        return Frame(path, set(), 0, 0, False, "kernel reported @@COVABORT@@")
    start += len(BEGIN)
    stop = blob.find(END, start)
    if stop < 0:
        return Frame(path, set(), 0, 0, False, "no @@COVEND@@ sentinel")
    body = blob[start:stop]
    if len(body) < 12 or body[0:4] != b"FUNC":
        return Frame(path, set(), 0, 0, False, "frame does not start with FUNC")
    count = struct.unpack("<I", body[4:8])[0]
    offset = 8
    addrs = set()
    checksum = 0
    for byte in body[4:8]:
        checksum += byte
    for _ in range(count):
        if offset + 9 > len(body):
            return Frame(path, set(), 0, 0, False, "truncated entry stream")
        addr = struct.unpack("<Q", body[offset:offset + 8])[0]
        for byte in body[offset:offset + 9]:
            checksum += byte
        addrs.add(addr)
        offset += 9
    if body[offset:offset + 4] != b"STAT":
        return Frame(path, set(), 0, 0, False, "missing STAT trailer")
    offset += 4
    registered, dropped, emitted = struct.unpack("<III", body[offset:offset + 12])
    for byte in body[offset:offset + 8]:
        checksum += byte
    if (checksum & 0xFFFFFFFF) != emitted:
        return Frame(path, set(), 0, 0, False, "checksum mismatch")
    return Frame(path, addrs, registered, dropped, True, "ok")


def load_symbols(elf):
    """Return (sorted addresses, parallel name list) of defined functions."""
    proc = subprocess.run([NM, "-n", elf], capture_output=True, text=True)
    demangled = subprocess.run([NM, "-n", "-C", elf], capture_output=True,
                               text=True)
    if proc.returncode != 0:
        print(f"  nm failed for {elf}: {proc.stderr.strip()}")
        return [], [], []
    addrs, names, pretty = [], [], []
    # Identity is the canonicalized mangled name (two distinct instantiations
    # can demangle to the same text); the demangled run is only used for
    # display.
    demap = {}
    for line in demangled.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in FUNC_TYPES:
            demap[parts[0]] = " ".join(parts[2:])
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[1] not in FUNC_TYPES:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        addrs.append(addr)
        # Identity is the canonicalized mangled name: Itanium C1/C2 and
        # D0/D1/D2 twins (issue #142) count as one function. The demangled
        # run below is only used for display.
        names.append(canonicalize_abi_variant(" ".join(parts[2:])))
        pretty.append(demap.get(parts[0], " ".join(parts[2:])))
    return addrs, names, pretty


def lookup_symbol(addrs, addr):
    """Map an address to the symbol that contains it (addr <= x < next)."""
    if not addrs:
        return None
    idx = bisect.bisect_right(addrs, addr) - 1
    if idx < 0:
        return None
    return idx


def attribute(elf, addrs):
    """Return list of (function name, source file) for the given addresses."""
    if not addrs:
        return []
    query = "".join(f"{a:x}\n" for a in addrs)
    proc = subprocess.run([ADDR2LINE, "-e", elf, "-f", "-C"],
                          input=query, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"  addr2line failed for {elf}: {proc.stderr.strip()}")
        return []
    lines = proc.stdout.splitlines()
    out = []
    for i in range(0, len(lines) - 1, 2):
        name = lines[i].strip()
        loc = lines[i + 1].strip()
        src = loc.split(":")[0]
        out.append((name, src))
    return out


def area_of(src, is_test):
    """Classify a source path into an area bucket."""
    norm = src.replace("\\", "/")
    while norm.startswith("./") or norm.startswith("../"):
        norm = norm[2:] if norm.startswith("./") else norm[3:]
    if is_test:
        return "kernel/test"
    marker = norm.find("src/")
    if marker < 0:
        return "other"
    rest = norm[marker + 4:]
    parts = rest.split("/")
    if parts[0] == "kernel":
        if len(parts) == 1:
            return "kernel"
        # A top-level file (src/kernel/foo.cpp) is its own path segment but
        # not its own area — only real subdirectories form areas.
        if "." in parts[1]:
            return "kernel"
        return f"kernel/{parts[1]}"
    if parts[0] == "lib":
        return "lib"
    return parts[0]


# Files scoped out of the coverage denominator (issue #183): the FDT
# library is reachable only through the AARCH64/RISCV64 boot-DTB consumer
# (kernel.cpp), so on x86 test boots its functions are permanently
# uncoverable. Report-side exclusion only — instrumentation is kept so raw
# dumps still record entries from the static-blob unit test.
COVERAGE_SCOPED_OUT = ("src/lib/fdt/",)


def is_scoped_out(src):
    """True when a source path is excluded from the coverage denominator."""
    norm = src.replace("\\", "/")
    return any(m in norm for m in COVERAGE_SCOPED_OUT)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True, help="build/coverage root")
    parser.add_argument("--out", required=True, help="report output directory")
    parser.add_argument("--allow-truncated", action="store_true",
                        help="merge even when a capture failed validation")
    args = parser.parse_args()

    root = args.dir
    classes = sorted(d for d in os.listdir(root)
                     if os.path.isdir(os.path.join(root, d)))
    frames = []
    for name in classes:
        raw = os.path.join(root, name, "serial.raw")
        elf = os.path.join(root, name, "kernel.elf")
        if not (os.path.exists(raw) and os.path.exists(elf)):
            continue
        with open(raw, "rb") as handle:
            frame = parse_frame(raw, handle.read())
        frame.name = name
        frame.elf = elf
        frames.append(frame)
        status = "ok" if frame.valid else f"INVALID ({frame.note})"
        print(f"  {name:<24} {len(frame.addrs):>6} functions  {status}")

    if not frames:
        print("Error: no build/coverage/<class>/serial.raw captures found")
        return 1

    bad = [f for f in frames if not f.valid]
    if bad and not args.allow_truncated:
        print("Error: invalid captures present — "
              "re-run those classes or pass --allow-truncated")
        return 1
    if any(f.dropped for f in frames if f.valid):
        print("Error: coverage table overflowed (dropped > 0) — "
              "raise COVERAGE_MAX_FUNCS and re-run")
        return 1

    # Union per class, keyed by (source file, function name) — relinking
    # shifts addresses, so names/files are the stable identity.
    covered = {}          # (src, func) -> set of classes
    pretty_names = {}     # (src, func) -> demangled display name
    universe = {}         # (src, func) -> True
    for frame in frames:
        if not frame.valid:
            continue
        sym_addrs, sym_names, sym_pretty = load_symbols(frame.elf)
        idxs = [lookup_symbol(sym_addrs, a) for a in frame.addrs]
        resolved = set(i for i in idxs if i is not None)
        # Universe first (all defined functions), then coverage.
        table = attribute(frame.elf, sym_addrs)
        for i, (name, src) in enumerate(table):
            if src == "??" or not src:
                continue
            if is_scoped_out(src):
                continue
            is_test = "/kernel/test/" in src or src.startswith("src/kernel/test")
            key = (src, sym_names[i] if i < len(sym_names) else name,
                   area_of(src, is_test))
            universe[key] = True
            if i < len(sym_pretty):
                pretty_names.setdefault(key, sym_pretty[i])
        for i in resolved:
            if i >= len(table):
                continue
            name, src = table[i]
            if src == "??" or not src:
                continue
            if is_scoped_out(src):
                continue
            is_test = "/kernel/test/" in src or src.startswith("src/kernel/test")
            key = (src, sym_names[i] if i < len(sym_names) else name,
                   area_of(src, is_test))
            universe[key] = True
            if i < len(sym_pretty):
                pretty_names.setdefault(key, sym_pretty[i])
            covered.setdefault(key, set()).add(frame.name)

    if not universe:
        print("Error: no source-attributable functions found (missing -g?)")
        return 1

    def bucket(items, only_kernel):
        total, cov = {}, {}
        for key in items:
            src, _, area = key
            if only_kernel and (area == "kernel/test" or area == "other"):
                continue
            total[area] = total.get(area, 0) + 1
            if key in covered:
                cov[area] = cov.get(area, 0) + 1
        return total, cov

    total_area, cov_area = bucket(universe, True)
    total_all, cov_all = bucket(universe, False)

    file_total, file_cov = {}, {}
    for key in universe:
        src = key[0]
        file_total[src] = file_total.get(src, 0) + 1
        if key in covered:
            file_cov[src] = file_cov.get(src, 0) + 1

    def pct(num, den):
        return (num / den * 100.0) if den else 0.0

    grand_total = sum(total_area.values())
    grand_cov = sum(cov_area.values())

    lines = []
    lines.append("# Dynamic test coverage (function level)\n")
    lines.append(f"Classes merged: {len([f for f in frames if f.valid])} "
                 f"({', '.join(f.name for f in frames if f.valid)})\n")
    lines.append(f"Kernel functions executed: **{grand_cov}/{grand_total} "
                 f"({pct(grand_cov, grand_total):.1f}%)** "
                 f"(excludes test code and unmapped symbols)\n")
    if "kernel/test" in total_all:
        lines.append(f"Test-code functions executed: "
                     f"{cov_all.get('kernel/test', 0)}/"
                     f"{total_all['kernel/test']} (informational)\n")
    lines.append("Note: `kernel/gcov` is the coverage handler itself — it is "
                 "deliberately excluded from instrumentation, so 0% there is "
                 "expected.\n")
    lines.append("Note: Itanium ABI duplicate variants are collapsed — C1/C2 "
                 "constructors count as one entry (C1) and D0/D1/D2 "
                 "destructors as one (D1). The base-object/deleting twins are "
                 "never entered for classes that are never used as bases, so "
                 "counting them would inflate the denominator with "
                 "permanently uncoverable entries (issue #142).\n")
    lines.append("Note: `src/lib/fdt/*` is scoped out of the denominator "
                 "(issue #183) — the FDT library is reachable only via the "
                 "AARCH64/RISCV64 boot-DTB consumer, so it is permanently "
                 "uncoverable on x86 test boots. It is measured instead by "
                 "the static-blob `lib_fdt_*` unit tests.\n")
    lines.append("\n## Coverage per area (worst first)\n")
    lines.append("| Area | Covered | Total | % |")
    lines.append("|---|---:|---:|---:|")
    for area in sorted(total_area, key=lambda a: pct(cov_area.get(a, 0),
                                                     total_area[a])):
        lines.append(f"| {area} | {cov_area.get(area, 0)} | {total_area[area]} | "
                     f"{pct(cov_area.get(area, 0), total_area[area]):.1f}% |")
    def shorten(path):
        """Render an absolute build path as a repo-relative one."""
        idx = path.find("src/")
        return path[idx:] if idx >= 0 else path

    lines.append("\n## Least covered files (kernel, worst 40)\n")
    lines.append("| File | Covered | Total | % |")
    lines.append("|---|---:|---:|---:|")
    ranked = sorted(
        (f for f in file_total
         if not shorten(f).startswith("src/kernel/test")),
        key=lambda f: pct(file_cov.get(f, 0), file_total[f]))
    for src in ranked[:40]:
        lines.append(f"| `{shorten(src)}` | {file_cov.get(src, 0)} | "
                     f"{file_total[src]} | "
                     f"{pct(file_cov.get(src, 0), file_total[src]):.1f}% |")

    gaps = sorted(
        ((file_total[f] - file_cov.get(f, 0), f) for f in file_total
         if not shorten(f).startswith("src/kernel/test")),
        reverse=True)
    lines.append("\n## Largest absolute gaps (kernel, top 25)\n")
    lines.append("| File | Uncovered | Covered | Total |")
    lines.append("|---|---:|---:|---:|")
    for gap, src in gaps[:25]:
        if gap <= 0:
            break
        lines.append(f"| `{shorten(src)}` | {gap} | {file_cov.get(src, 0)} | "
                     f"{file_total[src]} |")
    # Per-area list of the functions that were never executed (top N each) —
    # this is what turns "area X is at 46 %" into actionable work items.
    lines.append("\n## Uncovered functions per area (never entered)\n")
    by_area = {}
    for key in universe:
        if key in covered:
            continue
        src, name, area = key
        if shorten(src).startswith("src/kernel/test"):
            continue
        by_area.setdefault(area, []).append(
            (shorten(src), pretty_names.get(key, name)))
    for area in sorted(by_area, key=lambda a: -len(by_area[a])):
        entries = sorted(by_area[area])[:UNCOVERED_PER_AREA]
        lines.append(f"\n### {area} — {len(by_area[area])} uncovered\n")
        for src, name in entries:
            lines.append(f"- `{shorten(name)}` ({src})")
        if len(by_area[area]) > UNCOVERED_PER_AREA:
            lines.append(f"- … and {len(by_area[area]) - UNCOVERED_PER_AREA} more")

    zero = [f for f in ranked if file_cov.get(f, 0) == 0]
    lines.append(f"\n## Files with 0% coverage: {len(zero)}\n")
    for src in zero:
        lines.append(f"- `{shorten(src)}` ({file_total[src]} functions)")

    os.makedirs(args.out, exist_ok=True)
    md_path = os.path.join(args.out, "report.md")
    with open(md_path, "w") as handle:
        handle.write("\n".join(lines) + "\n")

    def rows(mapping, totals):
        out = ""
        for key in sorted(totals, key=lambda k: pct(mapping.get(k, 0),
                                                    totals[k])):
            value = pct(mapping.get(key, 0), totals[key])
            color = "#f44336" if value < 25 else (
                "#ff9800" if value < 75 else "#4CAF50")
            out += (f'<tr><td><code>{html.escape(str(key))}</code></td>'
                    f"<td>{mapping.get(key, 0)}</td><td>{totals[key]}</td>"
                    f'<td style="color:{color}">{value:.1f}%</td></tr>\n')
        return out

    html_doc = f"""<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>NexIOS dynamic coverage</title>
<style>
body {{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:20px}}
table {{border-collapse:collapse;width:100%}}
th,td {{text-align:left;padding:6px;border-bottom:1px solid #ddd}}
th {{background:#333;color:#fff}}
</style></head>
<body>
<h1>NexIOS dynamic coverage (function level)</h1>
<p>Kernel functions executed: <b>{grand_cov}/{grand_total}
({pct(grand_cov, grand_total):.1f}%)</b></p>
<h2>Per area</h2>
<table><tr><th>Area</th><th>Covered</th><th>Total</th><th>%</th></tr>
{rows(cov_area, total_area)}
</table>
<h2>Per file (worst 100)</h2>
<table><tr><th>File</th><th>Covered</th><th>Total</th><th>%</th></tr>
{rows({f: file_cov.get(f, 0) for f in ranked[:100]},
      {f: file_total[f] for f in ranked[:100]})}
</table>
</body></html>"""
    html_path = os.path.join(args.out, "report.html")
    with open(html_path, "w") as handle:
        handle.write(html_doc)

    print(f"\nKernel coverage: {grand_cov}/{grand_total} functions "
          f"({pct(grand_cov, grand_total):.1f}%)")
    for area in sorted(total_area, key=lambda a: pct(cov_area.get(a, 0),
                                                     total_area[a]))[:12]:
        print(f"  {area:<28} {cov_area.get(area, 0):>5}/{total_area[area]:<5} "
              f"{pct(cov_area.get(area, 0), total_area[area]):5.1f}%")
    print(f"\nReport: {md_path}\nReport: {html_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
