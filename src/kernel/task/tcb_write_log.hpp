#pragma once
#include <types.hpp>
#include <concepts.hpp>

// Freestanding build has no <cstddef>; provide offsetof via the compiler
// builtin so the TCB_WRITE macro works when CONFIG_TCB_WRITE_LOG is enabled.
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

// TCB field-write ring buffer — stray-write tracer.
//
// Implements "Idea #4: Ring-buffer write tracker for TCB fields" from
// docs/investigation-cumulative-corruption.md. Captures the last N writes to
// critical TCB fields (magic, id, kernel_stack, state, ...) so that when a
// corruption is detected we can dump the last modifications and their callers.
//
// This header MUST be included AFTER kernel/task/task.hpp so that the complete
// TaskControlBlock type is visible for offsetof() in the TCB_WRITE macro.
//
// Disabled by default (zero overhead). Enable with -DCONFIG_TCB_WRITE_LOG to
// wrap critical TCB field writes and record them.

namespace kernel::diag {

struct TcbWriteLog {
    uint64_t seq;           ///< Monotonic record sequence (tick order proxy)
    uint64_t tcb_addr;      ///< Address of the TCB being modified
    uint64_t field_offset;  ///< Offset of the field within TaskControlBlock
    uint64_t old_value;     ///< Field value before the write
    uint64_t new_value;     ///< Field value after the write
    void *caller;           ///< __builtin_return_address(0) of the writer
};

/// Record a single critical TCB field write.
void record_tcb_write(uint64_t tcb_addr, uint64_t field_offset,
                      uint64_t old_value, uint64_t new_value);

/// Dump the ring buffer to the serial log (via Logger::raw_write).
/// @param tag optional prefix line (e.g. a corruption-site marker).
void dump_tcb_write_log(const char *tag = nullptr);

namespace detail {

/// Lossless coercion of any TCB field value (integer, enum, or pointer) to
/// uint64_t for ring-buffer storage. Overloads avoid reinterpret/static cast
/// ambiguity for pointer-typed fields such as kernel_stack.
inline uint64_t to_u64(uint64_t v) { return v; }
template <typename T>
inline uint64_t to_u64(T *p) {
    return reinterpret_cast<uint64_t>(p);
}
/// @brief Generic fallback for integer and enum field values (issue
///        #348-E: constrained so struct misuse fails at the call site,
///        not inside the cast).
template <typename T>
    requires kernel::Integral<T> || kernel::Enum<T>
inline uint64_t to_u64(T v) {
    return static_cast<uint64_t>(v);
}

} // namespace detail
} // namespace kernel::diag

#ifdef CONFIG_TCB_WRITE_LOG
#define TCB_WRITE(tcb, field, val)                                            \
    do {                                                                      \
        auto *_tcbrc = (tcb);                                                 \
        kernel::diag::record_tcb_write(                                       \
            reinterpret_cast<uint64_t>(_tcbrc),                               \
            offsetof(TaskControlBlock, field),                                \
            kernel::diag::detail::to_u64(_tcbrc->field),                      \
            kernel::diag::detail::to_u64(val));                               \
        _tcbrc->field = (val);                                                \
    } while (0)
#else
#define TCB_WRITE(tcb, field, val) do { (tcb)->field = (val); } while (0)
#endif
