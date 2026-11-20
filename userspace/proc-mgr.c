/* NexIOS RTOS — proc-mgr policy PD (issue #564, F11 strangler step b).
 * Sporadic server SS(1,10,0): process policy + verdicts only, no device
 * caps, no allocation, never exits on bad input.
 *
 * Stage B policy is allow-with-typed-refusals: BIND/LOOPBACK plus the
 * seven ops (FORK/EXEC/WAITPID/KILL/GETPID/SIGNAL/SIGRETURN) are allowed
 * (0 with request echo); STOP/RESUME are refused (-1, STOP_REFUSED
 * equivalent); unknown ops and reserved-field violations are refused
 * with -22 (EINVAL equivalent). The kernel's gates stay authoritative
 * (INV-PROC-11 deny-only): every OK verdict still runs the existing
 * admission/validation/parent_id/class checks in caller context, so this
 * file is a policy seam, not an authority.
 */
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
    PROCMGR_OP_BIND = 0,
    PROCMGR_OP_LOOPBACK = 1,
    PROCMGR_OP_FORK = 2,
    PROCMGR_OP_EXEC = 3,
    PROCMGR_OP_WAITPID = 4,
    PROCMGR_OP_KILL = 5,
    PROCMGR_OP_GETPID = 6,
    PROCMGR_OP_SIGNAL = 7,
    PROCMGR_OP_SIGRETURN = 8,
    PROCMGR_OP_STOP = 9,
    PROCMGR_OP_RESUME = 10,
} proc_mgr_op_t;

struct ProcMgrRpc {
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
    uint32_t rsvd3;
};

static void procmgr_assert_shape(void) {
    char check[sizeof(struct ProcMgrRpc) <= 48 ? 1 : -1];
    (void)check;
}

int main(void) {
    procmgr_assert_shape();
    (void)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue;
        struct ProcMgrRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; // torn/stale payload: kernel type must match body op

        struct ProcMgrRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;
        if (req.rsvd != 0 || req.rsvd3 != 0) {
            rep.status = (uint16_t)(uint16_t)-22;
        } else {
            switch (req.op) {
            case PROCMGR_OP_BIND:
                rep.status = 0;
                break;
            case PROCMGR_OP_LOOPBACK:
                rep.arg0 = req.arg0;
                rep.status = 0;
                break;
            case PROCMGR_OP_FORK:
            case PROCMGR_OP_EXEC:
            case PROCMGR_OP_WAITPID:
            case PROCMGR_OP_KILL:
            case PROCMGR_OP_GETPID:
            case PROCMGR_OP_SIGNAL:
            case PROCMGR_OP_SIGRETURN:
                rep.arg0 = req.arg0;
                rep.arg1 = req.arg1;
                rep.status = 0;
                break;
            case PROCMGR_OP_STOP:
            case PROCMGR_OP_RESUME:
                rep.status = (uint16_t)(uint16_t)-1;
                break;
            default:
                /* Unknown op: typed refusal, never exit. */
                rep.status = (uint16_t)(uint16_t)-22;
                break;
            }
        }
        // Reply to the requester (owner_id carried in the RPC body):
        // send_sync_bounded waits on the REQUESTER's own queue with a
        // sender match, so a reply addressed to PID 1 (init) is a dead
        // letter and every RPC times out (issue #441).
        ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
