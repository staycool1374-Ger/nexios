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

namespace {

void format_u64(char *&p, char *end, uint64_t v) {
    if (p >= end)
        return;
    char buf[24];
    int len = 0;
    if (v == 0) {
        buf[len++] = '0';
    } else {
        while (v > 0 && len < 23) {
            buf[len++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        for (int i = 0; i < len / 2; ++i) {
            char c = buf[i];
            buf[i] = buf[len - 1 - i];
            buf[len - 1 - i] = c;
        }
    }
    for (int i = 0; i < len && p < end; ++i)
        *p++ = buf[i];
}

} // anonymous namespace

void dmesg_task_main() {
    log::LogEntry entry{};
    char buf[256];

    while (true) {
        arch::pause();
        while (log::DmesgService::instance().pop(entry)) {
            char *p = buf;
            char *end = buf + sizeof(buf) - 1;

            const char *prefix = "[DMESG] TS=";
            while (*prefix && p < end)
                *p++ = *prefix++;
            format_u64(p, end, entry.timestamp);

            const char *task_str = " TASK=";
            while (*task_str && p < end)
                *p++ = *task_str++;
            format_u64(p, end, entry.task_id);

            const char *err_str =
                log::base_code_is_info(entry.error_code) ? " INFO=" : " ERR=";
            while (*err_str && p < end)
                *p++ = *err_str++;
            const char *sub = log::subsystem_name(entry.subsystem);
            while (*sub && p < end)
                *p++ = *sub++;
            *p++ = ':';
            const char *err_name =
                log::error_string(entry.subsystem, entry.error_code);
            while (*err_name && p < end)
                 *p++ = *err_name++;

            const char *msg_str = ": ";
            while (*msg_str && p < end)
                *p++ = *msg_str++;
            const char *msg = entry.message; // owned char array, never null
            while (*msg && p < end)
                *p++ = *msg++;

            *p++ = '\n';
            *p = '\0';

            arch::Serial::puts(buf);

            for (int i = 0; i < 100; ++i)
                arch::pause();
        }

        kernel::Scheduler::reschedule();
    }
}

} // namespace kernel::task