#!/usr/bin/env python3
"""History-appender concurrency test (issue #170, A5).

Usage:
    python3 tools/meson/tests/test_history_lock.py <builddir> <arch>

Spawns N concurrent test_history.py writers against a scratch history
file and asserts N intact rows (no interleaving, no loss). Uses a
throwaway directory — never the real test-history.txt.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent.parent
APPEND = REPO / "tools" / "meson" / "test_history.py"
WRITERS = 8
ROWS_EACH = 25


def main():
    """Run concurrent appenders and verify row integrity."""
    if len(sys.argv) != 3:
        print("usage: test_history_lock.py <builddir> <arch>")
        return 2
    with tempfile.TemporaryDirectory(prefix="history-lock-") as tmp:
        target = Path(tmp) / "test-history.txt"
        import concurrent.futures as futures
        with futures.ThreadPoolExecutor(max_workers=WRITERS) as pool:
            pending = []
            for writer in range(WRITERS):
                for row in range(ROWS_EACH):
                    text = ("2026-01-01 00:00:%02d w%02dr%02d PASSED: 1 "
                            "FAILED: 0 TIME: 1ms" % (writer, writer, row))
                    pending.append(pool.submit(subprocess.run,
                                               [sys.executable, str(APPEND),
                                                str(target), text],
                                               capture_output=True))
            for future in pending:
                result = future.result()
                if result.returncode != 0:
                    print("appender failed: %s"
                          % result.stderr.decode(errors="replace"))
                    return 1
        lines = target.read_text(encoding="utf-8").splitlines()
        if len(lines) != WRITERS * ROWS_EACH:
            print("lost rows: got %d, want %d"
                  % (len(lines), WRITERS * ROWS_EACH))
            return 1
        bad = [line for line in lines
               if not line.endswith("TIME: 1ms") or line.count("PASSED:") != 1]
        if bad:
            print("torn rows: %d, e.g. %r" % (len(bad), bad[0][:80]))
            return 1
    print("history-lock PASSED: %d writers x %d rows intact"
          % (WRITERS, ROWS_EACH))
    return 0


if __name__ == "__main__":
    sys.exit(main())
