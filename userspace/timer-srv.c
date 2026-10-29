#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <ipc.h>

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

#define TIMER_SRV_MAX_TIMERS 50
#define TIMER_SRV_MAX_TIMERFDS 32

typedef enum {
    TMR_OP_BIND = 0,
    TMR_OP_CLOCK_GETTIME = 1,
    TMR_OP_TIMER_CREATE = 2,
    TMR_OP_TIMER_SETTIME = 3,
    TMR_OP_TIMER_DELETE = 4,
    TMR_OP_TIMERFD_CREATE = 5,
    TMR_OP_TIMERFD_SETTIME = 6,
    TMR_OP_CANCEL_OWNERS = 7,
} timer_srv_op_t;

struct TimerRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t timer_id;
    uint64_t value_ns;
    uint64_t interval_ns;
    uint16_t clock_id;
    int16_t status;
    uint32_t rsvd2;
};

static void tmr_assert_shape(void) {
    char check[sizeof(struct TimerRpc) <= 48 ? 1 : -1];
    (void)check;
}

struct TimerSlot {
    uint8_t used;
    uint8_t fired;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint32_t epoch;
    uint64_t deadline_ticks;
    uint64_t interval_ticks;
};

static struct TimerSlot g_timers[TIMER_SRV_MAX_TIMERS];
static uint32_t g_epoch = 1;
static uint64_t g_my_id = 0;

static unsigned long tmr_now_ticks(void) {
    return (unsigned long)__syscall5(5, 0, 0, 0, 0);
}

static uint64_t tmr_next_deadline(void) {
    uint64_t best = 0;
    for (int i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
        if (!g_timers[i].used || g_timers[i].fired)
            continue;
        if (best == 0 || g_timers[i].deadline_ticks < best)
            best = g_timers[i].deadline_ticks;
    }
    return best;
}

static void tmr_fire_due(unsigned long now) {
    for (int i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
        if (!g_timers[i].used || g_timers[i].fired)
            continue;
        if ((unsigned long)g_timers[i].deadline_ticks <= now)
            g_timers[i].fired = 1;
    }
}

int main(void) {
    tmr_assert_shape();
    g_my_id = (uint64_t)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        uint64_t deadline = tmr_next_deadline();
        unsigned long now = tmr_now_ticks();
        unsigned long timeout = 0;
        if (deadline != 0 && deadline > (uint64_t)now)
            timeout = (unsigned long)(deadline - (uint64_t)now);
        else if (deadline == 0)
            timeout = 0;

        int rc = ipc_recv_timeout(rbuf, sizeof(rbuf), timeout);
        now = tmr_now_ticks();
        tmr_fire_due(now);
        if (rc <= 0)
            continue;

        struct TimerRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);

        struct TimerRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = g_epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.timer_id = req.timer_id;
        rep.status = 0;

        switch (req.op) {
        case TMR_OP_BIND:
            rep.value_ns = (uint64_t)tmr_now_ticks();
            break;
        case TMR_OP_CLOCK_GETTIME:
            rep.value_ns = (uint64_t)tmr_now_ticks() * 1000000ULL;
            break;
        case TMR_OP_CANCEL_OWNERS:
            for (int i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
                if (g_timers[i].used &&
                    g_timers[i].owner_id == req.owner_id &&
                    g_timers[i].owner_gen == req.owner_gen) {
                    g_timers[i].used = 0;
                    g_timers[i].fired = 0;
                }
            }
            break;
        default:
            rep.status = -38;
            break;
        }

        ipc_send(1, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
