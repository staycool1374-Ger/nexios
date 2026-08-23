# Test Cases — v0.4.x (Phase 5: SMP + Multicore)

## Branch: testbed only

*Outline — test details to be expanded when implementation begins.*

### Per-CPU Data Isolation — test_percpu.cpp
- Each CPU has isolated GDT with unique base/limit
- Each CPU has isolated TSS with unique kernel stack
- Per-CPU data access via `gs:` segment is CPU-local
- Cross-CPU data access returns local copy (not sibling's)

### Local APIC — test_lapic.cpp
- LAPIC mapped at correct MMIO base (0xFEE00000)
- LAPIC ID register matches APIC ID
- LAPIC version register returns valid value (0x0014 or similar)
- LAPIC timer one-shot fires at correct interval
- LAPIC timer periodic fires at correct interval
- LAPIC EOI clears in-service register
- X2APIC mode enabled if supported

### I/O APIC — test_ioapic.cpp
- I/O APIC found at correct MMIO base
- I/O APIC ID, version registers match expected values
- IRQ redirection entry routes IRQ to target CPU
- Mask/unmask IRQ via redirection entry mask bit
- Invalid IRQ vector rejected

### SMP Boot — test_smp_boot.cpp
- Secondary APs woken via INIT-SIPI-SIPI sequence
- All detected APs reach `ap_startup()` entry point
- Each AP has unique kernel stack and PML4
- Scheduler sees N+1 tasks after boot (BSP + N APs)

### Core State Isolation — test_core_isolation.cpp
- Each core has independent PML4
- Kernel stack per core is non-overlapping
- TSS IST stacks per core are independent
- Cross-core pointer corruption test (no leakage)

### IRQ Priority (TPR) — test_irq_priority.cpp
- APIC TPR blocks interrupts below threshold
- High-priority IRQ preempts low-priority handler
- TPR restored after handler completes
- NMI not blocked by TPR

### Distributed Run Queues — test_percpu_sched.cpp
- Each CPU has independent run queue
- Task enqueued on one CPU does not appear on another's queue
- Lock contention on run-queue operations is eliminated
- N tasks on M CPUs: each CPU serves its own queue

### Load Balancing — test_load_balancer.cpp
- Idle CPU pulls tasks from busy CPU's queue (idle-pull)
- Overloaded CPU pushes tasks to idle CPU (work-push)
- Load balancer does not migrate real-time tasks
- Migration threshold prevents thrashing
- Load balanced within 10% deviation across CPUs

### Core Affinity — test_affinity.cpp
- `SYS_SET_AFFINITY` pins task to specific CPU
- `SYS_GET_AFFINITY` returns current CPU mask
- Pinned task never runs on non-affine CPU
- Affinity mask with no CPUs online returns EINVAL
- sched_setaffinity/sched_getaffinity wrappers in libc

### Cache Coloring — test_cache_coloring.cpp
- Consecutive allocations get different cache colors
- Cache color collisions minimized for threaded access
- Coloring algorithm handles arbitrary allocation sizes
- Page address determines color (bit extraction)

### SMP Synchronization Primitives — test_smp_sync.cpp
- SMP spinlock: multiple CPUs, lock/unlock, no data race
- Reader-writer lock: concurrent readers, exclusive writer
- Spinlock with IRQ save/restore works on all CPUs
- Lock holder CPU migration does not corrupt lock
- Ticket lock: FIFO ordering under contention

### SMP Verification — test_smp_verify.cpp
- Single-core WCET/WCRT bounds hold on SMP
- Lock contention adds bounded latency
- No priority inversion across CPUs
- 24h stress: all CPUs, random fork/exit/IPC

### PCID Support — test_pcid.cpp
- PCID (Process Context Identifier) enabled in CR4
- TLB entries tagged with PCID on context switch
- Address space switch with same PCID invalidates only matching entries
- PCID rollover handled (all 4096 IDs exhausted)

### Selective TLB Invalidation — test_invpcid.cpp
- `INVPCID` with individual-address mode invalidates single page
- `INVPCID` with single-context mode invalidates all entries for one PCID
- `INVPCID` with all-context mode invalidates global entries
- Non-existent address ignored by INVPCID

### Lazy TLB Shootdown — test_lazy_tlb.cpp
- Remote TLB invalidation deferred until next context switch
- Lazy shootdown coalesces multiple invalidations
- Memory corruption prevented despite lazy invalidation
- Lazy timeout forces flush if no context switch occurs

### IPI Batching — test_ipi_batching.cpp
- Multiple pending TLB flushes batched into single IPI
- Batch processing order is deterministic
- IPI count per test is less than unbatched version
- Batch size does not overflow IPI message buffer

### Latency Profiling — test_tlb_latency.cpp
- Per-TLB-flush latency recorded
- Average latency < unbatched baseline
- P99 latency bounded (< 2x average)
- Latency proportional to number of active CPUs

### PML4 Synchronization — test_pml4_sync.cpp
- Page table update on one CPU visible to all CPUs
- Shootdown completes before write is considered visible
- Shared page modification (fork) synchronized across CPUs
- Page removal invalidated on all CPUs before page reuse
