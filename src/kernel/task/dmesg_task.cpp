/*
 * NexIOS RTOS — Kernel Log Consumer Task
 * Asynchronously drains dmesg ring buffer to UART with structured formatting.
 */

/// @file dmesg_task.cpp
/// @brief Dmesg consumer task: drains ring-buffer log entries to UART.

#include <kernel/task/dmesg_task.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/sync_errors.hpp>
#include <kernel/vfs/vfs_errors.hpp>
#include <kernel/memory/mempool_errors.hpp>
#include <kernel/task/scheduler_errors.hpp>
#include <kernel/ipc/ipc_errors.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <string.hpp>

namespace kernel::task {

void dmesg_task_main() {
    log::LogEntry entry{};
    char buf[log::DMESG_RENDER_CAP];

    while (true) {
        arch::pause();
        while (log::DmesgService::instance().pop(entry)) {
            // Canonical issue-#234 line format via the single renderer.
            log::format_dmesg_entry(buf, sizeof(buf), entry);
            arch::Serial::puts(buf);

            for (int i = 0; i < 100; ++i)
                arch::pause();
        }

        kernel::Scheduler::reschedule();
    }
}

} // namespace kernel::task