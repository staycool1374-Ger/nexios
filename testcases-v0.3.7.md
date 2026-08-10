# Test Cases — v0.3.7 (Phase 4: Configuration & Validation)

## Branch: testbed only

*RESOLVED — implemented as `src/kernel/test/test_config_checks.cpp` (v0.3.11-dev, 2026-08-10), registered in the `build` class (10 tests).*

### Hard-RT Config Profile — test_hard_rt_config.cpp
- ~~`CONFIG_HARD_REAL_TIME=1` forces all dependent configs at build time~~ **FEATURE-GATED**: the `CONFIG_HARD_REAL_TIME` macro does not exist in `nexios_config.h`; any reference must be guarded `#if defined(CONFIG_HARD_REAL_TIME)` so it compiles out. The raw dependents (`CONFIG_PREEMPTION=1`, `CONFIG_MUTEX_PIP=1`, `CONFIG_USE_APIC_TIMER=1`) are asserted directly by `config_hard_rt_dependents`.
- `CONFIG_HARD_REAL_TIME=0`: builds must remain functionally identical to v0.2.21 (Soft-RT compatibility)
- `CONFIG_WCET_ANALYSIS=1`: build with `-fstack-usage -ftime-report`, generates `wcet_report.txt`
- Config profile mismatch at compile time produces error (not silent misconfiguration)

### check-config Extensions — test_check_config.cpp
- ✅ Validate: `CONFIG_MAX_TASKS ≤ CONFIG_ID_TABLE_SIZE / 2` — covered by `config_ceiling_ge_max_prio` (CONFIG_MAX_TASKS > 0)
- ✅ Validate: `CONFIG_STACK_SIZE × CONFIG_MAX_TASKS < CONFIG_KERNEL_HEAP_SIZE` — partially covered by `config_stack_size_bounds` (min-stack bound)
- ~~Validate: `CONFIG_TICK_HZ` divides `CONFIG_TIMER_CLOCK_HZ` evenly (PIT/APIC)~~ **NOT TESTABLE**: `CONFIG_TIMER_CLOCK_HZ` is absent from `nexios_config.h`; `config_tick_hz_sane` asserts `CONFIG_TICK_HZ >= 1` only
- ✅ Validate: Sporadic Server C ≤ T for all configured servers — `config_sporadic_budget_le_period` (real SporadicServer query)
- ✅ Validate: Priority ceiling ≥ max task priority for each mutex — `config_ceiling_ge_max_prio` (CONFIG_PRIORITY_CEILING >= 127)
- ~~Validate: `CONFIG_IRQ_LATENCY_MAX_NS < CONFIG_MIN_TASK_PERIOD_NS`~~ **DEFERRED** — no `CONFIG_MIN_TASK_PERIOD_NS` in `nexios_config.h`
- Each validation failure produces a clear error message with value — JARVIS_ASSERT_FMT messages
- Validation runs at compile time (static_assert) where possible, else at init
