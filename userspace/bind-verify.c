/* QE live BIND probe for issue #441 (PD reply-to-owner_id fix).
 *
 * Reads PD pids from /tmp/bindpids (whitespace-separated, written by the
 * expect harness from `tasks` output in harness role order), sends each a
 * BIND RPC (op 0, owner_id = getpid()), waits with a bounded timeout
 * (fail-closed: timeout or shape mismatch => rc<0). Prints
 * `S BIND idx=<i> pid=<pid> rc=<rc>` per PD + `S BIND probe op=done fails=N`.
 * Exit 0 iff all rc=0. Timer PD (role index 3 in the 441 harness order)
 * validates status==0; others validate arg0==1 or status==0 (driver vs
 * server reply shapes share the 48-byte layout).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ipc.h>

#define MSG_TYPE_REPLY 0xF0000002
#define BIND_TIMEOUT_TICKS 1000u

struct Rpc48 {
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

static int bind_one(unsigned long pid, int is_timer) {
    struct Rpc48 req;
    memset(&req, 0, sizeof(req));
    req.op = 0;
    req.owner_id = (uint32_t)getpid();
    if (ipc_send(pid, 0, &req, sizeof(req), 0) < 0)
        return -1;
    static unsigned char rbuf[64];
    int rc = ipc_recv_timeout(rbuf, sizeof(rbuf), BIND_TIMEOUT_TICKS);
    if (rc != (int)MSG_TYPE_REPLY)
        return -2;
    struct Rpc48 rep;
    memset(&rep, 0, sizeof(rep));
    unsigned long copy = sizeof(rep) < 48 ? sizeof(rep) : 48;
    memcpy(&rep, rbuf, copy);
    if (rep.op != 0)
        return -3;
    if (rep.owner_id != (uint32_t)getpid())
        return -4;
    if (is_timer) {
        if (rep.status != 0)
            return -5;
    } else {
        if (rep.arg0 != (uint64_t)1 && rep.status != 0)
            return -5;
    }
    return 0;
}

int main(void) {
    int fd = open("/tmp/bindpids", 0);
    if (fd < 0) {
        printf("S BIND probe op=open_pids rc=-1\n");
        return 1;
    }
    char buf[128];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("S BIND probe op=read_pids rc=-1\n");
        return 1;
    }
    buf[n] = '\0';
    unsigned long pids[16];
    int npids = 0;
    unsigned long cur = 0;
    int in_num = 0;
    for (long i = 0; i <= n && npids < 16; ++i) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10u + (unsigned long)(c - '0');
            in_num = 1;
        } else if (in_num) {
            pids[npids++] = cur;
            cur = 0;
            in_num = 0;
        }
    }
    if (npids == 0) {
        printf("S BIND probe op=parse_pids rc=-1\n");
        return 1;
    }
    int fails = 0;
    for (int i = 0; i < npids; ++i) {
        int rc = bind_one(pids[i], i == 3);
        printf("S BIND idx=%d pid=%lu rc=%d\n", i, pids[i], rc);
        if (rc != 0)
            ++fails;
    }
    printf("S BIND probe op=done fails=%d of=%d\n", fails, npids);
    return fails == 0 ? 0 : 2;
}
