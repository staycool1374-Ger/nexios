#!/usr/bin/env python3
"""Write the baked test-config.txt for one class (issue #170).

Usage:
    gen_test_config.py <test_class> <output_path> [<compat_copy_path> ...]

Mirrors the Makefile worker lines that `printf '%s\\n' "$(CLASS)"` into
initrd/tests/test-config.txt before boot. The builddir output is the single
source of truth; extra paths are one-direction compatibility copies
(gitignored legacy location). qemu_test.py asserts the baked config
pre-QEMU (A3); release bakes `none` unless overridden.

Called at Meson configure time (custom_target command in Phase 4); usable
standalone for verification now.
"""

import sys
from pathlib import Path


def main():
    """Write the class name to the output and every compat copy."""
    if len(sys.argv) < 3:
        print("usage: gen_test_config.py <test_class> <output> [compat...]")
        return 2
    test_class, output, compat_paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    if not test_class or "\n" in test_class:
        print("refusing empty/multiline test class")
        return 2
    payload = test_class + "\n"
    Path(output).write_text(payload, encoding="utf-8")
    for compat in compat_paths:
        Path(compat).write_text(payload, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
