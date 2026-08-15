# Test Cases — v0.3.10 (Phase 4: Documentation & Certification Artifacts)

## Branch: testbed only

*Backlog — only the certification-artifact tests remain. The Test-Discipline
Rework (T0–T7) recorded below the line is DONE: all 127 named test functions
exist and are registered (verified 2026-08-15), and the previously "orphaned"
files (test_locking.cpp, test_locking_stress.cpp, test_preemption.cpp,
test_ipc_extended.cpp, test_daemon_restart_crash.cpp) are wired into
`test_registry.cpp` classes + `all`.*

### WCET Analysis Report — test_wcet_report.cpp
- `docs/wcet_analysis.md` generated with measured max cycles per kernel function
- Toolchain: `objdump -d` + static analysis (aiT, OTAWA, or custom script)
- WCET report covers: scheduler dispatch, IPC send/recv, syscall entry/exit, IRQ entry/exit, MemPool alloc/free
- Each WCET figure includes test environment (QEMU or hardware, CPU model, clock speed)
- WCET figures traceable to specific test invocation

### Safety Manual — test_safety_manual.cpp
- `docs/safety_manual.md` documents: assumptions, limitations, configuration rules for ASIL D
- Safety manual covers: scheduler invariants, memory isolation, interrupt latency bounds, watchdog coverage
- Configuration rules specify mandatory settings for each safety level
- Known limitations documented (e.g., OOM policy gap, single-core assumption)

### Traceability Matrix — test_traceability.cpp
- `docs/traceability.csv`: each ISO 26262-6 requirement → design element → code module → test case
- Every requirement in safety manual has at least one test case mapped
- Traceability matrix is machine-readable (CSV)
- CI job validates: no test in matrix without existing test registration
