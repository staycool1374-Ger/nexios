#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>
#include <ipc.h>

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

typedef enum {
    LOG_OP_BIND = 0,
    LOG_OP_SINK_PUSH = 1,
    LOG_OP_SINK_DRAIN = 2,
    LOG_OP_METER = 3,
    LOG_OP_INJECT_DROP = 4,
} log_srv_op_t;

struct LogRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t arg0;
    uint64_t arg1;
    uint16_t status_op;
    uint16_t status;
    uint32_t rsvd2;
};

static void log_assert_shape(void) {
    char check[sizeof(struct LogRpc) <= 48 ? 1 : -1];
    (void)check;
}

static unsigned long g_dropped = 0;

int main(void) {
    log_assert_shape();
    (void)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue;
        struct LogRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; // torn/stale payload: kernel type must match body op

        struct LogRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;
        switch (req.op) {
        case LOG_OP_BIND:
            rep.status = 0;
            break;
        case LOG_OP_INJECT_DROP:
            ++g_dropped;
            rep.status = 0;
            break;
        default:
            /* Step-a diagnostic shape: sink stays in-kernel. */
            rep.status = (uint16_t)(uint16_t)-38;
            break;
        }
        (void)g_dropped;
        ipc_send(1, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
