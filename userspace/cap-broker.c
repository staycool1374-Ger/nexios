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
    CAP_OP_BIND = 0,
    CAP_OP_LOOPBACK = 1,
    CAP_OP_SET_POLICY = 2,
    CAP_OP_GET_POLICY = 3,
    CAP_OP_CLEAR_POLICY = 4,
} cap_broker_op_t;

struct CapBrokerRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t target_id;
    uint32_t mask;
    uint32_t rsvd2;
    uint64_t arg0;
    uint16_t status_op;
    uint16_t status;
    uint32_t rsvd3;
};

static void cap_assert_shape(void) {
    char check[sizeof(struct CapBrokerRpc) <= 48 ? 1 : -1];
    (void)check;
}

int main(void) {
    cap_assert_shape();
    (void)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue;
        struct CapBrokerRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; // torn/stale payload: kernel type must match body op

        struct CapBrokerRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;
        switch (req.op) {
        case CAP_OP_BIND:
            rep.status = 0;
            break;
        case CAP_OP_LOOPBACK:
            rep.arg0 = req.arg0;
            rep.status = 0;
            break;
        default:
            /* Step-a diagnostic shape: policy stays kernel-side. */
            rep.status = (uint16_t)(uint16_t)-38;
            break;
        }
        // Reply to the requester (owner_id carried in the RPC body):
        // send_sync_bounded waits on the REQUESTER's own queue with a
        // sender match, so a reply addressed to PID 1 (init) is a dead
        // letter and every RPC times out (issue #51 step-b evidence).
        // Mirrors the iocd args[0] requester convention (issue #441).
        ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
