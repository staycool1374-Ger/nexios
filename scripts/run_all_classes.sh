#!/usr/bin/env bash
# Full test-base runner (issue #173, replaces the removed `all` class).
# Runs the 16 structural/semantic aggregates plus the 4 standalone
# specials, one reboot per class, and records PASS/FAIL/abnormal
# termination per class into <repo>/test-history.txt using the mandatory
# AGENTS.md row format:
#   <YYYY-MM-DD HH:MM:SS> <test-class> PASSED: <n> FAILED: <n> TIME: <consumed-time>
#   (abnormal termination appends STATUS: <TIMEOUT|PANIC|NO_SUMMARY>)
# Usage: bash scripts/run_all_classes.sh [arch] [build]
#        (normally via `make test-full [arch] [build]`)
# No debugging — only observe what works and what does not.
set -u
cd "$(dirname "$0")/.."

ARCH="${1:-x86_64}"
BUILD="${2:-debug}"

# 16 aggregates + 4 standalone specials.  testrunner runs last: its
# expected-panic test halts the kernel, so it gets its own boot at the
# end and deterministic log ordering.
CLASSES="core ipc capability proc_elf storage servers drivers hal smp smp_multicpu deadline ui logging_debug random bench task_tcb_log task_fpu ahci_live iommu_live testrunner"

# Issue #207: riscv64 runs its own green-evidenced class set (arch_riscv64
# 24/24 incl. full daemon boot, scheduler_zombie 5/5).  The generic
# aggregates are not gated on riscv64 (no evidence yet — wiring them in
# blind would red the gate for unrelated pre-existing gaps).
if [ "$ARCH" = "riscv64" ]; then
    CLASSES="arch_riscv64 scheduler_zombie"
fi

# Per-class outer timeout covers the build (a cold arch switch rebuilds
# everything) plus the QEMU run.  riscv64 needs headroom: TCG wall
# dilation puts arch_riscv64 near 270 s (measured) on top of the build.
OUTER_TIMEOUT=300
if [ "$ARCH" = "riscv64" ]; then
    OUTER_TIMEOUT=900
fi

# x86_64-only classes: no registry entry (smp, smp_multicpu) or no QEMU
# machine (ahci_live, iommu_live) on other architectures.
X86_ONLY="smp smp_multicpu ahci_live iommu_live"

# Debug-only classes: the q35 hardware-variant boots (ahci_live, iommu_live)
# hardcode the debug ISO in the Makefile and register zero TF_RELEASE
# tests, so a release run would execute nothing even if it booted.
DEBUG_ONLY="ahci_live iommu_live"

RUN=""
for c in $CLASSES; do
    skip=0
    for x in $X86_ONLY; do
        if [ "$c" = "$x" ] && [ "$ARCH" != "x86_64" ]; then
            printf 'SKIP %s (x86_64-only, ARCH=%s)\n' "$c" "$ARCH"
            skip=1
            break
        fi
    done
    if [ "$skip" -eq 0 ]; then
        for x in $DEBUG_ONLY; do
            if [ "$c" = "$x" ] && [ "$BUILD" != "debug" ]; then
                printf 'SKIP %s (debug-only QEMU variant, BUILD=%s)\n' "$c" "$BUILD"
                skip=1
                break
            fi
        done
    fi
    if [ "$skip" -eq 0 ]; then RUN="$RUN $c"; fi
done

# Fail fast when the q35 variants cannot boot: clear message instead of a
# QEMU boot error deep in the loop.  Mirrors the Makefile OVMF_CODE guards
# (which remain the authoritative per-class check inside execute-test).
for c in $RUN; do
    if [ "$c" = "ahci_live" ] || [ "$c" = "iommu_live" ]; then
        OVMF_FW=$(find /opt/homebrew/Cellar/qemu /usr/share/qemu /usr/local/share/qemu -name 'edk2-x86_64-code.fd' 2>/dev/null | head -1)
        if [ -z "$OVMF_FW" ]; then
            echo "ERROR: $c requires OVMF edk2-x86_64-code.fd, none found under /opt/homebrew/Cellar/qemu, /usr/share/qemu, /usr/local/share/qemu"
            exit 1
        fi
        break
    fi
done

LOGDIR=/tmp/jarvis_classruns
mkdir -p "$LOGDIR"

total=0; passed_classes=0; failed_classes=0
for c in $RUN; do
    total=$((total+1))
    pkill -9 -f qemu-system 2>/dev/null
    log="$LOGDIR/$c.log"
    start=$(date +%s%N)
    timeout "$OUTER_TIMEOUT" make execute-test "$ARCH" "$BUILD" "$c" > "$log" 2>&1
    rc=$?
    end=$(date +%s%N)
    wall_ms=$(( (end - start) / 1000000 ))

    # Harness summary (authoritative when present)
    npass=$(grep -E '^\s*PASSED:' "$log" | tail -1 | awk '{print $2}' | tr -d '[:space:]')
    nfail=$(grep -E '^\s*FAILED:' "$log" | tail -1 | awk '{print $2}' | tr -d '[:space:]')
    nms=$(grep -E '^\s*TIME_ELAPSED_MS:' "$log" | tail -1 | awk '{print $2}' | tr -d '[:space:]')

    ts="$(date '+%Y-%m-%d %H:%M:%S')"
    status=""
    # Expected-panic classes (testrunner) end with "RESULT: PASS (expected
    # panic: ...)" — that is a PASS, not a PANIC.  Detect it first.
    expected_panic_pass=0
    if grep -qE 'RESULT: PASS \(expected panic|PRESULT: PASS \(expected panic' "$log"; then
        expected_panic_pass=1
    fi
    # Env-gated empty class (task_fpu: legacy test_fpu*.cpp filtered out of
    # the x86_64 build, expected count 0): a clean boot that registers
    # nothing is the correct outcome, not a failure.
    expected_empty_pass=0
    if [ "$c" = "task_fpu" ] && grep -q "No tests registered" "$log" \
        && ! grep -qiE 'kernel panic|page fault|triple fault|#PF|PANIC:' "$log"; then
        expected_empty_pass=1
    fi
    if [ -z "${npass:-}" ] || [ -z "${nfail:-}" ]; then
        # No PASS/FAIL summary → abnormal termination
        npass=0; nfail=0; nms="${wall_ms}ms"
        if [ "$expected_panic_pass" -eq 1 ]; then
            status="STATUS: EXPECTED_PANIC_PASS"
        elif [ "$expected_empty_pass" -eq 1 ]; then
            status="STATUS: NO_TESTS_REGISTERED"
        elif grep -qiE 'kernel panic|page fault|triple fault|#PF|PANIC:' "$log"; then
            status="STATUS: PANIC"
        elif grep -qiE 'RESULT: TIMEOUT|Terminated: 15|\[STALL\]|watchdog' "$log"; then
            status="STATUS: TIMEOUT"
        else
            status="STATUS: NO_SUMMARY"
        fi
    elif [ -n "${nms:-}" ]; then
        nms="${nms}ms"
    else
        nms="${wall_ms}ms"
    fi

    row="$ts $c PASSED: $npass FAILED: $nfail TIME: $nms"
    if [ -n "$status" ]; then row="$row $status"; fi

    # Verdict: OK iff summary present or expected-panic/empty PASS, 0 fails, rc==0
    if [ -n "$status" ] && [ "$expected_panic_pass" -eq 0 ] && [ "$expected_empty_pass" -eq 0 ]; then
        verdict="FAIL"
        failed_classes=$((failed_classes+1))
    elif [ "$nfail" -eq 0 ] && [ "$rc" -eq 0 ]; then
        verdict="OK"
        passed_classes=$((passed_classes+1))
    elif [ "$expected_empty_pass" -eq 1 ]; then
        # Empty-class boot exits nonzero (no summary to report); the
        # STATUS above already records the benign outcome.
        verdict="OK"
        passed_classes=$((passed_classes+1))
    else
        verdict="FAIL"
        failed_classes=$((failed_classes+1))
    fi

    printf '%s\n' "$row" >> test-history.txt
    printf '[%s] %s\n' "$verdict" "$row"
done

printf '\nSUMMARY: total=%d ok=%d fail=%d\n' "$total" "$passed_classes" "$failed_classes" >> test-history.txt
printf '\n=== SUMMARY total=%d ok=%d fail=%d ===\n' "$total" "$passed_classes" "$failed_classes"
