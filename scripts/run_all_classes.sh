#!/usr/bin/env bash
# Full test-base runner (issue #173, replaces the removed `all` class).
# Runs the 22 structural/semantic aggregates plus the 4 standalone
# specials, one reboot per class, and records PASS/FAIL/abnormal
# termination per class into <repo>/test-history.txt using the mandatory
# AGENTS.md row format:
#   <YYYY-MM-DD HH:MM:SS> <test-class> PASSED: <n> FAILED: <n> TIME: <consumed-time>
#   (abnormal termination appends STATUS: <TIMEOUT|PANIC|NO_SUMMARY>)
# Usage: bash scripts/run_all_classes.sh [arch] [build]
#        (the full-suite entry point; per-class runs go through qemu_test.py)
# Per-class run policy (timeouts, retries, scopes, markers) comes from
# tools/test-policy.tsv via tools/test-policy.awk — no literals here.
# No debugging — only observe what works and what does not.
set -u
cd "$(dirname "$0")/.."

ARCH="${1:-x86_64}"
BUILD="${2:-debug}"

# Run policy (issue #407 item 3): per-class timeouts, retries, scopes and
# verdict markers live in tools/test-policy.tsv, resolved here ONLY through
# tools/test-policy.awk. No policy literals below.
POLICY_FILE=tools/test-policy.tsv
POLICY_AWK=tools/test-policy.awk
policy() { # <class> <key> — print resolved value, fail loud on empty
    local out
    out=$(awk -f "$POLICY_AWK" -v arch="$ARCH" -v class="$1" -v key="$2" "$POLICY_FILE") || { echo "ERROR: test-policy: no $2 for class=$1 ARCH=$ARCH" >&2; exit 1; }
    if [ -z "$out" ]; then echo "ERROR: test-policy: no $2 for class=$1 ARCH=$ARCH" >&2; exit 1; fi
    printf '%s' "$out"
}

# 22 aggregates + 4 standalone specials.  testrunner runs last: its
# expected-panic test halts the kernel, so it gets its own boot at the
# end and deterministic log ordering.
CLASSES="core_basic core_sync core_sched core_task core_memory core_syscall core_tracing ipc capability proc_elf storage servers drivers hal smp smp_multicpu deadline ui logging_debug random bench task_tcb_log task_fpu ahci_live iommu_live testrunner"

# Issue #357: the per-arch gate lists live in tools/meson/list_tests.py
# (measured rows only) and pass through NEXIOS_TEST_CLASSES; the script
# default below is the x86_64 control list. Arch/build scoping is
# table-driven (tools/test-policy.tsv) as defense.
if [ -n "${NEXIOS_TEST_CLASSES:-}" ]; then
    CLASSES="$NEXIOS_TEST_CLASSES"
fi

# Issue #207: riscv64 runs its own green-evidenced class set (arch_riscv64
# 24/24 incl. full daemon boot, scheduler_zombie 5/5).  The generic
# aggregates are not gated on riscv64 (no evidence yet — wiring them in
# blind would red the gate for unrelated pre-existing gaps).
# Issue #357: skipped when list_tests.py passes its measured list.
if [ "$ARCH" = "riscv64" ] && [ -z "${NEXIOS_TEST_CLASSES:-}" ]; then
    CLASSES="arch_riscv64 scheduler_zombie"
fi

RUN=""
for c in $CLASSES; do
    skip=0
    ascope=$(policy "$c" arch_scope)
    if [ "$ascope" != "any" ]; then
        hit=0
        for x in $ascope; do
            if [ "$ARCH" = "$x" ]; then hit=1; break; fi
        done
        if [ "$hit" -eq 0 ]; then
            printf 'SKIP %s (arch_scope=%s, ARCH=%s)\n' "$c" "$ascope" "$ARCH"
            skip=1
        fi
    fi
    if [ "$skip" -eq 0 ]; then
        bscope=$(policy "$c" build_scope)
        if [ "$bscope" != "any" ]; then
            hit=0
            for x in $bscope; do
                if [ "$BUILD" = "$x" ]; then hit=1; break; fi
            done
            if [ "$hit" -eq 0 ]; then
                printf 'SKIP %s (build_scope=%s, BUILD=%s)\n' "$c" "$bscope" "$BUILD"
                skip=1
            fi
        fi
    fi
    if [ "$skip" -eq 0 ]; then RUN="$RUN $c"; fi
done

# Inspection mode: dump resolved policy per class, no QEMU, no history write.
if [ -n "${NEXIOS_POLICY_DUMP:-}" ]; then
    for c in $RUN; do
        printf '%s|%s|%s|%s|%s|%s|%s\n' "$c" "$(policy "$c" outer_timeout)" "$(policy "$c" max_attempts)" "$(policy "$c" arch_scope)" "$(policy "$c" build_scope)" "$(policy "$c" expected_panic)" "$(policy "$c" expected_empty)"
    done
    exit 0
fi

# Fail fast when the q35 variants cannot boot: clear message instead of a
# QEMU boot error deep in the loop.  Mirrors the OVMF_CODE guards
# (which remain the authoritative per-class check inside qemu_test.py).
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

LOGDIR=/tmp/jarvis_classruns-$(basename "$PWD" | sed 's/[^A-Za-z0-9_]/_/g')
mkdir -p "$LOGDIR"

total=0; passed_classes=0; failed_classes=0
for c in $RUN; do
    total=$((total+1))
    # Scope cleanup to this checkout's QEMU (issue #372): the -name tag is
    # added by qemu_test.py, with a trailing-delimiter guard so tag `jarvis`
    # never matches `jarvis_test` artifacts. Never blanket-kill qemu-system:
    # a sibling checkout may be testing concurrently.
    pkill -9 -f "qemu-system.*-name jarvis-$(basename "$PWD" | sed 's/[^A-Za-z0-9_]/_/g')([^A-Za-z0-9_]|$)" 2>/dev/null || true
    log="$LOGDIR/$c.log"
    start=$(date +%s%N)
    outer=$(policy "$c" outer_timeout)
    # Meson runner (issue #170): qemu_test.py contract
    # (same verdict tokens, same history format);
    # --no-history because THIS script owns the history append below.
    timeout "$outer" python3 tools/meson/qemu_test.py --arch "$ARCH" --build "$BUILD" --class "$c" --no-history > "$log" 2>&1
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
    # panic: ...)" — that is a PASS, not a PANIC.  Detect it first, gated
    # on the table marker.
    expected_panic_pass=0
    if [ "$(policy "$c" expected_panic)" = "1" ] && grep -qE 'RESULT: PASS \(expected panic|PRESULT: PASS \(expected panic' "$log"; then
        expected_panic_pass=1
    fi
    # Env-gated empty class (task_fpu: legacy test_fpu*.cpp filtered out of
    # the x86_64 build, expected count 0): a clean boot that registers
    # nothing is the correct outcome, not a failure. Gated on the table
    # marker (the task_fpu name check stays as defense).
    expected_empty_pass=0
    if [ "$(policy "$c" expected_empty)" = "1" ] && [ "$c" = "task_fpu" ] && grep -q "No tests registered" "$log" \
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
        elif grep -q 'STALL-UNRECOVERED' "$log"; then
            # Issue #322: the harness retried the identical binary once and it
            # still produced no summary (zero-S-line TIMEOUT / QEMU_EXIT) — a
            # host/TCG stall, not a code verdict.
            status="STATUS: STALL"
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
    # Issue #322: a run that stalled once and went green on the identical-binary
    # retry is a PASS, but the stall is recorded so the flake rate is visible.
    if [ -z "$status" ] && [ "${nfail:-0}" -eq 0 ] && grep -q 'STALL-RECOVERED' "$log"; then
        row="$row STATUS: STALL-RECOVERED"
    fi

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

printf '\nSUMMARY: total=%d ok=%d fail=%d ARCH=%s\n' "$total" "$passed_classes" "$failed_classes" "$ARCH" >> test-history.txt
printf '\n=== SUMMARY total=%d ok=%d fail=%d ARCH=%s ===\n' "$total" "$passed_classes" "$failed_classes" "$ARCH"
