#!/usr/bin/env bash
# Equivalence oracle for tools/test-policy.tsv + tools/test-policy.awk
# (issue #407 item 3). Host-only, no QEMU. Exits nonzero on first mismatch.
# Expected values are hardcoded ON PURPOSE — this file is the independent
# oracle, not a second source of truth.
set -u
cd "$(dirname "$0")/.."

POLICY_FILE=tools/test-policy.tsv
POLICY_AWK=tools/test-policy.awk
fail=0

check() { # <desc> <expected> <actual>
    if [ "$2" = "$3" ]; then
        printf 'ok %s = %s\n' "$1" "$3"
    else
        printf 'FAIL %s: want %s, have %s\n' "$1" "$2" "$3" >&2
        fail=1
    fi
}

lookup() { # <arch> <class> <key>
    awk -f "$POLICY_AWK" -v arch="$1" -v class="$2" -v key="$3" "$POLICY_FILE"
}

# 1. Table validates.
awk -f "$POLICY_AWK" -v validate=1 "$POLICY_FILE" || { echo "FAIL validate" >&2; fail=1; }

# 2. Expect timeouts: default 120 / heavy 250 / riscv64 400 (arch>group).
check "x86_64/task_tcb_log" 120 "$(lookup x86_64 task_tcb_log expect_timeout)"
check "x86_64/testrunner" 120 "$(lookup x86_64 testrunner expect_timeout)"
check "x86_64/core_basic" 250 "$(lookup x86_64 core_basic expect_timeout)"
check "x86_64/deadline" 250 "$(lookup x86_64 deadline expect_timeout)"
check "x86_64/ahci_live" 250 "$(lookup x86_64 ahci_live expect_timeout)"
check "aarch64/core_basic" 250 "$(lookup aarch64 core_basic expect_timeout)"
check "aarch64/ui" 120 "$(lookup aarch64 ui expect_timeout)"
check "riscv64/arch_riscv64" 400 "$(lookup riscv64 arch_riscv64 expect_timeout)"
check "riscv64/scheduler_zombie" 400 "$(lookup riscv64 scheduler_zombie expect_timeout)"
check "riscv64/core_basic" 400 "$(lookup riscv64 core_basic expect_timeout)"
check "x86_64/unknown-class" 120 "$(lookup x86_64 no_such_class expect_timeout)"

# 3. Outer timeouts: 600 default, 1200 riscv64, no per-class override.
check "outer/x86_64/deadline" 600 "$(lookup x86_64 deadline outer_timeout)"
check "outer/aarch64/core_basic" 600 "$(lookup aarch64 core_basic outer_timeout)"
check "outer/riscv64/arch_riscv64" 1200 "$(lookup riscv64 arch_riscv64 outer_timeout)"
check "outer/riscv64/scheduler_zombie" 1200 "$(lookup riscv64 scheduler_zombie outer_timeout)"

# 4. Attempts + watchdog uniform.
for spec in "x86_64 core_basic" "aarch64 ui" "riscv64 scheduler_zombie"; do
    set -- $spec
    check "$1/$2/max_attempts" 2 "$(lookup "$1" "$2" max_attempts)"
    check "$1/$2/watchdog_stall" 220 "$(lookup "$1" "$2" watchdog_stall)"
done

# 5. Markers: testrunner panics, task_fpu empty, everything else neither.
check "testrunner/expected_panic" 1 "$(lookup x86_64 testrunner expected_panic)"
check "testrunner/expected_empty" 0 "$(lookup x86_64 testrunner expected_empty)"
check "task_fpu/expected_panic" 0 "$(lookup x86_64 task_fpu expected_panic)"
check "task_fpu/expected_empty" 1 "$(lookup x86_64 task_fpu expected_empty)"
check "core_basic/expected_panic" 0 "$(lookup x86_64 core_basic expected_panic)"
check "core_basic/expected_empty" 0 "$(lookup x86_64 core_basic expected_empty)"

# 6. Scopes: x86-only + debug-only classes, everything else any/any.
for c in smp smp_multicpu ahci_live iommu_live; do
    check "$c/arch_scope" "x86_64" "$(lookup aarch64 "$c" arch_scope)"
done
for c in ahci_live iommu_live; do
    check "$c/build_scope" "debug" "$(lookup x86_64 "$c" build_scope)"
done
check "core_basic/arch_scope" "any" "$(lookup riscv64 core_basic arch_scope)"
check "core_basic/build_scope" "any" "$(lookup x86_64 core_basic build_scope)"

# 7. Stall signatures default.
check "stall_signatures" "TIMEOUT,QEMU_EXIT" "$(lookup x86_64 deadline stall_signatures)"

# 8. Fail-loud contract: unknown key exits 3.
if lookup x86_64 core_basic bogus >/dev/null 2>&1; then
    echo "FAIL bogus key did not fail" >&2; fail=1
else
    rc=$?
    check "bogus-key-exit" 3 "$rc"
fi

# 9. Skip-set oracle via the script dump mode (no QEMU, no history write).
dump() { # <arch> <build> <classes...>
    local arch=$1 build=$2; shift 2
    NEXIOS_TEST_CLASSES="$*" NEXIOS_POLICY_DUMP=1 \
        bash scripts/run_all_classes.sh "$arch" "$build"
}
out=$(dump aarch64 release "smp ahci_live iommu_live core_basic")
echo "$out" | grep -q "SKIP smp" || { echo "FAIL dump: smp kept on aarch64" >&2; fail=1; }
echo "$out" | grep -q "SKIP ahci_live" || { echo "FAIL dump: ahci_live kept on aarch64/release" >&2; fail=1; }
echo "$out" | grep -q "SKIP iommu_live" || { echo "FAIL dump: iommu_live kept on aarch64/release" >&2; fail=1; }
echo "$out" | grep -q "core_basic|600|" || { echo "FAIL dump: core_basic dropped on aarch64/release" >&2; fail=1; }
out=$(dump x86_64 debug "smp ahci_live iommu_live core_basic")
for c in smp ahci_live iommu_live core_basic; do
    echo "$out" | grep -q "$c|" || { echo "FAIL dump: $c dropped on x86_64/debug" >&2; fail=1; }
done
out=$(dump x86_64 release "smp ahci_live iommu_live core_basic")
echo "$out" | grep -q "SKIP ahci_live" || { echo "FAIL dump: ahci_live kept on release" >&2; fail=1; }
echo "$out" | grep -q "SKIP iommu_live" || { echo "FAIL dump: iommu_live kept on release" >&2; fail=1; }
echo "$out" | grep -q "smp|" || { echo "FAIL dump: smp dropped on x86_64/release" >&2; fail=1; }

if [ "$fail" -eq 0 ]; then echo "test-policy-test: ALL OK"; else echo "test-policy-test: FAILURES" >&2; fi
exit "$fail"
