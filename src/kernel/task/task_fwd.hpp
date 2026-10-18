/// @file task_fwd.hpp
/// @brief Forward declaration of TaskControlBlock.
///
/// Pointer convention (issue #324 Tick 1 / JRVS-SCHED-007): registry and
/// list APIs take TaskControlBlock& (never null at any live call site);
/// TaskControlBlock* is reserved for genuinely-nullable outputs
/// (find_task, current_task, first_ptr/next_ptr).  Trust-boundary checks
/// (is_valid/safe_tcb) stay only where links cross ISR-vs-task races or
/// snapshot restore — not on internally-owned, freshly-verified pointers.

#pragma once

namespace kernel {

struct TaskControlBlock;

} // namespace kernel
