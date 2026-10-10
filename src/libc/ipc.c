#include <syscall.h>
#include <ipc.h>

int ipc_send(unsigned long dest, unsigned long type, const void* data, size_t size, unsigned long flags) {
    return (int)__syscall5(1, (long)dest, (long)data, (long)type, (long)size);
}

int ipc_recv(void* buf, size_t max_size) {
    return (int)__syscall5(2, 0, (long)buf, (long)max_size, 0);
}

int ipc_send_sync(unsigned long dest, unsigned long type, const void* data, size_t size, void* reply_buf) {
    // Issue #317 option (b): the reply buffer address is threaded through the
    // 5th argument register (reply is written THERE, not over the request).
    return (int)__syscall5r(3, (long)dest, (long)data, (long)type, (long)size,
                            (long)reply_buf);
}

int ipc_notify(unsigned long task_id, unsigned long value) {
    return (int)__syscall5(27, (long)task_id, (long)value, 0, 0);
}

int ipc_notify_wait(unsigned long* value) {
    return (int)__syscall5(28, (long)value, 0, 0, 0);
}

int ipc_event_set(unsigned long task_id, unsigned long bits) {
    return (int)__syscall5(29, (long)task_id, (long)bits, 0, 0);
}

int ipc_event_wait(unsigned long bits, int clear_on_exit) {
    return (int)__syscall5(30, (long)bits, (long)clear_on_exit, 0, 0);
}
