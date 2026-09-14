#!/usr/bin/env python3
"""Host unit tests for tools/gcov_line_report.py (issue #147).

Regression cover for follow-up B: gcov-tool merge skips nested .gcda
files, so merge_dirs() must fold per leaf directory.  Uses the real
captured class trees under build/coverage when present, else skips.
Run: python3 tools/test_gcov_line_report.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gcov_line_report as glr

COVERAGE_DIR = os.path.join(os.getcwd(), "build", "coverage")


def counter_values(path):
    """All COUNTERS u64 values in a .gcda file, in record order."""
    with open(path, "rb") as handle:
        data = handle.read()
    values = []
    off = 16  # magic, version, stamp, checksum
    while off + 8 <= len(data):
        tag, length = struct.unpack("<II", data[off:off + 8])
        payload = data[off + 8:off + 8 + length]
        if tag == 0x01A10000 and len(payload) == length:
            values.extend(struct.unpack("<%dQ" % (length // 8), payload))
        off += 8 + length
    return values


def needs_capture(test):
    """Skip unless both single-class captures exist."""
    for name in ("basic_lib", "kernel_top"):
        gcda = os.path.join(COVERAGE_DIR, name, "gcda")
        if not os.path.isdir(gcda):
            test.skipTest("missing capture: %s" % gcda)
    return test


class MergeDirsTest(unittest.TestCase):
    @needs_capture
    def test_two_class_merge_nonempty(self):
        """Two real class trees merge to the full file set (no fallback)."""
        tmp = tempfile.mkdtemp()
        try:
            out = os.path.join(tmp, "merged")
            glr.merge_dirs(
                [os.path.join(COVERAGE_DIR, "basic_lib", "gcda"),
                 os.path.join(COVERAGE_DIR, "kernel_top", "gcda")],
                out)
            merged = [os.path.join(r, f)
                      for r, _d, fs in os.walk(out) for f in fs
                      if f.endswith(".gcda")]
            first = [os.path.join(r, f)
                     for r, _d, fs in
                     os.walk(os.path.join(COVERAGE_DIR, "basic_lib", "gcda"))
                     for f in fs if f.endswith(".gcda")]
            self.assertGreater(len(merged), 0)
            self.assertEqual(len(merged), len(first))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    @needs_capture
    def test_merge_sums_counters(self):
        """Merged counters equal the per-class sum (not first-wins)."""
        first = os.path.join(COVERAGE_DIR, "basic_lib", "gcda",
                             "build", "lib", "logger.gcda")
        second = os.path.join(COVERAGE_DIR, "kernel_top", "gcda",
                              "build", "lib", "logger.gcda")
        tmp = tempfile.mkdtemp()
        try:
            first_dir = os.path.join(tmp, "a")
            second_dir = os.path.join(tmp, "b")
            os.makedirs(first_dir)
            os.makedirs(second_dir)
            shutil.copy(first, first_dir)
            shutil.copy(second, second_dir)
            out = os.path.join(tmp, "merged")
            glr.merge_dirs([first_dir, second_dir], out)
            merged_values = counter_values(
                os.path.join(out, "logger.gcda"))
            first_values = counter_values(os.path.join(first_dir,
                                                        "logger.gcda"))
            second_values = counter_values(os.path.join(second_dir,
                                                         "logger.gcda"))
            self.assertEqual(len(merged_values), len(first_values))
            self.assertTrue(any(value > 0 for value in merged_values),
                            "expected nonzero merged counters")
            for merged_value, first_value, second_value in zip(
                    merged_values, first_values, second_values):
                self.assertEqual(merged_value, first_value + second_value)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_merge_union_single_side_leaf(self):
        """A leaf present on one side only is copied through (union)."""
        tmp = tempfile.mkdtemp()
        try:
            first = os.path.join(tmp, "a", "sub")
            os.makedirs(first)
            with open(os.path.join(first, "only.gcda"), "wb") as handle:
                handle.write(b"sentinel")
            os.makedirs(os.path.join(tmp, "b"))
            out = os.path.join(tmp, "merged")
            glr.merge_dirs([os.path.join(tmp, "a"), os.path.join(tmp, "b")],
                           out)
            self.assertTrue(os.path.isfile(
                os.path.join(out, "sub", "only.gcda")))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_leaf_helper_finds_only_gcda_dirs(self):
        """_leaf_gcda_dirs maps relative dirs holding .gcda files."""
        tmp = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(tmp, "x", "y"))
            open(os.path.join(tmp, "x", "y", "one.gcda"), "wb").close()
            open(os.path.join(tmp, "x", "note.txt"), "wb").close()
            leaves = glr._leaf_gcda_dirs(tmp)
            self.assertEqual(leaves, {os.path.join("x", "y"): ["one.gcda"]})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class ParseCaptureTest(unittest.TestCase):
    def test_rejects_abort(self):
        """ABORT sentinel invalidates the whole capture."""
        tmp = tempfile.mkdtemp()
        try:
            path = os.path.join(tmp, "serial.raw")
            with open(path, "wb") as handle:
                handle.write(b"@@GCDABEGIN@@\n@@GCDAABORT@@\n@@GCDAEND@@\n")
            self.assertIsNone(glr.parse_capture(path))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_rejects_truncated_payload(self):
        """Declared length beyond available bytes invalidates the capture."""
        tmp = tempfile.mkdtemp()
        try:
            path = os.path.join(tmp, "serial.raw")
            with open(path, "wb") as handle:
                handle.write(b"@@GCDABEGIN@@\nGCOV:100:x.gcda\nshort\n"
                             b"@@GCDAEND@@\n")
            self.assertIsNone(glr.parse_capture(path))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class BranchParseTest(unittest.TestCase):
    GCOV_SAMPLE = (
        "        -:    0:Source:sample.cpp\n"
        "        5:    1:int f(int x) {\n"
        "        5:    2:  if (x) {\n"
        "branch  0 taken 80%\n"
        "branch  1 taken 20%\n"
        "        1:    3:    return 1;\n"
        "    #####:    4:  }\n"
        "branch  2 never executed\n"
        "        -:    5:}\n"
    )

    def test_branch_regex_counts(self):
        """BRDA lines: 3 total, 2 taken (never-executed excluded)."""
        taken = total = 0
        for line in self.GCOV_SAMPLE.splitlines():
            match = glr.GCOV_BRANCH_RE.match(line)
            if not match:
                continue
            total += 1
            if "never executed" not in match.group("taken"):
                taken += 1
        self.assertEqual(total, 3)
        self.assertEqual(taken, 2)


if __name__ == "__main__":
    subprocess.run([sys.executable, "-c", "pass"], check=True)
    unittest.main(verbosity=2)
