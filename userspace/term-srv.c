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
    TERM_OP_BIND = 0,
    TERM_OP_PROMPT = 1,
    TERM_OP_READLINE = 2,
    TERM_OP_HISTORY = 3,
    TERM_OP_SCROLL = 4,
    TERM_OP_CLEAR = 5,
    TERM_OP_REDRAW = 6,
    TERM_OP_ECHO = 7,
    TERM_OP_CANCEL_OWNERS = 8,
    TERM_OP_INJECT_POISON = 9,
} term_srv_op_t;

struct TermRpc {
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

static void term_assert_shape(void) {
    char check[sizeof(struct TermRpc) <= 48 ? 1 : -1];
    (void)check;
}

static int g_poisoned = 0;

int main(void) {
    term_assert_shape();
    (void)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue;
        struct TermRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; // torn/stale payload: kernel type must match body op

        struct TermRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;
        switch (req.op) {
        case TERM_OP_BIND:
            rep.status = 0;
            break;
        case TERM_OP_INJECT_POISON:
            g_poisoned = 1;
            rep.status = 0;
            break;
        default:
            /* Step-a diagnostic shape: emulation stays in-kernel. */
            rep.status = (uint16_t)(uint16_t)-38;
            break;
        }
        (void)g_poisoned;
        ipc_send(1, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
