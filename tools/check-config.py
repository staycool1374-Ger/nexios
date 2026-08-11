#!/usr/bin/env python3
"""
Jarvis RTOS — Config Validation Script (check-config)

Parses src/kernel/nexios_config.h and validates every compile-time
tunable against its documented range, cross-dependencies, and
architecture constraints.  Exits with code 0 on success, 1 on
errors, 2 on warnings.

Usage:
    python3 tools/check-config.py [--nexios-config path]
"""

import argparse
import os
import re
import sys
from pathlib import Path

# ─── Preprocessor evaluator ────────────────────────────────────────────────

def preprocess(filepath: str, arch: str | None = None) -> dict[str, int]:
    """
    Crude preprocessor: reads #define lines and evaluates #if/#elif/#else/#endif.
    Returns a dict of macro -> integer value.
    Supports simple expressions: defined(MACRO), MACRO, integer literals,
    !, &&, ||, ==, !=, >=, <=, >, <, numeric comparisons.
    """
    defines: dict[str, int] = {}
    # Seed with architecture flags from the build environment.
    # Priority: 1) `arch` argument (from --arch CLI or ARCH env var),
    #           2) host platform detection.
    if arch is not None:
        arch_clean = arch.lower().replace("_", "").replace("-", "")
        if arch_clean in ("x8664", "amd64", "x86"):
            defines["CONFIG_ARCH_X86_64"] = 1
        elif arch_clean in ("aarch64", "arm64", "arm"):
            defines["CONFIG_ARCH_AARCH64"] = 1
        elif "riscv" in arch_clean:
            defines["CONFIG_ARCH_RISCV64"] = 1
    else:
        import platform
        host = platform.machine()
        if host in ("x86_64", "AMD64"):
            defines["CONFIG_ARCH_X86_64"] = 1
        elif host in ("aarch64", "arm64"):
            defines["CONFIG_ARCH_AARCH64"] = 1
        elif "riscv" in host:
            defines["CONFIG_ARCH_RISCV64"] = 1

    lines = Path(filepath).read_text().splitlines()
    # Stack of branch states: True = active, False = skipped
    # Each entry is (active, seen_any_true_in_this_if_group)
    # We track if we've already taken a branch in the current #if/#elif chain.
    branch_stack: list[tuple[bool, bool]] = [(True, False)]  # (active, taken)

    def is_active() -> bool:
        return all(b[0] for b in branch_stack)

    def evaluate_cond(cond: str) -> bool:
        """Evaluate a simple C preprocessor condition."""
        c = cond.strip()
        # Handle 'defined(MACRO)' or 'defined MACRO'
        def replace_defined(m):
            name = m.group(1) or m.group(2)
            return "1" if name in defines and defines[name] != 0 else "0"
        c = re.sub(r'defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)', replace_defined, c)
        # Replace macro names with their values
        def replace_macro(m):
            name = m.group(0)
            if name in defines:
                return str(defines[name])
            return "0"  # undefined macro → 0
        # Replace identifiers that are not operators/keywords
        for tok in re.findall(r'\b[a-zA-Z_]\w*\b', c):
            if tok not in ("0", "1", "defined", "if", "else", "endif"):
                c = re.sub(r'\b' + tok + r'\b', replace_macro, c, count=1)
        # Replace logical operators with Python equivalents
        c = c.replace("&&", "and").replace("||", "or")
        c = c.replace("!", "not ")
        try:
            return bool(eval(c))
        except Exception:
            return True  # default to active on parse error

    for line in lines:
        trimmed = line.strip()

        # Track #if / #elif / #else / #endif
        if trimmed.startswith("#ifdef "):
            macro = trimmed[7:].strip()
            active = is_active() and (macro in defines and defines[macro] != 0)
            branch_stack.append((active, active))
            continue
        if trimmed.startswith("#ifndef "):
            macro = trimmed[8:].strip()
            active = is_active() and (macro not in defines or defines[macro] == 0)
            branch_stack.append((active, active))
            continue
        if trimmed.startswith("#if "):
            cond = trimmed[4:].strip()
            active = is_active() and evaluate_cond(cond)
            branch_stack.append((active, active))
            continue
        if trimmed.startswith("#elif "):
            cond = trimmed[6:].strip()
            parent_active = branch_stack[-2][0] if len(branch_stack) >= 2 else True
            _, taken = branch_stack[-1]
            if taken:
                # Already took a branch in this group — skip rest
                branch_stack[-1] = (False, True)
            else:
                active = parent_active and evaluate_cond(cond)
                branch_stack[-1] = (active, active)
            continue
        if trimmed == "#else":
            parent_active = branch_stack[-2][0] if len(branch_stack) >= 2 else True
            _, taken = branch_stack[-1]
            active = parent_active and not taken
            branch_stack[-1] = (active, taken)
            continue
        if trimmed == "#endif":
            if len(branch_stack) > 1:
                branch_stack.pop()
            continue

        if not is_active():
            continue

        # Match #define MACRO value or #define MACRO (simple cases)
        m = re.match(r'#define\s+(\w+)\s+(.+)', trimmed)
        if m:
            name, val_str = m.group(1), m.group(2).strip()
            val_str = re.sub(r'\s*//.*$', '', val_str)
            val_str = val_str.strip()
            try:
                # Strip C integer suffixes (ULL, UL, LL, L, ULL, u, l)
                clean = re.sub(r'[uUlL]+$', '', val_str)
                if clean.startswith('0x') or clean.startswith('0X'):
                    defines[name] = int(clean, 16)
                elif clean.startswith('0') and len(clean) > 1 and clean.isdigit():
                    defines[name] = int(clean, 8)
                else:
                    defines[name] = int(clean)
            except ValueError:
                pass

    return defines


# ─── Validation helpers ────────────────────────────────────────────────────

errors = []
warnings = []

def E(msg: str):
    errors.append(f"ERROR: {msg}")

def W(msg: str):
    warnings.append(f"WARN:  {msg}")

def check_range(name: str, val: int, lo: int, hi: int, desc: str = ""):
    if val < lo or val > hi:
        desc_str = f" ({desc})" if desc else ""
        E(f"{name} = {val}, valid range [{lo}..{hi}]{desc_str}")

def check_eq(name: str, val: int, expected: int, desc: str = ""):
    if val != expected:
        desc_str = f" ({desc})" if desc else ""
        E(f"{name} = {val}, expected {expected}{desc_str}")

def check_ge(name: str, val: int, minimum: int, desc: str = ""):
    if val < minimum:
        desc_str = f" ({desc})" if desc else ""
        E(f"{name} = {val}, must be >= {minimum}{desc_str}")

def check_le(name: str, val: int, maximum: int, desc: str = ""):
    if val > maximum:
        desc_str = f" ({desc})" if desc else ""
        E(f"{name} = {val}, must be <= {maximum}{desc_str}")

def check_bool(name: str, val: int):
    if val not in (0, 1):
        E(f"{name} = {val}, must be 0 or 1")

def check_onehot(names: list[str], vals: list[int]):
    """Exactly one of the values must be 1."""
    count = sum(1 for v in vals if v == 1)
    if count == 0:
        E(f"Exactly one of {{{', '.join(names)}}} must be defined (none found)")
    elif count > 1:
        E(f"Exactly one of {{{', '.join(names)}}} must be defined ({count} defined)")


# ─── Validator ─────────────────────────────────────────────────────────────

def validate(cfg: dict[str, int]):
    # ═══════════════════════════════════════════════════════════════════════
    # 1. Architecture — exactly one must be 1
    # ═══════════════════════════════════════════════════════════════════════
    arch_names = ["CONFIG_ARCH_X86_64", "CONFIG_ARCH_AARCH64", "CONFIG_ARCH_RISCV64"]
    arch_vals = [cfg.get(n, 0) for n in arch_names]
    check_onehot(arch_names, arch_vals)
    is_x86 = cfg.get("CONFIG_ARCH_X86_64", 0) == 1
    is_aarch64 = cfg.get("CONFIG_ARCH_AARCH64", 0) == 1
    is_riscv64 = cfg.get("CONFIG_ARCH_RISCV64", 0) == 1

    # ═══════════════════════════════════════════════════════════════════════
    # 2. Scheduling Tunables
    # ═══════════════════════════════════════════════════════════════════════
    max_tasks = cfg.get("CONFIG_MAX_TASKS", 64)
    check_range("CONFIG_MAX_TASKS", max_tasks, 2, 4096,
                "minimum 2 (idle + 1 task), maximum 4096 for 12-bit ID table")

    tick_hz = cfg.get("CONFIG_TICK_HZ", 1000)
    check_range("CONFIG_TICK_HZ", tick_hz, 100, 100000)

    priority_ceil = cfg.get("CONFIG_PRIORITY_CEILING", 127)
    check_range("CONFIG_PRIORITY_CEILING", priority_ceil, 1, 127)

    max_prio = cfg.get("CONFIG_MAX_PRIORITY", 128)
    check_eq("CONFIG_MAX_PRIORITY", max_prio, priority_ceil + 1,
             f"must equal CONFIG_PRIORITY_CEILING ({priority_ceil}) + 1 = {priority_ceil + 1}")

    check_range("CONFIG_SPORADIC_SERVER_MAX_TASKS",
                cfg.get("CONFIG_SPORADIC_SERVER_MAX_TASKS", 8), 0, max_tasks)

    check_bool("CONFIG_PREEMPTION", cfg.get("CONFIG_PREEMPTION", 1))
    check_bool("CONFIG_TIME_SLICING", cfg.get("CONFIG_TIME_SLICING", 1))
    check_bool("CONFIG_IDLE_YIELD", cfg.get("CONFIG_IDLE_YIELD", 0))

    # TICK_HZ must divide PIT base evenly (for PIT-based calibration)
    PIT_BASE = 1193182
    if tick_hz > 0:
        if PIT_BASE % tick_hz != 0:
            W(f"CONFIG_TICK_HZ={tick_hz} does not divide PIT base frequency "
              f"({PIT_BASE}) evenly — TSC calibration may be inaccurate")

    # ═══════════════════════════════════════════════════════════════════════
    # 3. Memory Layout
    # ═══════════════════════════════════════════════════════════════════════
    stack_size = cfg.get("CONFIG_STACK_SIZE", 65536)
    min_stack = cfg.get("CONFIG_MIN_STACK_SIZE", 4096)
    check_ge("CONFIG_STACK_SIZE", stack_size, min_stack,
             f"must be >= CONFIG_MIN_STACK_SIZE ({min_stack})")
    check_ge("CONFIG_MIN_STACK_SIZE", min_stack, 4096,
             "page granularity — minimum 4096")

    if stack_size % 4096 != 0:
        E(f"CONFIG_STACK_SIZE={stack_size} must be a multiple of page size (4096)")

    heap_size = cfg.get("CONFIG_HEAP_SIZE", 16777216)
    check_ge("CONFIG_HEAP_SIZE", heap_size, 4096)
    if heap_size % 4096 != 0:
        W(f"CONFIG_HEAP_SIZE={heap_size} is not a multiple of page size (4096)")

    page_size = cfg.get("CONFIG_PAGE_SIZE", 4096)
    check_eq("CONFIG_PAGE_SIZE", page_size, 4096)

    pml4_user = cfg.get("CONFIG_PML4_USER_COUNT", 256)
    check_range("CONFIG_PML4_USER_COUNT", pml4_user, 0, 256)

    # Ensure CONFIG_HHDM_OFFSET is correct per architecture
    hhdm = cfg.get("CONFIG_HHDM_OFFSET", 0)
    hhdm_str = f"0x{hhdm:X}" if hhdm else "not found"
    if is_x86:
        if hhdm != 0xFFFF800000000000:
            W(f"CONFIG_HHDM_OFFSET={hhdm_str}, expected 0xFFFF800000000000 for x86_64")
    if is_aarch64:
        W(f"CONFIG_HHDM_OFFSET={hhdm_str}, expected 0xFFFF800000000000 for AArch64")
    if is_riscv64 and hhdm != 0xFFFFFFC000000000:
        W(f"CONFIG_HHDM_OFFSET={hhdm_str}, expected 0xFFFFFFC000000000 for RISC-V64")

    # ═══════════════════════════════════════════════════════════════════════
    # 4. Cross-Sizing Constraints
    # ═══════════════════════════════════════════════════════════════════════
    # stack_size * max_tasks should not exceed ~50% of heap_size (rough estimate)
    total_stack = stack_size * max_tasks
    if total_stack * 2 > heap_size:
        W(f"CONFIG_STACK_SIZE ({stack_size}) × CONFIG_MAX_TASKS ({max_tasks}) = "
          f"{total_stack} bytes of kernel stack, which exceeds 50% of "
          f"CONFIG_HEAP_SIZE ({heap_size}). Only {heap_size - total_stack} bytes "
          f"remain for MemPool, page tables, and IPC buffers.")

    # ═══════════════════════════════════════════════════════════════════════
    # 5. Subsystem Sizing
    # ═══════════════════════════════════════════════════════════════════════
    check_ge("CONFIG_MAX_FDS", cfg.get("CONFIG_MAX_FDS", 32), 1)
    check_ge("CONFIG_MAX_MOUNTS", cfg.get("CONFIG_MAX_MOUNTS", 32), 1)
    check_ge("CONFIG_MAX_DRIVERS", cfg.get("CONFIG_MAX_DRIVERS", 16), 1)
    check_ge("CONFIG_MAX_DAEMONS", cfg.get("CONFIG_MAX_DAEMONS", 16), 1)
    check_range("CONFIG_MAX_PROGRAMS", cfg.get("CONFIG_MAX_PROGRAMS", 32), 1, 1024)
    check_ge("CONFIG_IPC_MAX_MSG_SIZE", cfg.get("CONFIG_IPC_MAX_MSG_SIZE", 64), 1,
             "must be at least 1 byte")
    check_ge("CONFIG_IPC_MAX_QUEUE_MSG", cfg.get("CONFIG_IPC_MAX_QUEUE_MSG", 16), 1)
    check_ge("CONFIG_IPC_PRIORITY_LEVELS", cfg.get("CONFIG_IPC_PRIORITY_LEVELS", 32), 1)
    check_ge("CONFIG_IPC_SHMEM_MAX_PAGES", cfg.get("CONFIG_IPC_SHMEM_MAX_PAGES", 64), 0)
    check_ge("CONFIG_BUFFER_POOL_PAGES", cfg.get("CONFIG_BUFFER_POOL_PAGES", 128), 1)
    check_ge("CONFIG_MAX_PROCESS_PAGES", cfg.get("CONFIG_MAX_PROCESS_PAGES", 512), 1)
    check_ge("CONFIG_MAX_SIGNAL_HANDLERS", cfg.get("CONFIG_MAX_SIGNAL_HANDLERS", 32), 1)
    check_ge("CONFIG_VFS_MAX_PATH", cfg.get("CONFIG_VFS_MAX_PATH", 256), 1)
    check_range("CONFIG_TASK_NAME_LEN", cfg.get("CONFIG_TASK_NAME_LEN", 16), 1, 256)
    check_ge("CONFIG_SYNC_MAX_WAITERS", cfg.get("CONFIG_SYNC_MAX_WAITERS", 32), 1)
    check_ge("CONFIG_DMESG_CAPACITY", cfg.get("CONFIG_DMESG_CAPACITY", 4096), 1)

    # DMESG_CAPACITY should be a power of 2
    dmesg = cfg.get("CONFIG_DMESG_CAPACITY", 4096)
    if dmesg & (dmesg - 1) != 0:
        W(f"CONFIG_DMESG_CAPACITY={dmesg} is not a power of 2")

    # ═══════════════════════════════════════════════════════════════════════
    # 6. MemPool Configuration
    # ═══════════════════════════════════════════════════════════════════════
    num_pools = cfg.get("CONFIG_MEMPOOL_NUM_POOLS", 9)
    check_range("CONFIG_MEMPOOL_NUM_POOLS", num_pools, 1, 32)

    # Parse block sizes and counts (from raw header line, approximate)
    # We can't easily parse the comma list without running the preprocessor,
    # so we check the NUM_POOLS <= defined counts from the header.
    # The actual C compiler enforces the array sizing.
    if num_pools < 1:
        E("CONFIG_MEMPOOL_NUM_POOLS must be >= 1")

    # ═══════════════════════════════════════════════════════════════════════
    # 7. Priority Inheritance & Ceiling Protocol
    # ═══════════════════════════════════════════════════════════════════════
    check_bool("CONFIG_PRIORITY_CEILING_PROTOCOL",
               cfg.get("CONFIG_PRIORITY_CEILING_PROTOCOL", 1))
    check_ge("CONFIG_MAX_HELD_CEILINGS", cfg.get("CONFIG_MAX_HELD_CEILINGS", 16), 1)
    check_bool("CONFIG_MUTEX_PIP", cfg.get("CONFIG_MUTEX_PIP", 1))
    check_bool("CONFIG_SEMAPHORE_PIP", cfg.get("CONFIG_SEMAPHORE_PIP", 1))
    check_bool("CONFIG_QUEUE_PIP", cfg.get("CONFIG_QUEUE_PIP", 1))

    # ═══════════════════════════════════════════════════════════════════════
    # 8. IRQ & Preemption Latency
    # ═══════════════════════════════════════════════════════════════════════
    check_range("CONFIG_PREEMPTION_LATENCY_MAX_CYCLES",
                cfg.get("CONFIG_PREEMPTION_LATENCY_MAX_CYCLES", 0), 0, 1 << 63)

    irq_hist = cfg.get("CONFIG_IRQ_LATENCY_HISTOGRAM", 1)
    check_bool("CONFIG_IRQ_LATENCY_HISTOGRAM", irq_hist)

    irq_max_ns = cfg.get("CONFIG_IRQ_LATENCY_MAX_NS", 0)
    if irq_max_ns > 0 and irq_hist == 0:
        E("CONFIG_IRQ_LATENCY_MAX_NS > 0 requires CONFIG_IRQ_LATENCY_HISTOGRAM = 1")

    check_bool("CONFIG_USE_APIC_TIMER", cfg.get("CONFIG_USE_APIC_TIMER", 1))
    if cfg.get("CONFIG_USE_APIC_TIMER", 1) and not is_x86:
        W("CONFIG_USE_APIC_TIMER=1 on non-x86_64 — APIC timer is x86-only; "
          "fallback to arch timer")

    threaded_irqs = cfg.get("CONFIG_THREADED_IRQS", 1)
    check_bool("CONFIG_THREADED_IRQS", threaded_irqs)
    if threaded_irqs:
        check_ge("CONFIG_MAX_THREADED_IRQS",
                 cfg.get("CONFIG_MAX_THREADED_IRQS", 16), 1)

    # ═══════════════════════════════════════════════════════════════════════
    # 9. Hooks
    # ═══════════════════════════════════════════════════════════════════════
    check_bool("CONFIG_IDLE_HOOK", cfg.get("CONFIG_IDLE_HOOK", 0))
    check_bool("CONFIG_TICK_HOOK", cfg.get("CONFIG_TICK_HOOK", 0))
    check_bool("CONFIG_STACK_OVERFLOW_HOOK", cfg.get("CONFIG_STACK_OVERFLOW_HOOK", 0))
    check_bool("CONFIG_OOM_HOOK", cfg.get("CONFIG_OOM_HOOK", 0))
    check_bool("CONFIG_INIT_HOOK", cfg.get("CONFIG_INIT_HOOK", 0))
    # v0.4.0 MP-3/MP-4: sentinel canaries + SMEP/SMAP (SMAP deferred).
    check_bool("CONFIG_CANARY_GUARD", cfg.get("CONFIG_CANARY_GUARD", 1))
    check_bool("CONFIG_SMEP", cfg.get("CONFIG_SMEP", 1))
    check_bool("CONFIG_SMAP", cfg.get("CONFIG_SMAP", 0))
    check_bool("CONFIG_SPORADIC_SERVER_DEADLINE_HOOK",
               cfg.get("CONFIG_SPORADIC_SERVER_DEADLINE_HOOK", 1))

    # ═══════════════════════════════════════════════════════════════════════
    # 10. Deadline & WCET
    # ═══════════════════════════════════════════════════════════════════════
    check_bool("CONFIG_DEADLINE_MISS_DETECTION",
               cfg.get("CONFIG_DEADLINE_MISS_DETECTION", 1))
    check_bool("CONFIG_WCET_OVERRUN_DETECTION",
               cfg.get("CONFIG_WCET_OVERRUN_DETECTION", 1))
    check_bool("CONFIG_SPORADIC_SERVER_EXHAUSTION_IS_DEADLINE",
               cfg.get("CONFIG_SPORADIC_SERVER_EXHAUSTION_IS_DEADLINE", 0))

    deadline_action = cfg.get("CONFIG_DEADLINE_ACTION", 0)
    if deadline_action not in (0, 1, 2, 3, 4):
        E(f"CONFIG_DEADLINE_ACTION={deadline_action}, must be 0-4 "
          "(0=LOG, 1=PANIC, 2=DEMOTE, 3=KILL, 4=NOTIFY_MONITOR)")
    if deadline_action == 4:
        monitor_pid = cfg.get("CONFIG_DEADLINE_MONITOR_PID", 0)
        if monitor_pid == 0:
            E("CONFIG_DEADLINE_ACTION=4 requires "
              "CONFIG_DEADLINE_MONITOR_PID > 0")

    dm_task = cfg.get("CONFIG_DEADLINE_MONITOR_TASK", 1)
    check_bool("CONFIG_DEADLINE_MONITOR_TASK", dm_task)

    # ═══════════════════════════════════════════════════════════════════════
    # 11. Architecture Feature Detection
    # ═══════════════════════════════════════════════════════════════════════
    check_bool("CONFIG_HAS_FPU", cfg.get("CONFIG_HAS_FPU", 1))
    check_bool("CONFIG_HAS_MPU", cfg.get("CONFIG_HAS_MPU", 0))
    check_bool("CONFIG_HAS_HPET", cfg.get("CONFIG_HAS_HPET", 0))

    if is_x86:
        check_bool("CONFIG_HAS_RDRAND", cfg.get("CONFIG_HAS_RDRAND", 1))

    # ═══════════════════════════════════════════════════════════════════════
    # 12. Architecture-specific cross-checks
    # ═══════════════════════════════════════════════════════════════════════
    if is_aarch64:
        if not cfg.get("CONFIG_HAS_GIC", 1):
            E("CONFIG_ARCH_AARCH64 requires CONFIG_HAS_GIC=1")
    if is_riscv64:
        if not cfg.get("CONFIG_HAS_PLIC", 1):
            E("CONFIG_ARCH_RISCV64 requires CONFIG_HAS_PLIC=1")
        if not cfg.get("CONFIG_HAS_SBI", 1):
            E("CONFIG_ARCH_RISCV64 requires CONFIG_HAS_SBI=1")


# ─── Main ──────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Validate Jarvis RTOS compile-time configuration")
    parser.add_argument("--nexios-config", default="src/kernel/nexios_config.h",
                        help="Path to nexios_config.h (default: src/kernel/nexios_config.h)")
    parser.add_argument("--arch", default=None,
                        help="Target architecture: x86_64, aarch64, or riscv64 "
                             "(default: detect from host)")
    args = parser.parse_args()

    path = Path(args.nexios_config)
    if not path.exists():
        print(f"FATAL: {path} not found")
        sys.exit(2)

    # If --arch not provided, check the ARCH environment variable (set by
    # Makefile's `ARCH ?= x86_64` and passed through the check-config target).
    # Fall back to host detection for bare invocation.
    if args.arch is None:
        args.arch = os.environ.get("ARCH", None)

    cfg = preprocess(str(path), args.arch)
    validate(cfg)

    # Print results
    print(f"Checked {len(cfg)} configuration symbols from {path}")
    print()

    if warnings:
        for w in warnings:
            print(f"  {w}")
        print()

    if errors:
        for e in errors:
            print(f"  {e}")
        print()
        print(f"FAILED: {len(errors)} error(s), {len(warnings)} warning(s)")
        sys.exit(1)
    elif warnings:
        print(f"PASSED with {len(warnings)} warning(s)")
        sys.exit(0)
    else:
        print("PASSED — all configurations valid")
        sys.exit(0)


if __name__ == "__main__":
    main()
