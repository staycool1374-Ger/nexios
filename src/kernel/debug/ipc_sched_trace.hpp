/// @file ipc_sched_trace.hpp
/// @brief Reusable diagnostic harness for IPC reply-loss (H1) and deferred-
///        switch self-deadlock (H2) analysis in the scheduler/IPC subsystem.
///
/// #604 RETIRED the scheduler/IPC trace kit (all gated call sites deleted,
/// and both gated arms removed).  What remains is the ungated formatting kit
/// (trace/fmt_u64/fmt_str, reused by tcb_write_log.cpp) plus the H2 event-ring
/// declaration (h2_record/h2_dump_ring, defined in task/scheduler_debug.cpp).
/// History (tag semantics, retirement proof, resurrection recipe) now lives in
/// docs/specs/scheduler.md §13.  Resurrection: tag archive/ipc-sched-trace-kit
/// @ 16c040705 holds the intact kit (header + all gated blocks, macro OFF).
///
/// Proven signatures (what each retired trace tag proved):
///   [SYNC]        entered IPC::send_sync for (cur -> dest)
///   [SEND]/[WAKE] a message was pushed into dest's queue and dest was woken
///   [SYNC-FAIL] dest-gone-empty  -> genuine peer death, reply never arrived (OK)
///   [SYNC-FAIL] dest-gone-reply  -> H1 PROVEN: reply present in own queue but
///                                    send_sync returned false and discarded it
///   [WEDGE]      H2 candidate: save_rsp_to != 0 while a READY task is orphaned
///                (inrq=1, phys=0) and the ready bitmap is empty -> the timer
///                ISR's rate_monotonic_schedule early-returned on a stale
///                pending-switch flag and never re-dispatched the driver, so
///                the driver could never clear the flag -> self-deadlock.

#pragma once

#include <kernel/arch/qemu_debugcon.hpp>
#include <kernel/arch/serial.hpp>
#include <lib/types.hpp>

// The formatting helpers (fmt_u64/fmt_str/trace) are ALWAYS available: they are
// tiny, side-effect-free buffer formatters reused by tcb_write_log.cpp.

namespace kernel::debug {

/// @brief Emit a raw trace line to the debug console.  On x86_64 the QEMU
///        debugcon (0xE9) is used — lock-free, no UART baud pacing — so the
///        per-tick/per-switch traces do not perturb timing.  UART elsewhere.
inline void trace(const char *msg) {
#if defined(CONFIG_ARCH_X86_64)
    arch::QemuDebugcon::write(msg);
    arch::QemuDebugcon::putc('\n');
#else
    arch::Serial::puts(msg);
    arch::Serial::puts("\n");
#endif
}

/// @brief Print an integer (decimal, unsigned) appended at buf[pos].
inline int fmt_u64(char *buf, int pos, uint64_t v) {
    char tmp[21];
    int i = 0;
    if (v == 0)
        tmp[i++] = '0';
    while (v) {
        tmp[i++] = char('0' + (v % 10));
        v /= 10;
    }
    while (i)
        buf[pos++] = tmp[--i];
    return pos;
}
inline void fmt_str(char *buf, int &pos, const char *s) {
    while (*s)
        buf[pos++] = *s++;
}

} // namespace kernel::debug



// Deferred-switch H2 event ring (issue #368): the single definition of
// h2_record/h2_dump_ring lives in task/scheduler_debug.cpp (TU-local ring
// storage, external-linkage functions).  Declared here so every H2_REC user
// (global_state.cpp, scheduler*.cpp via core/global_state.hpp) sees it.
namespace kernel::debug {
/// @brief Append one deferred-switch event to the H2 ring (debug-only).
/// @param ev Event class (H2_EV_* in core/global_state.hpp).
/// @param a/b/c Event payload words (meaning selected by ev).
void h2_record(uint64_t ev, uint64_t a, uint64_t b, uint64_t c) noexcept;
/// @brief Dump the H2 ring to the debug console (GDB/post-hang use).
void h2_dump_ring() noexcept;
} // namespace kernel::debug
