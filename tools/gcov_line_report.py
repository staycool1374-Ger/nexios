#!/usr/bin/env python3
"""Phase A (issue #123): real gcov line/branch coverage for NexIOS.

Reads the per-class COM1 captures produced by the -fprofile-arcs kernel
(COVERAGE_PHASE=line), reconstructs the .gcda files, merges the per-class runs
with x86_64-elf-gcov-tool, runs x86_64-elf-gcov and finally lcov/genhtml.

Wire format emitted by src/kernel/gcov/gcov_handler.cpp:

    \\n@@GCDABEGIN@@\\n
    "GCOV:" <len> ":" <filename> "\\n" <len bytes of gcda payload>
    ... one record per instrumented translation unit ...
    "STAT:" <tus> ":" <dropped> ":" <checksum> ":" <ctors seen> ":" <invoked> "\\n"
    \\n@@GCDAEND@@\\n

A capture is only used when both sentinels are present and every payload has
its declared length; @@GCDAABORT@@ marks a truncated stream and is rejected.

Usage:
    python3 tools/gcov_line_report.py --dir build/coverage --out build/coverage
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

BEGIN = b"@@GCDABEGIN@@"
END = b"@@GCDAEND@@"
ABORT = b"@@GCDAABORT@@"
FRAME_RE = re.compile(rb"GCOV:(\d+):([^\n]*)\n")
STAT_RE = re.compile(rb"STAT:([^\n]*)")

GCOV = os.environ.get("GCOV_TOOL", "x86_64-elf-gcov")
GCOV_MERGE = os.environ.get("GCOV_MERGE_TOOL", "x86_64-elf-gcov-tool")
LCOV = os.environ.get("LCOV_TOOL", "lcov")
GENHTML = os.environ.get("GENHTML_TOOL", "genhtml")


def log(msg):
    print(msg, flush=True)


def parse_capture(path):
    """Return (frames, stats) for one serial capture, or None if invalid."""
    with open(path, "rb") as handle:
        data = handle.read()

    if BEGIN not in data:
        return None
    if ABORT in data:
        return None
    if END not in data:
        return None

    stats = None
    match = STAT_RE.search(data)
    if match:
        stats = match.group(1).decode("latin1")

    frames = []
    for frame in FRAME_RE.finditer(data):
        length = int(frame.group(1))
        name = frame.group(2).decode("latin1")
        payload = data[frame.end():frame.end() + length]
        if len(payload) != length:
            return None  # truncated payload -> reject the whole capture
        frames.append((name, payload))
    return frames, stats


def gcda_relpath(name, cwd):
    """Map the kernel-reported absolute .gcda path to a repo-relative one.

    The kernel reports absolute paths (``-fprofile-abs-path``), e.g.
    /repo/build/initrd/initrd.gcda.  The .gcno lives at build/initrd/initrd.gcno,
    so the merged tree must mirror the *repo-relative* path.
    """
    path = os.path.normpath(name)
    if path.startswith(cwd):
        return os.path.relpath(path, cwd)
    return path.lstrip("/")


def write_gcda(frames, root, cwd):
    """Write each payload to <root>/<repo-relative gcda path>."""
    written = []
    for name, payload in frames:
        if not name.endswith(".gcda"):
            continue
        target = os.path.join(root, gcda_relpath(name, cwd))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as handle:
            handle.write(payload)
        written.append(target)
    return written


def _leaf_gcda_dirs(root):
    """Map relative dir -> sorted .gcda basenames for one class tree."""
    leaves = {}
    for dirpath, _dirnames, files in os.walk(root):
        gcdas = sorted(entry for entry in files if entry.endswith(".gcda"))
        if gcdas:
            leaves[os.path.relpath(dirpath, root)] = gcdas
    return leaves


def merge_dirs(source_dirs, out_dir):
    """Fold per-class gcda directories together with gcov-tool merge.

    gcov-tool merge (gcov 16.1) only processes TOP-LEVEL *.gcda files:
    anything in a subdirectory is enumerated and then reported as
    "Skip" (verified: flat same-name pairs merge with exact counter
    sums, e.g. 327+291=618; nested pairs skip).  Our per-class trees
    mirror repo-relative paths (build/...), so merge each relative leaf
    directory separately into a mirrored output tree.  Merging is
    per-file (counter arrays summed per function id), therefore
    leaf-by-leaf folding is byte-equivalent to a recursive merge.
    """
    if not source_dirs:
        return None
    if len(source_dirs) == 1:
        shutil.copytree(source_dirs[0], out_dir, dirs_exist_ok=True)
        return out_dir

    current = source_dirs[0]
    for index, nxt in enumerate(source_dirs[1:], start=1):
        step_dir = os.path.join(out_dir + ".step%d" % index)
        if os.path.exists(step_dir):
            shutil.rmtree(step_dir)
        current_leaves = _leaf_gcda_dirs(current)
        next_leaves = _leaf_gcda_dirs(nxt)
        for rel in sorted(set(current_leaves) | set(next_leaves)):
            first = os.path.join(current, rel)
            second = os.path.join(nxt, rel)
            dest = os.path.join(step_dir, rel)
            os.makedirs(dest, exist_ok=True)
            if os.path.isdir(first) and os.path.isdir(second):
                result = subprocess.run(
                    [GCOV_MERGE, "merge", first, second, "-o", dest],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                if result.returncode != 0:
                    raise RuntimeError(
                        "gcov-tool merge failed for leaf %s" % rel)
            else:
                # Leaf exists on one side only: union, not merge.
                shutil.copytree(first if os.path.isdir(first) else second,
                                dest, dirs_exist_ok=True)
        current = step_dir

    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    shutil.copytree(current, out_dir)
    return out_dir


GCOV_LINE_RE = re.compile(
    r"^\s*(?P<count>-|\d+|[#$%]+)\s*:\s*(?P<line>\d+)\s*:")
GCOV_BRANCH_RE = re.compile(r"^\s*branch\s+\d+\s+(?P<taken>.*)$")


def summarise_with_gcov(gcno_root, out_dir):
    """Run x86_64-elf-gcov per .gcno and parse the classic .gcov text output.

    lcov 2.4 cannot parse gcov 16.1.0 output ("inconsistent: mismatched end
    line"), so the authoritative numbers come from here: a line is *covered*
    when its marker is a number (execution count) and *executable-but-missed*
    when it is ##### / $$$$$ / %%%%%.  A dash marks non-code and is ignored.
    """
    per_file = {}
    gcov_dir = os.path.join(out_dir, "gcov")
    if os.path.exists(gcov_dir):
        shutil.rmtree(gcov_dir)
    os.makedirs(gcov_dir, exist_ok=True)

    # Run gcov from the directory holding the gcno/gcda pair: that is the only
    # invocation form this toolchain reliably accepts (a separate -o output
    # directory silently produces nothing).  -b adds branch-probability
    # lines; line markers are unchanged, so line hit/miss semantics hold.
    for root, _dirs, files in os.walk(gcno_root):
        for entry in files:
            if not entry.endswith(".gcno"):
                continue
            if not os.path.exists(os.path.join(root, entry[:-5] + ".gcda")):
                continue
            subprocess.run([GCOV, "-b", entry], cwd=root,
                           capture_output=True, check=False)

    for root, _dirs, files in os.walk(gcno_root):
        for entry in files:
            if not entry.endswith(".gcov"):
                continue
            source = None
            hit = missed = 0
            branch_hit = branch_total = 0
            with open(os.path.join(root, entry), "r",
                      encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    if "Source:" in line and source is None:
                        source = line.split("Source:", 1)[1].strip()
                        continue
                    branch = GCOV_BRANCH_RE.match(line)
                    if branch:
                        # A branch counts as taken unless never executed.
                        branch_total += 1
                        if "never executed" not in branch.group("taken"):
                            branch_hit += 1
                        continue
                    match = GCOV_LINE_RE.match(line)
                    if not match:
                        continue
                    marker = match.group("count")
                    if marker == "-":
                        continue
                    if marker.isdigit():
                        hit += 1
                    else:
                        missed += 1
            if source is None:
                source = entry[:-5]
            previous = per_file.get(source, (0, 0, 0, 0, 0))
            per_file[source] = (previous[0] + 1, previous[1] + hit,
                                previous[2] + missed,
                                previous[3] + branch_hit,
                                previous[4] + branch_total)

    records = []
    for source, (_parts, hit, missed, branch_hit, branch_total) in \
            per_file.items():
        total = hit + missed
        records.append((source, hit, total, branch_hit, branch_total))
    return sorted(records, key=lambda item: item[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", required=True, help="build/coverage root")
    parser.add_argument("--out", required=True, help="report output directory")
    parser.add_argument("--gcno-root", default="build",
                        help="directory holding the .gcno files (default: build)")
    args = parser.parse_args()

    classes = sorted(
        entry for entry in os.listdir(args.dir)
        if os.path.isfile(os.path.join(args.dir, entry, "serial.raw"))
    )
    if not classes:
        log("No captures found in %s" % args.dir)
        return 1

    per_class = []
    for name in classes:
        parsed = parse_capture(os.path.join(args.dir, name, "serial.raw"))
        if not parsed:
            log("  %-32s INVALID (sentinels/abort/truncated)" % name)
            continue
        frames, stats = parsed
        gcda_root = os.path.join(args.dir, name, "gcda")
        if os.path.exists(gcda_root):
            shutil.rmtree(gcda_root)
        written = write_gcda(frames, gcda_root, os.getcwd())
        log("  %-32s %4d gcda files   STAT=%s" % (name, len(written), stats))
        per_class.append(gcda_root)

    if not per_class:
        log("No valid captures.")
        return 1

    merged = os.path.join(args.dir, "gcda-merged")
    if os.path.exists(merged):
        shutil.rmtree(merged)
    merge_dirs(per_class, merged)
    merged_files = sum(1 for _root, _d, files in os.walk(merged)
                       for _f in files if _f.endswith(".gcda")) if os.path.isdir(
                           merged) else 0
    if merged_files == 0:
        # An empty merge is a hard error, never a silent single-class
        # report: the header below would otherwise masquerade one class
        # as merged.  Keep the first-class fallback so partial numbers
        # stay available, but say so loudly.
        log("ERROR: gcov-tool merge produced no files - falling back to "
            "the first class only. Cross-class merging is NOT applied.")
        if os.path.exists(merged):
            shutil.rmtree(merged)
        shutil.copytree(per_class[0], merged)
    log("Merged %d class runs -> %s" % (len(per_class), merged))

    # Place each merged gcda next to its .gcno so gcov/lcov can find it.
    installed = 0
    for root, _dirs, files in os.walk(merged):
        for entry in files:
            if not entry.endswith(".gcda"):
                continue
            source = os.path.join(root, entry)
            relative = os.path.relpath(source, merged)
            # `relative` is repo-relative (build/<...>).  Re-base it onto the
            # gcno root, i.e. build/ + <...>.
            if not relative.startswith(args.gcno_root.rstrip("/") + os.sep):
                continue
            inner = os.path.relpath(relative, args.gcno_root)
            target = os.path.join(args.gcno_root, inner)
            if os.path.commonpath([os.path.abspath(target),
                                   os.path.abspath(args.gcno_root)]) != \
                    os.path.abspath(args.gcno_root):
                continue
            if os.path.exists(target):
                os.remove(target)
            shutil.copyfile(source, target)
            installed += 1
    log("Installed %d merged .gcda files into %s/" % (installed, args.gcno_root))

    os.makedirs(args.out, exist_ok=True)
    info = os.path.join(args.out, "coverage.info")

    if shutil.which(LCOV):
        subprocess.run(
            [LCOV, "--capture", "--directory", args.gcno_root,
             "--gcov-tool", GCOV, "--output-file", info,
             "--exclude", "*/coverage/*",
             "--ignore-errors", "mismatch,empty,source,path,inconsistent,"
                                "unused"],
            check=False,
        )
    if os.path.exists(info) and shutil.which(GENHTML):
        # lcov 2.4 vs gcov 16.1 end-line skew (issue #147 follow-up C):
        # "inconsistent" records are skipped, "unused" covers the
        # coverage/* exclude when no such files remain.  line-report.md
        # (from gcov text output) stays authoritative; HTML is best-effort.
        subprocess.run(
            [GENHTML, info, "--output-directory",
             os.path.join(args.out, "html"),
             "--ignore-errors", "source,inconsistent", "--legend", "--title",
             "NexIOS line coverage"],
            check=False,
        )

    records = summarise_with_gcov(args.gcno_root, args.out)

    stale = []
    if os.path.exists(info):
        source = None
        found = hit = 0
        with open(info, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith("SF:"):
                    source = line[3:]
                    found = hit = 0
                elif line.startswith("LF:"):
                    found = int(line[3:])
                elif line.startswith("LH:"):
                    hit = int(line[3:])
                elif line == "end_of_record" and source:
                    stale.append((source, hit, found))
                    source = None

    records.sort(key=lambda item: item[0])
    total_lines = sum(item[2] for item in records)
    total_hit = sum(item[1] for item in records)
    pct = (100.0 * total_hit / total_lines) if total_lines else 0.0
    total_branches = sum(item[4] for item in records)
    total_branches_hit = sum(item[3] for item in records)
    branch_pct = (100.0 * total_branches_hit / total_branches) \
        if total_branches else 0.0

    report = os.path.join(args.out, "line-report.md")
    with open(report, "w", encoding="utf-8") as handle:
        handle.write("# Phase A — line/branch coverage (issue #123)\n\n")
        handle.write("Merged %d class runs: %s\n\n"
                     % (len(per_class), ", ".join(classes)))
        handle.write("lcov info: `%s`  \nHTML tree: `%s`\n\n"
                     % (info, os.path.join(args.out, "html")))
        handle.write("## Totals\n\n")
        handle.write("| Lines | Hit | Coverage | Branches | BrHit | BrCoverage |\n"
                     "|---:|---:|---:|---:|---:|---:|\n")
        handle.write("| %d | %d | %.1f%% | %d | %d | %.1f%% |\n\n"
                     % (total_lines, total_hit, pct, total_branches,
                        total_branches_hit, branch_pct))
        handle.write("## Per file (worst first)\n\n")
        handle.write("| File | Hit | Lines | % | BrHit | Br | Br% |\n"
                     "|---|---:|---:|---:|---:|---:|---:|\n")
        for source, hit, found, branch_hit, branch_total in sorted(
                records, key=lambda i: (i[2] and i[1] / i[2]) or 0.0):
            if not found:
                continue
            branch = (100.0 * branch_hit / branch_total) if branch_total \
                else 0.0
            handle.write("| `%s` | %d | %d | %.1f%% | %d | %d | %.1f%% |\n"
                         % (source, hit, found, 100.0 * hit / found,
                            branch_hit, branch_total, branch))
    log("\nLines: %d  Hit: %d  Coverage: %.1f%%" % (total_lines, total_hit, pct))
    log("Branches: %d  BrHit: %d  BrCoverage: %.1f%%"
        % (total_branches, total_branches_hit, branch_pct))
    log("Report: %s" % report)
    log("HTML:    %s" % os.path.join(args.out, "html"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
