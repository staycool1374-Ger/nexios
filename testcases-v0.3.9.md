# Test Cases — v0.3.9 (Phase 4: Dynamic RT Task Spawning & I/O Isolation)

## Branch: testbed only

*FEATURE-GAP REGISTER — none of the v0.3.9 kernel features exist yet in
`src/kernel` on this branch. The sections below are retained as a backlog of
intended test areas. NO `JARVIS_TEST_PASS()` placeholder stubs are created for
these — a stub that always passes documents nothing and hides the gap (T3-1
remediation). Re-open each section when its kernel feature lands on `main`.*

### Real-Time runelf Extensions — test_runelf_rt.cpp
- `runelf` accepts real-time attributes (`--period`, `--wcet`) via shell invocation
- Attributes parsed and passed to `TaskControlBlock::create()` as RT params
- Invalid attributes (period=0, wcet > period) rejected at parse time

### Admission Control — test_admission_control.cpp
- `is_rm_schedulable` (Liu-Layland) called before ELF execution
- New task passes schedulability check → ELF loads and runs
- New task fails schedulability check → ELF rejected with error, no memory allocated
- `is_rm_schedulable` correctly updates after task exit (capacity restored)
- Edge: idle system always admits at least one RT task

### Hardware-Enforced WCET Monitoring — test_wcet_monitor.cpp
- Parsed execution budget (C) bound to HPET/APIC hardware timer on context activation
- Task overruns budget → timer fires → overrun handler invoked (deferred-kill or notification)
- Budget timer reprogrammed on each context switch-in
- Budget timer cancelled on context switch-out (task not consuming cycles)
- Overrun detection does not fire for within-budget execution

### Sandboxed IPC Routing — test_sandboxed_ipc.cpp
- Spawned ELF task has restricted MMIO and port I/O access
- Hardware requests routed through capability-secured IPC channels
- `vfsd` and `iocd` daemons service IPC under Sporadic Server budgets
- Unauthorized capability access returns error (EACCES)
- Capability delegation follows sandbox boundary rules

### Zero-Overhead Shared Memory — test_shm_rt.cpp
- Capability-delegated shared memory pages between driver server and runelf task
- Lock-free SPSC ring buffer spanning page boundaries
- Zero-syscall, zero-copy data ingestion from driver to RT task
- Ring buffer overflow detected (dropped samples or backpressure)

### Incremental ELF Loading — test_incremental_elf.cpp
- Chunked ELF parser maps ≤ `CONFIG_ELF_LOAD_PAGES_PER_SLICE` per scheduler invocation
- Uses `MemPool`/`BufferPool` for temporary I/O chunks (no dynamic allocation)
- Loading mechanism bound to vfsd Sporadic Server budget
- Incremental load does not delay higher-priority RT tasks
