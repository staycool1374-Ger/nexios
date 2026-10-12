#!/usr/bin/env python3
"""Doxygen warning ratchet (issue #303, Phase 6 module 8).

Runs Doxygen with HTML output redirected to a temp dir (the committed
docs/doxygen/html tree is never touched), counts "warning:" lines on
stderr, and compares against tools/doxygen_baseline.txt.

Exit 0 when count <= baseline (green; prints the count and, when
strictly below baseline, a note to lower the file). Exit 1 when the
count grows (new undocumented APIs or doc errors landed).

The baseline is a human-lowered ratchet: fix warnings, re-run this
script, and commit the lower number. Never raise it without review.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE_FILE = os.path.join(REPO, "tools", "doxygen_baseline.txt")
RE_WARN = re.compile(r"warning:", re.IGNORECASE)


def main() -> int:
    try:
        with open(BASELINE_FILE, encoding="utf-8") as f:
            baseline = int(f.read().strip().split()[0])
    except (OSError, ValueError, IndexError) as exc:
        print(f"doxygen-check: cannot read baseline: {exc}")
        return 2

    with tempfile.TemporaryDirectory(prefix="doxy-check-") as tmp:
        with open(os.path.join(REPO, "Doxyfile"), encoding="utf-8") as f:
            config = f.read()
        config += f"\nOUTPUT_DIRECTORY = {tmp}\n"
        proc = subprocess.run(
            ["doxygen", "-"],
            input=config,
            capture_output=True,
            text=True,
            cwd=REPO,
            timeout=600,
        )
    warnings = [ln for ln in proc.stderr.splitlines() if RE_WARN.search(ln)]
    count = len(warnings)
    print(f"doxygen-check: {count} warnings (baseline {baseline})")
    if count <= baseline:
        if count < baseline:
            print("doxygen-check: improved — consider lowering "
                  "tools/doxygen_baseline.txt")
        print("doxygen-check: PASSED")
        return 0
    print("doxygen-check: FAILED — warnings grew; fix new warnings or "
          "justify a baseline change")
    for ln in warnings[:20]:
        print(f"  {ln}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
