# Test Cases — v0.5.x (Phase 6: System Integration)

## Branch: testbed only

*Outline — test details to be expanded when implementation begins.*

### Cross-Boundary Flows — test_cross_boundary.cpp
- Kernel-to-userspace IPC round-trip with payload
- Userspace driver (iocd) delivers keyboard input to shell via IPC
- VFS daemon (vfsd) services open/read/write from userspace
- Capability-gated MMIO access from iocd succeeds
- Unauthorized capability access returns error

### 24-Hour Stress Test — test_stress_24h.cpp
- Sustained fork/exit cycle (10 tasks per second)
- Continuous IPC traffic (100 msg/s between task pairs)
- Memory alloc/free churn (all PMM pages cycled hourly)
- Scheduler invariant checks pass at 1h, 6h, 12h, 24h
- Deadline overrun rate < 0.01%

### IPC Throughput Benchmark — test_ipc_throughput.cpp
- Messages per second between two kernel tasks (single-core)
- Messages per second across kernel-userspace boundary
- Throughput vs payload size (1B, 64B, 256B, 1KB)
- Throughput vs queue depth (1, 4, 16, 64)
- Context switches per second under IPC load

### Context Switch Benchmark — test_cs_bench.cpp
- Task-to-task context switch latency
- Kernel-to-userspace entry/exit latency
- IRQ entry/exit latency
- Syscall entry/exit latency
- All benchmarks publish min/max/avg/p99

### Stack Cookies — test_stack_cookie.cpp
- `-fstack-protector` enabled for kernel build
- Stack cookie initialized at task creation
- Buffer overflow triggers stack smashing handler
- Stack smashing handler halts or logs and aborts
- No false positives on valid code paths

### Pointer Isolation Audit — test_pointer_isolation.cpp
- No raw `reinterpret_cast` in kernel core (verified by grep/pattern)
- All kernel-user boundary crossings use CheckedPtr
- CheckedPtr validates user-range before dereference
- Kernel pointer leaked to userspace detected and blocked

### Release Build — test_release_build.cpp
- `make release` compiles without errors
- Release build uses `-O3`, `-DNDEBUG`
- Release binary is stripped of symbols
- Release binary runs all release tests (`JARVIS_REGISTER_RELEASE_TEST`)
- Release binary boots in QEMU without debug output

### Doxygen Documentation — test_doxygen.cpp
- Doxygen config parses without warnings
- All public APIs have `///` documentation blocks
- Doxygen output generates clean HTML

### printf / scanf — test_libc_stdio.cpp
- `printf("hello")` outputs correct string to stdout
- `printf` with format specifiers (%d, %x, %s, %c, %p) produces expected output
- `snprintf` respects buffer size limit
- `fprintf` writes to specified file descriptor
- `scanf` parses integers, hex, strings from input buffer
- `sscanf` with mismatched specifier returns 0 matches

### malloc / free — test_libc_malloc.cpp
- `malloc(n)` returns aligned, non-overlapping blocks
- `free(p)` returns memory to pool
- `malloc(0)` returns valid pointer (or NULL, per implementation)
- `free(NULL)` is a no-op
- `realloc(p, n)` preserves existing content up to min sizes
- `calloc(n, s)` zeroes allocated memory
- Repeated alloc/free cycle does not leak memory
- OOM returns NULL

### String & Memory — test_libc_string.cpp
- `memcpy`, `memmove`, `memset`, `memcmp` produce correct results for all sizes 0..256
- Overlapping regions handled correctly by `memmove`
- `strlen`, `strcpy`, `strncpy`, `strcat`, `strcmp`, `strncmp` match reference libc
- `strchr`, `strrchr`, `strstr`, `strtok` behave as specified

### Threads & Atomics — test_libc_thread.cpp
- `errno` is thread-local (independent values in concurrent tasks)
- Atomic builtins (`__sync_fetch_and_add`, etc.) compile and produce correct results
- `longjmp`/`setjmp` round-trips correctly

### Libc Integration — test_libc_syscall.cpp
- `printf` internally uses `write` syscall (not raw inline assembly)
- `malloc` calls `SYS_BRK` (or equivalent) for heap expansion
- Libc functions do not corrupt kernel-space memory
- Existing userspace programs (shell, ls, cat) link against new libc without modification
- Binary size increase from libc is within acceptable bounds
