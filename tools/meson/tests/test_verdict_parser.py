#!/usr/bin/env python3
"""Verdict-contract tests for the Meson harness (issue #170, A1).

Usage:
    python3 tools/meson/tests/test_verdict_parser.py <builddir> <arch>

Exercises qemu_test.classify_verdict (the single source of truth — no
duplicated logic) over synthetic transcripts:
- a TEST SUMMARY with failures stays FAIL (never retried, never PASS)
- missing SUMMARY / zero S-lines / 0-0 counts are FAIL
  (TIMEOUT/QEMU_EXIT), never PASS
- an expected-panic marker plus its log line is PASS
- STALL-RECOVERED attaches only to a final PASS
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from qemu_test import classify_verdict  # noqa: E402

SUMMARY_PASS = """\
 ==============================
  TEST SUMMARY
   PLANNED:         22
   EXECUTED:        22
   TIME_ELAPSED_MS: 3665
   PASSED:          22
   FAILED:          0
=============================="""

SUMMARY_FAIL = SUMMARY_PASS.replace("FAILED:          0",
                                    "FAILED:          1").replace(
    "PASSED:          22", "PASSED:          21")


def main():
    """Run the verdict-contract cases."""
    if len(sys.argv) != 3:
        print("usage: test_verdict_parser.py <builddir> <arch>")
        return 2
    cases = [
        ("clean pass", SUMMARY_PASS, "ui",
         (0, "22", "0", "3665ms", "")),
        ("failing summary stays FAIL", SUMMARY_FAIL, "ui",
         (1, "21", "1", "3665ms", "")),
        ("empty log is NO_SUMMARY",
         "RESULT: QEMU_EXIT (no TEST SUMMARY)", "ui",
         (1, "0", "0", "0ms", " STATUS: NO_SUMMARY")),
        ("timeout verdict is TIMEOUT",
         "RESULT: TIMEOUT (120s, no summary)", "ui",
         (1, "0", "0", "0ms", " STATUS: TIMEOUT")),
        ("zero S-lines, timeout", "S: x foo\nRESULT: TIMEOUT", "ui",
         (1, "0", "0", "0ms", " STATUS: TIMEOUT")),
        ("expected panic passes",
         "RESULT: PASS (expected panic: test-main)\n", "testrunner",
         (0, "0", "0", "0ms", " STATUS: EXPECTED_PANIC_PASS")),
        ("panic without summary fails",
         "KERNEL PANIC: test\n", "ui",
         (1, "0", "0", "0ms", " STATUS: PANIC")),
        ("stall recovered attaches to pass",
         SUMMARY_PASS + "STALL-RECOVERED", "ui",
         (0, "22", "0", "3665ms", " STATUS: STALL-RECOVERED")),
        ("stall recovered never overrides fail", SUMMARY_FAIL, "ui",
         (1, "21", "1", "3665ms", "")),
        ("task_fpu empty is NO_TESTS_REGISTERED",
         "No tests registered\n", "task_fpu",
         (0, "0", "0", "0ms", " STATUS: NO_TESTS_REGISTERED")),
    ]
    failures = 0
    for name, text, cls, want in cases:
        got = classify_verdict(text, cls)
        if got != want:
            print("FAIL %s: got %r want %r" % (name, got, want))
            failures += 1
    if failures:
        print("verdict-contract FAILED: %d/%d" % (failures, len(cases)))
        return 1
    print("verdict-contract PASSED: %d/%d" % (len(cases), len(cases)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
