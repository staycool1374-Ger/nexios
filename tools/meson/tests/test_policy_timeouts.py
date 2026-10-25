#!/usr/bin/env python3
"""QEMU test() timeout parity (issue #170, A1; measured+30pct rule).

Usage:
    python3 tools/meson/tests/test_policy_timeouts.py <builddir> <arch>

Asserts every QEMU test() timeout equals the policy outer_timeout
backstop (never the Meson 30 s default) by re-resolving the policy the
same way list_tests.py does and comparing against
`meson introspect --tests`. Per-attempt budgets (measured+30pct
expect_timeout) are enforced inside the runner, not here.
"""

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent.parent


def policy(arch, cls, key):
    """Resolve one policy value via awk (fail loud on empty)."""
    out = subprocess.run(
        ["awk", "-f", str(REPO / "tools" / "test-policy.awk"),
         "-v", "arch=%s" % arch, "-v", "class=%s" % cls,
         "-v", "key=%s" % key, str(REPO / "tools" / "test-policy.tsv")],
        capture_output=True, text=True, check=False,
    )
    value = out.stdout.strip()
    if out.returncode != 0 or not value:
        raise SystemExit("ERROR: no %s for class=%s ARCH=%s"
                         % (key, cls, arch))
    return value


def main():
    """Compare every QEMU test timeout against 2x the policy value."""
    if len(sys.argv) != 3:
        print("usage: test_policy_timeouts.py <builddir> <arch>")
        return 2
    builddir, arch = sys.argv[1], sys.argv[2]
    out = subprocess.run(
        ["meson", "introspect", builddir, "--tests"],
        capture_output=True, text=True, check=True,
    )
    failures = []
    checked = 0
    for test in json.loads(out.stdout):
        name = test.get("name", "")
        if not name.startswith("qemu/"):
            continue
        cls = name.split("/")[-1]
        try:
            expected = int(policy(arch, cls, "outer_timeout"))
        except ValueError:
            failures.append("%s: non-numeric outer_timeout" % cls)
            continue
        actual = test.get("timeout", 30)
        checked += 1
        if actual != expected:
            failures.append("%s: timeout=%s, want policy outer=%s"
                            % (name, actual, expected))
        if actual <= 30:
            failures.append("%s: timeout=%s looks like the Meson default"
                            % (name, actual))
    if checked == 0:
        print("no qemu/ tests registered (run meson setup first)")
        return 2
    if failures:
        print("policy-timeout parity FAILED:")
        for failure in failures:
            print("  - %s" % failure)
        return 1
    print("policy-timeout parity PASSED: %d tests" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
