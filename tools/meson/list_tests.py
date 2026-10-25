#!/usr/bin/env python3
"""List QEMU test classes with per-test timeouts (issue #170 Phase 4).

Usage:
    python3 tools/meson/list_tests.py <arch> <buildtype>

Mirrors the run_all_classes.sh selection: the x86_64 debug list, the
riscv64 reduced list, NEXIOS_TEST_CLASSES override, arch/build scoping
via tools/test-policy.awk. Prints one `class|timeout` line per class
(timeout = the policy outer_timeout backstop, never the 30 s default;
per-attempt budgets are the measured+30pct expect_timeout rows, same
table — the outer only bounds whole-test wall including rebuild+retries).
"""

import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
POLICY_TSV = REPO / "tools" / "test-policy.tsv"
POLICY_AWK = REPO / "tools" / "test-policy.awk"

X86_64_CLASSES = ("core_basic core_sync core_sched core_task core_memory "
                  "core_syscall core_tracing ipc capability proc_elf storage "
                  "servers drivers hal smp smp_multicpu deadline ui "
                  "logging_debug random bench task_tcb_log task_fpu "
                  "ahci_live iommu_live testrunner").split()
# aarch64 debug gate (issue #357): verbatim TEST_FULL_CLASSES_aarch64
# (Makefile:1105) — B1 green aggregates + testrunner. The x86_64 list is
# NOT a substitute: it omits the six aarch64-only classes below and
# carries known-reds the gate excludes (core_sched STALL, core_memory
# PANIC, capability STALL, deadline PANIC, bench fails).
AARCH64_CLASSES = ("core_basic core_sync core_task core_syscall "
                   "core_tracing ipc hal logging_debug random task_tcb_log "
                   "storage drivers ui servers task_fpu configuration_build "
                   "task_core process_lifecycle shell_interaction cap_irq "
                   "cap_irq_notify testrunner").split()
RISCV64_CLASSES = ("arch_riscv64 scheduler_zombie configuration_build "
                   "memory_vmm memory_page_tables timing_core hal_timer "
                   "irq_early_std storage drivers ui servers task_fpu "
                   "shell_interaction cap_irq cap_irq_notify task_core "
                   "process_lifecycle hal").split()


def policy(arch, cls, key):
    """Resolve one policy value via awk (fail loud on empty)."""
    out = subprocess.run(
        ["awk", "-f", str(POLICY_AWK), "-v", "arch=%s" % arch,
         "-v", "class=%s" % cls, "-v", "key=%s" % key, str(POLICY_TSV)],
        capture_output=True, text=True, check=False,
    )
    value = out.stdout.strip()
    if out.returncode != 0 or not value:
        raise SystemExit("ERROR: no %s for class=%s ARCH=%s"
                         % (key, cls, arch))
    return value


def scoped_in(arch, build, cls, key):
    """arch_scope/build_scope membership (mirrors run_all_classes.sh)."""
    scope = policy(arch, cls, key).split()
    if scope == ["any"]:
        return True
    want = arch if key == "arch_scope" else build
    return want in scope


def main():
    """Print class|timeout lines for the arch/build selection."""
    if len(sys.argv) != 3:
        print("usage: list_tests.py <arch> <buildtype>")
        return 2
    arch = sys.argv[1]
    build = {"debug": "debug", "debugoptimized": "debug",
             "release": "release", "plain": "debug",
             "minsize": "debug"}.get(sys.argv[2], sys.argv[2])
    override = os.environ.get("NEXIOS_TEST_CLASSES", "").split()
    if override:
        classes = override
    elif arch == "x86_64":
        classes = list(X86_64_CLASSES)
    elif arch == "aarch64":
        classes = list(AARCH64_CLASSES)
    elif arch == "riscv64":
        classes = list(RISCV64_CLASSES)
    else:
        classes = list(X86_64_CLASSES)
    for cls in classes:
        if not scoped_in(arch, build, cls, "arch_scope"):
            continue
        if not scoped_in(arch, build, cls, "build_scope"):
            continue
        try:
            timeout = int(policy(arch, cls, "outer_timeout"))
        except ValueError:
            print("ERROR: non-numeric outer_timeout for %s" % cls)
            return 2
        print("%s|%d" % (cls, timeout))
    return 0


if __name__ == "__main__":
    sys.exit(main())
