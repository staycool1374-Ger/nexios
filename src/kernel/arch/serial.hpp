#pragma once
#include <kernel/arch/hal/serial.hpp>

namespace arch {

// FLAW-08 bound: a dead/hung COM1 must never hang the core. A working
// 16550 sets THRE within microseconds, so this is a bound not a spin
// target; expiry drops the byte (fail-open, same as Serial::putchar).
constexpr int kSerialTxWaitIters = 1000000;

} // namespace arch
