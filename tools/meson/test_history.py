#!/usr/bin/env python3
"""Exclusive-append test-history writer (issue #170).

Usage:
    python3 tools/meson/test_history.py <history-file> "<row...>"

Appends exactly one AGENTS.md-format row under a mkdir lock co-located
with the history file (atomic mkdir; stale-lock timeout + trap cleanup —
`flock` is absent on macOS). Used by qemu_test.py and run_all_classes.sh;
never hand-edit history files around it.
"""

import os
import sys
import time
from pathlib import Path

STALE_AFTER_SECONDS = 300


def _acquire(directory: Path, timeout_secs: int = 60):
    """Spin on atomic mkdir until the lock is ours or stale."""
    deadline = time.monotonic() + timeout_secs
    while True:
        try:
            directory.mkdir(parents=False, exist_ok=False)
            return
        except FileExistsError:
            try:
                age = time.time() - directory.stat().st_mtime
            except FileNotFoundError:
                continue
            if age > STALE_AFTER_SECONDS:
                try:
                    directory.rmdir()
                except OSError:
                    pass
            if time.monotonic() > deadline:
                raise TimeoutError("history lock wedged: %s" % directory)
            time.sleep(0.05)


def append_row(history_path: str, row: str):
    """Append one row (no trailing newline required) atomically."""
    path = Path(history_path)
    lock = path.parent / (path.name + ".lock")
    _acquire(lock)
    try:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(row if row.endswith("\n") else row + "\n")
    finally:
        try:
            lock.rmdir()
        except OSError:
            pass


def main():
    """CLI: append one row to the history file."""
    if len(sys.argv) != 3:
        print("usage: test_history.py <history-file> <row>")
        return 2
    try:
        append_row(sys.argv[1], sys.argv[2])
    except (TimeoutError, OSError) as exc:
        print("history append FAILED: %s" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
