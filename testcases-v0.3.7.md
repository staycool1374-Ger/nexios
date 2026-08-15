# Test Cases — v0.3.7 (Phase 4: Configuration & Validation)

## Branch: testbed only

*Backlog — the remaining v0.3.7 config-validation items NOT covered by
`src/kernel/test/test_config_checks.cpp` (5 tests) + `test_buildsystem.cpp`
(5 tests), registered in the `configuration_build` class (10 tests, v0.3.11-dev).*

### Hard-RT Config Profile — test_hard_rt_config.cpp
- `CONFIG_HARD_REAL_TIME=0`: builds must remain functionally identical to v0.2.21 (Soft-RT compatibility)
- `CONFIG_WCET_ANALYSIS=1`: build with `-fstack-usage -ftime-report`, generates `wcet_report.txt`
- Config profile mismatch at compile time produces error (not silent misconfiguration)
