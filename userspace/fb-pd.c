/* Driver PD: fb (issue #48, strangler step a — BIND + typed-error dispatch pre-flip). */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>
#include "driver_pd.h"

static uint64_t g_my_id;

int main(void) {
    drv_assert_shape();
    g_my_id = (uint64_t)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = drv_recv(rbuf, sizeof(rbuf), 0);
        if (rc <= 0)
            continue;
        struct DriverRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);

        struct DriverRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        switch (req.op) {
        case DRV_OP_BIND:
            rep.arg0 = (uint64_t)1;
            break;
        default:
            rep.status = (uint16_t)(uint16_t)-38;
            break;
        }
        ipc_send(1, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
