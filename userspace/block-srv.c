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
    BLK_OP_BIND = 0,
    BLK_OP_GEOMETRY = 1,
    BLK_OP_READ_BLOCK = 2,
    BLK_OP_WRITE_BLOCK = 3,
    BLK_OP_FLUSH = 4,
    BLK_OP_DMA_MAP = 5,
    BLK_OP_DMA_UNMAP = 6,
    BLK_OP_LOOPBACK = 7,
    BLK_OP_INJECT_MID_TRANSFER = 8,
} block_srv_op_t;

struct BlockRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t lba;
    uint64_t arg;
    uint32_t handle;
    uint16_t status_op;
    uint16_t status;
};

static void blk_assert_shape(void) {
    char check[sizeof(struct BlockRpc) <= 48 ? 1 : -1];
    (void)check;
}

static unsigned char g_loopback[512];

int main(void) {
    blk_assert_shape();
    (void)getpid();
    for (unsigned i = 0; i < sizeof(g_loopback); ++i)
        g_loopback[i] = (unsigned char)(i & 0xFF);
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue; // fail-closed: no reply on transport error
        struct BlockRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; // torn/stale payload: kernel type must match body op (audit S3)

        struct BlockRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;

        switch (req.op) {
        case BLK_OP_BIND:
            rep.status = 0;
            break;
        case BLK_OP_LOOPBACK:
            rep.status = (g_loopback[0] == 0x00 &&
                          g_loopback[511] == (unsigned char)0xFF)
                             ? 0
                             : (uint16_t)(uint16_t)-5;
            break;
        case BLK_OP_DMA_MAP:
            /* Zero/invalid handles fail closed (-EACCES shape). */
            rep.status = (req.handle == 0) ? (uint16_t)(uint16_t)-13
                                           : (uint16_t)(uint16_t)-38;
            break;
        default:
            /* Step-a diagnostic shape: no hardware yet. */
            rep.status = (uint16_t)(uint16_t)-38;
            break;
        }

        ipc_send(1, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
