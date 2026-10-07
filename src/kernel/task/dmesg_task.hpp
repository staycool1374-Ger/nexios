#pragma once

/*
 * NexIOS RTOS — Kernel Log Consumer Task
 */

/// @file dmesg_task.hpp
/// @brief Kernel log consumer task that drains the dmesg ring buffer to UART.

namespace kernel::task {

void dmesg_task_main();

} // namespace kernel::task