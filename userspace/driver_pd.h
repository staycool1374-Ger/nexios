#ifndef NEXIOS_DRIVER_PD_H
#define NEXIOS_DRIVER_PD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ipc.h>

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

typedef enum {
    DRV_OP_BIND = 0,
    DRV_OP_READ = 1,
    DRV_OP_WRITE = 2,
    DRV_OP_POLL = 3,
    DRV_OP_MODE_SET = 4,
    DRV_OP_PRESENT = 5,
    DRV_OP_INJECT_FAULT = 240,
} drv_op_t;

struct DriverRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint16_t status_op;
    uint16_t status;
    uint32_t rsvd2;
};

static void drv_assert_shape(void) {
    char check[sizeof(struct DriverRpc) <= 48 ? 1 : -1];
    (void)check;
}

static inline int drv_send(unsigned long dest, unsigned long type,
                           const void *data, unsigned long size) {
    return ipc_send(dest, type, data, size, 0);
}

static inline int drv_recv(void *buf, unsigned long max_size,
                           unsigned long timeout_ticks) {
    return ipc_recv_timeout(buf, max_size, timeout_ticks);
}

#endif
