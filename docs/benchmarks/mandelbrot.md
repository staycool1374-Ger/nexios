# Mandelbrot CPU Benchmark

**Algorithm**: Mandelbrot 256×256 (step 2 → 128×128 = 16384 pixels), 64 iterations max, 100 repetitions

**Checksum**: `6393500`

| System | Time | vs Linux | Notes |
|--------|------|----------|-----------|
| **Linux** (Ubuntu, Kernel 6.x) | **29 ms** | 1.00× | g++ 15.2, `-O3`, native i7-3930K |
| **Jarvis RTOS** (QEMU + KVM) | **35 ms** | 1.21× | `-O3`, `-mgeneral-regs-only` (no SSE/AVX), QEMU VM-exit overhead |
| **Jarvis RTOS** (QEMU, no KVM) | **119 ms** | 4.10× | pure software emulation |

## Hardware

- **CPU**: Intel Core i7-3930K @ 3.20 GHz (6 cores / 12 threads, Sandy Bridge-E)
- **RAM**: 32 GB DDR3
- **QEMU**: 256 MB RAM, 1 vCPU

## Analysis

- Jarvis RTOS achieves **~83% of native Linux performance** when running on KVM
- The speed difference (35 vs 29 ms) is explained by:
  - **No SSE/AVX**: `-mgeneral-regs-only` prevents SIMD usage; Linux automatically uses SSE/AVX instructions
  - **QEMU-KVM VM-exit overhead**: PIT interrupts (1000 Hz) cause regular VM exits (~every 1 ms)
  - **TLB/memory overhead**: emulated HW has higher latencies on page-table accesses
- The computation is **integer-based** (no floating point) → fair comparison
