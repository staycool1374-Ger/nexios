/* Timer/Event externalization PD (issue #241 strangler step a, #194 flip).
 *
 * Single-threaded deadline engine: 50 POSIX timer slots + 32 timerfd slots
 * + 32 sleep slots, all static (INV-T12: no allocation on RT paths). No
 * locks (single loop). Time is CLOCK_MONOTONIC ns latched monotone
 * (INV-T4: never arch::ticks, never backwards — a backward sample holds
 * the latch and counts g_rewind_events).
 *
 * Main loop (S1 fix, r1 audit): fire-due FIRST on every wake (including
 * spurious), then compute the next wait with overflow-safe ceil division,
 * clamped to [1, kMaxWaitTicks]. timeout_ticks == 0 means block-forever
 * (ipc/ipc.cpp:1826-1828), so a due deadline with no further traffic would
 * hang server + client forever if 0 were passed for a live deadline —
 * the scaffold's :91-99 trap. kMaxWaitTicks = 4000 (next to the define).
 *
 * Cancellation exactness (INV-T5): every client event carries
 * token = (owner_id, owner_gen, slot, slot_gen, epoch). The kernel drops
 * mismatches before waking anybody; a freed slot bumps slot_gen; a
 * restart bumps g_epoch and purges all slots. Tombstone drain is
 * piggyback-bound (no new timer): every fire_due pass first sweeps the
 * pending dead-owner list (send-failure discovery), so linger is at most
 * one wake interval; CANCEL_OWNERS stays the explicit path and applies
 * synchronously. Table-full -EAGAIN stays fail-closed.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <time.h>
#include <ipc.h>

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

#define TIMER_SRV_MAX_TIMERS 50
#define TIMER_SRV_MAX_TIMERFDS 32
#define TIMER_SRV_MAX_SLEEPS 32
#define TIMER_SRV_MAX_PENDING 16

/* Wire version (mirrors kernel kSrvWireVersion; BIND rejects mismatch). */
#define TMR_WIRE_VERSION 2

/* Max bounded wait per recv (ticks); idle (no deadline) blocks forever. */
#define TMR_MAX_WAIT_TICKS 4000

/* Negative errno values used on the wire. */
#define TMR_EAGAIN 11
#define TMR_EINVAL 22
#define TMR_ENOSYS 38
#define TMR_EPROTO 71

typedef enum {
    TMR_OP_BIND = 0,
    TMR_OP_CLOCK_GETTIME = 1,
    TMR_OP_TIMER_CREATE = 2,
    TMR_OP_TIMER_SETTIME = 3,
    TMR_OP_TIMER_DELETE = 4,
    TMR_OP_TIMERFD_CREATE = 5,
    TMR_OP_TIMERFD_SETTIME = 6,
    TMR_OP_CANCEL_OWNERS = 7,
    TMR_OP_ANCHOR_PUSH = 8,
    TMR_OP_TIMERFD_EXPIRED = 9,
    TMR_OP_SLEEP_REQUEST = 10,
    TMR_OP_SLEEP_CANCEL = 11,
    TMR_OP_TIMER_GETTIME = 12,
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
    char check_wire[sizeof(struct TimerRpc) <= 48 ? 1 : -1];
    (void)check_wire;
    char check_timers[TIMER_SRV_MAX_TIMERS == 50 ? 1 : -1];
    (void)check_timers;
    char check_fds[TIMER_SRV_MAX_TIMERFDS == 32 ? 1 : -1];
    (void)check_fds;
}

struct TimerSlot {
    uint8_t used;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint32_t slot_gen;
    uint64_t deadline_ns;
    uint64_t interval_ns;
    uint64_t expiries;
};

struct SleepSlot {
    uint8_t used;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint32_t slot_gen;
    uint64_t token;
    uint64_t deadline_ns;
};

struct PendingPurge {
    uint32_t owner_id;
    uint32_t owner_gen;
};

static struct TimerSlot g_timers[TIMER_SRV_MAX_TIMERS];
static struct TimerSlot g_timerfds[TIMER_SRV_MAX_TIMERFDS];
static struct SleepSlot g_sleeps[TIMER_SRV_MAX_SLEEPS];
static struct PendingPurge g_pending[TIMER_SRV_MAX_PENDING];
static unsigned long g_pending_count;
static uint32_t g_epoch = 1;
static uint64_t g_my_id;
static uint64_t g_last_ns;
static uint64_t g_ns_per_tick;
static uint64_t g_anchor_ns;
static uint64_t g_anchor_mono_ns;
static uint8_t g_flags;
static uint64_t g_rewind_events;
static uint64_t g_purge_drops;

/* Flag bits (mirror kernel kTimerFlag*). */
#define TMR_FLAG_DEGRADED 0x01
#define TMR_FLAG_RTC_NEVER 0x02
#define TMR_FLAG_STALE_TOKEN 0x04

static uint64_t tmr_sat_add(uint64_t left, uint64_t right) {
    if (0xFFFFFFFFFFFFFFFFULL - left < right)
        return 0xFFFFFFFFFFFFFFFFULL;
    return left + right;
}

/* Monotone now (INV-T4): CLOCK_MONOTONIC only, latched max. A backward
 * sample holds the latch and counts (never acted on). */
static uint64_t tmr_now_ns(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return g_last_ns;
    uint64_t sample = (uint64_t)ts.tv_sec * 1000000000ULL +
                      (uint64_t)ts.tv_nsec;
    if (sample < g_last_ns) {
        ++g_rewind_events;
        return g_last_ns;
    }
    g_last_ns = sample;
    return sample;
}

static uint64_t tmr_timer_id(unsigned long index, unsigned long gen) {
    return ((uint64_t)gen << 32) | (uint64_t)index;
}

static unsigned long tmr_id_index(uint64_t timer_id) {
    return (unsigned long)(timer_id & 0xFFFFFFFFULL);
}

static unsigned long tmr_id_gen(uint64_t timer_id) {
    return (unsigned long)((timer_id >> 32) & 0xFFFFFFFFULL);
}

static unsigned long tmr_bump_gen(unsigned long gen) {
    unsigned long next = gen + 1;
    if (next == 0)
        ++next;
    return next;
}

/* Purge every slot owned by (owner_id, owner_gen); bump slot gens so
 * in-flight events die at the kernel drop site (INV-T5). */
static void tmr_purge_owner(uint32_t owner_id, uint32_t owner_gen) {
    int i;
    for (i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
        if (g_timers[i].used && g_timers[i].owner_id == owner_id &&
            g_timers[i].owner_gen == owner_gen) {
            g_timers[i].used = 0;
            g_timers[i].slot_gen = tmr_bump_gen(g_timers[i].slot_gen);
        }
    }
    for (i = 0; i < TIMER_SRV_MAX_TIMERFDS; ++i) {
        if (g_timerfds[i].used && g_timerfds[i].owner_id == owner_id &&
            g_timerfds[i].owner_gen == owner_gen) {
            g_timerfds[i].used = 0;
            g_timerfds[i].slot_gen = tmr_bump_gen(g_timerfds[i].slot_gen);
        }
    }
    for (i = 0; i < TIMER_SRV_MAX_SLEEPS; ++i) {
        if (g_sleeps[i].used && g_sleeps[i].owner_id == owner_id &&
            g_sleeps[i].owner_gen == owner_gen) {
            g_sleeps[i].used = 0;
            g_sleeps[i].slot_gen = tmr_bump_gen(g_sleeps[i].slot_gen);
        }
    }
}

static void tmr_purge_all(void) {
    int i;
    for (i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
        g_timers[i].used = 0;
        g_timers[i].slot_gen = tmr_bump_gen(g_timers[i].slot_gen);
    }
    for (i = 0; i < TIMER_SRV_MAX_TIMERFDS; ++i) {
        g_timerfds[i].used = 0;
        g_timerfds[i].slot_gen = tmr_bump_gen(g_timerfds[i].slot_gen);
    }
    for (i = 0; i < TIMER_SRV_MAX_SLEEPS; ++i) {
        g_sleeps[i].used = 0;
        g_sleeps[i].slot_gen = tmr_bump_gen(g_sleeps[i].slot_gen);
    }
    g_pending_count = 0;
}

/* Queue a dead-owner purge for the next fire_due sweep (send-failure
 * discovery path). Drop-newest past the bound (documented, counted). */
static void tmr_purge_later(uint32_t owner_id, uint32_t owner_gen) {
    if (g_pending_count >= TIMER_SRV_MAX_PENDING) {
        ++g_purge_drops;
        return;
    }
    g_pending[g_pending_count].owner_id = owner_id;
    g_pending[g_pending_count].owner_gen = owner_gen;
    ++g_pending_count;
}

static void tmr_send_fire(uint32_t owner_id, uint32_t owner_gen, uint8_t op,
                          uint64_t timer_id, uint64_t value_ns, int status,
                          uint8_t flags) {
    struct TimerRpc rep;
    memset(&rep, 0, sizeof(rep));
    rep.op = op;
    rep.flags = flags;
    rep.epoch = g_epoch;
    rep.owner_id = owner_id;
    rep.owner_gen = owner_gen;
    rep.timer_id = timer_id;
    rep.value_ns = value_ns;
    rep.status = (int16_t)status;
    int rc = ipc_send(owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    if (rc != 0) {
        /* Dead owner discovered on the send path: sweep next pass. */
        tmr_purge_later(owner_id, owner_gen);
    }
    (void)rc;
}

/* Earliest armed deadline in the server ns domain (0 = none). */
static uint64_t tmr_next_deadline(void) {
    uint64_t best = 0;
    int i;
    for (i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
        if (!g_timers[i].used)
            continue;
        if (best == 0 || g_timers[i].deadline_ns < best)
            best = g_timers[i].deadline_ns;
    }
    for (i = 0; i < TIMER_SRV_MAX_TIMERFDS; ++i) {
        if (!g_timerfds[i].used)
            continue;
        if (best == 0 || g_timerfds[i].deadline_ns < best)
            best = g_timerfds[i].deadline_ns;
    }
    for (i = 0; i < TIMER_SRV_MAX_SLEEPS; ++i) {
        if (!g_sleeps[i].used)
            continue;
        if (best == 0 || g_sleeps[i].deadline_ns < best)
            best = g_sleeps[i].deadline_ns;
    }
    return best;
}

/* Fire-due pass: sweep pending purges first (linger <= one wake
 * interval), then emit due events. Never burst-catch-up: a periodic
 * slot advances one period per pass; when still past due the deadline
 * skips forward to now + interval (missed periods collapse). */
static void tmr_fire_due(uint64_t now) {
    unsigned long p;
    int i;
    for (p = 0; p < g_pending_count; ++p)
        tmr_purge_owner(g_pending[p].owner_id, g_pending[p].owner_gen);
    g_pending_count = 0;
    for (i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
        struct TimerSlot *slot = &g_timers[i];
        uint64_t token;
        if (!slot->used || slot->deadline_ns > now)
            continue;
        token = tmr_timer_id((unsigned long)i, slot->slot_gen);
        if (slot->interval_ns == 0) {
            slot->used = 0;
            slot->slot_gen = tmr_bump_gen(slot->slot_gen);
        } else {
            uint64_t next = tmr_sat_add(slot->deadline_ns, slot->interval_ns);
            if (next <= now)
                next = tmr_sat_add(now, slot->interval_ns);
            slot->deadline_ns = next;
            if (slot->expiries < 0xFFFFFFFFFFFFFFFFULL)
                ++slot->expiries;
        }
    }
    for (i = 0; i < TIMER_SRV_MAX_TIMERFDS; ++i) {
        struct TimerSlot *slot = &g_timerfds[i];
        uint64_t token;
        uint64_t count;
        if (!slot->used || slot->deadline_ns > now)
            continue;
        token = tmr_timer_id((unsigned long)i, slot->slot_gen);
        if (slot->expiries < 0xFFFFFFFFFFFFFFFFULL)
            ++slot->expiries;
        /* Deliver-and-clear: each expiry is delivered exactly once;
         * periods that pass between reads arrive as one fire per
         * period (counts split across reads, never lost, never doubled). */
        count = slot->expiries;
        slot->expiries = 0;
        if (slot->interval_ns == 0) {
            slot->used = 0;
            slot->slot_gen = tmr_bump_gen(slot->slot_gen);
        } else {
            uint64_t next = tmr_sat_add(slot->deadline_ns, slot->interval_ns);
            if (next <= now)
                next = tmr_sat_add(now, slot->interval_ns);
            slot->deadline_ns = next;
        }
        tmr_send_fire(slot->owner_id, slot->owner_gen,
                      TMR_OP_TIMERFD_EXPIRED, token, count, 0, g_flags);
    }
    for (i = 0; i < TIMER_SRV_MAX_SLEEPS; ++i) {
        struct SleepSlot *slot = &g_sleeps[i];
        if (!slot->used || slot->deadline_ns > now)
            continue;
        slot->used = 0;
        slot->slot_gen = tmr_bump_gen(slot->slot_gen);
        tmr_send_fire(slot->owner_id, slot->owner_gen, TMR_OP_SLEEP_REQUEST,
                      slot->token, 0, 0, g_flags);
    }
}

/* Boot tick-rate calibration: count recv_timeout(1) iterations until the
 * tick counter advances 32 ticks (bounded 64 iterations, no division by
 * zero — a stuck counter falls back to 1 ms/tick + DEGRADED). An early
 * message breaks the loop and is stashed for dispatch (never dropped). */
static unsigned long g_cal_saved;
static unsigned char g_cal_buf[64];
static int g_cal_have;

static void tmr_calibrate(void) {
    unsigned long t0;
    unsigned long elapsed = 0;
    unsigned long it;
    uint64_t ns0 = tmr_now_ns();
    g_ns_per_tick = 1000000ULL;
    g_flags |= TMR_FLAG_DEGRADED;
    t0 = (unsigned long)__syscall5(5, 0, 0, 0, 0);
    for (it = 0; it < 64; ++it) {
        static unsigned char rbuf[64];
        int rc = ipc_recv_timeout(rbuf, sizeof(rbuf), 1);
        unsigned long now_ticks = (unsigned long)__syscall5(5, 0, 0, 0, 0);
        if (rc >= 0) {
            memcpy(g_cal_buf, rbuf, sizeof(g_cal_buf));
            g_cal_saved = (unsigned long)rc;
            g_cal_have = 1;
            break;
        }
        if (now_ticks < t0)
            break;
        elapsed = now_ticks - t0;
        if (elapsed >= 32)
            break;
    }
    if (elapsed > 0) {
        uint64_t ns1 = tmr_now_ns();
        uint64_t delta = (ns1 >= ns0) ? (ns1 - ns0) : 0;
        /* Overflow-safe ceil (mirrors the kernel decomposition): never 0. */
        uint64_t npt = (delta / elapsed) + ((delta % elapsed) != 0);
        if (npt == 0)
            npt = 1000000ULL;
        g_ns_per_tick = npt;
        g_flags &= (uint8_t)~TMR_FLAG_DEGRADED;
    }
}

int main(void) {
    tmr_assert_shape();
    g_my_id = (uint64_t)getpid();
    {
        struct TimerSlot *s;
        int i;
        for (i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
            s = &g_timers[i];
            s->used = 0;
            s->slot_gen = 1;
        }
        for (i = 0; i < TIMER_SRV_MAX_TIMERFDS; ++i) {
            s = &g_timerfds[i];
            s->used = 0;
            s->slot_gen = 1;
        }
        for (i = 0; i < TIMER_SRV_MAX_SLEEPS; ++i) {
            g_sleeps[i].used = 0;
            g_sleeps[i].slot_gen = 1;
        }
    }
    tmr_calibrate();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        /* S1: fire-due FIRST on every wake (including spurious), then
         * compute the wait. A live deadline never yields timeout 0
         * (block-forever); idle (no deadline) blocks forever and is
         * purely message-woken. */
        uint64_t now = tmr_now_ns();
        uint64_t deadline;
        uint64_t wait_ticks;
        int rc;
        tmr_fire_due(now);
        now = tmr_now_ns();
        deadline = tmr_next_deadline();
        if (deadline == 0) {
            wait_ticks = 0;
        } else {
            uint64_t npt = g_ns_per_tick ? g_ns_per_tick : 1000000ULL;
            uint64_t delta = (deadline > now) ? (deadline - now) : 0;
            uint64_t ticks = (delta / npt) + ((delta % npt) != 0);
            if (ticks < 1)
                ticks = 1;
            if (ticks > TMR_MAX_WAIT_TICKS)
                ticks = TMR_MAX_WAIT_TICKS;
            wait_ticks = ticks;
        }
        if (g_cal_have) {
            rc = (int)g_cal_saved;
            memcpy(rbuf, g_cal_buf, sizeof(rbuf));
            g_cal_have = 0;
        } else {
            rc = ipc_recv_timeout(rbuf, sizeof(rbuf), wait_ticks);
        }
        now = tmr_now_ns();
        tmr_fire_due(now);
        if (rc < 0)
            continue; /* fail-closed: no reply on transport error */
        {
            struct TimerRpc req;
            struct TimerRpc rep;
            memset(&req, 0, sizeof(req));
            {
                unsigned long copy =
                    sizeof(req) < 48 ? sizeof(req) : 48;
                memcpy(&req, rbuf, copy);
            }
            if ((unsigned long)rc != req.op)
                continue; /* torn/stale payload: type must match body op */
            if (req.epoch > g_epoch) {
                /* Epoch increase: purge everything (INV-T6). */
                tmr_purge_all();
                g_epoch = req.epoch;
            }
            memset(&rep, 0, sizeof(rep));
            rep.op = req.op;
            rep.epoch = g_epoch;
            rep.owner_id = req.owner_id;
            rep.owner_gen = req.owner_gen;
            rep.timer_id = req.timer_id;
            rep.status = 0;
            rep.flags = g_flags;
            switch (req.op) {
            case TMR_OP_BIND:
                if (req.value_ns != TMR_WIRE_VERSION) {
                    rep.status = -(int16_t)TMR_EPROTO;
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            case TMR_OP_CLOCK_GETTIME:
                if (req.clock_id == 1) {
                    rep.value_ns = tmr_now_ns();
                    rep.interval_ns = g_last_ns;
                } else if (req.clock_id == 0) {
                    if (g_anchor_ns == 0 &&
                        !(g_flags & TMR_FLAG_DEGRADED)) {
                        rep.value_ns = tmr_now_ns();
                        rep.flags =
                            (uint8_t)(g_flags | TMR_FLAG_DEGRADED);
                    } else {
                        uint64_t mono = tmr_now_ns();
                        uint64_t delta = (mono >= g_anchor_mono_ns)
                                             ? (mono - g_anchor_mono_ns)
                                             : 0;
                        rep.value_ns =
                            tmr_sat_add(g_anchor_ns, delta);
                        rep.interval_ns = g_anchor_mono_ns;
                    }
                } else {
                    rep.status = -(int16_t)TMR_EINVAL;
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            case TMR_OP_TIMER_CREATE: {
                int i;
                int free_idx = -1;
                if (req.clock_id != 1) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep,
                             sizeof(rep), 0);
                    break;
                }
                for (i = 0; i < TIMER_SRV_MAX_TIMERS; ++i) {
                    if (!g_timers[i].used) {
                        free_idx = i;
                        break;
                    }
                }
                if (free_idx < 0) {
                    rep.status = -(int16_t)TMR_EAGAIN;
                } else {
                    struct TimerSlot *slot = &g_timers[free_idx];
                    slot->used = 1;
                    slot->owner_id = req.owner_id;
                    slot->owner_gen = req.owner_gen;
                    slot->deadline_ns = 0;
                    slot->interval_ns = 0;
                    slot->expiries = 0;
                    rep.timer_id = tmr_timer_id((unsigned long)free_idx,
                                                slot->slot_gen);
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
            case TMR_OP_TIMER_SETTIME: {
                unsigned long idx = tmr_id_index(req.timer_id);
                unsigned long gen = tmr_id_gen(req.timer_id);
                struct TimerSlot *slot;
                if (idx >= TIMER_SRV_MAX_TIMERS) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep,
                             sizeof(rep), 0);
                    break;
                }
                slot = &g_timers[idx];
                if (!slot->used || slot->slot_gen != gen ||
                    slot->owner_id != req.owner_id ||
                    slot->owner_gen != req.owner_gen) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    rep.flags |= TMR_FLAG_STALE_TOKEN;
                    ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep,
                             sizeof(rep), 0);
                    break;
                }
                if (req.value_ns == 0) {
                    /* Disarm: no gen bump (the opaque handle stays
                     * valid for re-arm; plain timers have no async
                     * in-flight events). */
                    slot->deadline_ns = 0;
                    slot->interval_ns = 0;
                } else {
                    uint64_t mono = tmr_now_ns();
                    slot->deadline_ns =
                        tmr_sat_add(mono, req.value_ns);
                    slot->interval_ns = req.interval_ns;
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
            case TMR_OP_TIMER_GETTIME: {
                unsigned long idx = tmr_id_index(req.timer_id);
                unsigned long gen = tmr_id_gen(req.timer_id);
                struct TimerSlot *slot;
                if (idx >= TIMER_SRV_MAX_TIMERS) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep,
                             sizeof(rep), 0);
                    break;
                }
                slot = &g_timers[idx];
                if (!slot->used || slot->slot_gen != gen ||
                    slot->owner_id != req.owner_id ||
                    slot->owner_gen != req.owner_gen) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    rep.flags |= TMR_FLAG_STALE_TOKEN;
                } else if (slot->deadline_ns == 0) {
                    rep.value_ns = 0;
                    rep.interval_ns = 0;
                } else {
                    uint64_t mono = tmr_now_ns();
                    rep.value_ns = (slot->deadline_ns > mono)
                                       ? (slot->deadline_ns - mono)
                                       : 0;
                    rep.interval_ns = slot->interval_ns;
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
            case TMR_OP_TIMER_DELETE: {
                unsigned long idx = tmr_id_index(req.timer_id);
                unsigned long gen = tmr_id_gen(req.timer_id);
                struct TimerSlot *slot;
                if (idx >= TIMER_SRV_MAX_TIMERS) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep,
                             sizeof(rep), 0);
                    break;
                }
                slot = &g_timers[idx];
                if (!slot->used || slot->slot_gen != gen ||
                    slot->owner_id != req.owner_id ||
                    slot->owner_gen != req.owner_gen) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    rep.flags |= TMR_FLAG_STALE_TOKEN;
                } else {
                    slot->used = 0;
                    slot->slot_gen = tmr_bump_gen(slot->slot_gen);
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
            case TMR_OP_TIMERFD_CREATE: {
                int i;
                int free_idx = -1;
                for (i = 0; i < TIMER_SRV_MAX_TIMERFDS; ++i) {
                    if (!g_timerfds[i].used) {
                        free_idx = i;
                        break;
                    }
                }
                if (free_idx < 0) {
                    rep.status = -(int16_t)TMR_EAGAIN;
                } else {
                    struct TimerSlot *slot = &g_timerfds[free_idx];
                    slot->used = 1;
                    slot->owner_id = req.owner_id;
                    slot->owner_gen = req.owner_gen;
                    slot->deadline_ns = 0;
                    slot->interval_ns = 0;
                    slot->expiries = 0;
                    rep.timer_id = tmr_timer_id((unsigned long)free_idx,
                                                slot->slot_gen);
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
            case TMR_OP_TIMERFD_SETTIME: {
                unsigned long idx = tmr_id_index(req.timer_id);
                unsigned long gen = tmr_id_gen(req.timer_id);
                struct TimerSlot *slot;
                if (idx >= TIMER_SRV_MAX_TIMERFDS) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep,
                             sizeof(rep), 0);
                    break;
                }
                slot = &g_timerfds[idx];
                if (!slot->used || slot->slot_gen != gen ||
                    slot->owner_id != req.owner_id ||
                    slot->owner_gen != req.owner_gen) {
                    rep.status = -(int16_t)TMR_EINVAL;
                    rep.flags |= TMR_FLAG_STALE_TOKEN;
                    ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep,
                             sizeof(rep), 0);
                    break;
                }
                if (req.value_ns == 0) {
                    /* Disarm + STALE_TOKEN cancel marker: bump the gen
                     * so in-flight TIMERFD_EXPIRED events mismatch the
                     * kernel tag and die at the drop site (INV-T5). The
                     * new token rides home in the reply; the kernel
                     * re-tags the vnode before re-arming. */
                    slot->deadline_ns = 0;
                    slot->interval_ns = 0;
                    slot->slot_gen = tmr_bump_gen(slot->slot_gen);
                    rep.timer_id = tmr_timer_id(idx, slot->slot_gen);
                    rep.flags |= TMR_FLAG_STALE_TOKEN;
                } else {
                    uint64_t mono = tmr_now_ns();
                    slot->deadline_ns =
                        tmr_sat_add(mono, req.value_ns);
                    slot->interval_ns = req.interval_ns;
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
            case TMR_OP_CANCEL_OWNERS:
                /* Explicit tombstone path: purge synchronously. No reply
                 * (the kernel drains one-way; a reply would litter its
                 * queue with unconsumable messages). */
                tmr_purge_owner(req.owner_id, req.owner_gen);
                break;
            case TMR_OP_ANCHOR_PUSH:
                if (req.value_ns == 0 && req.interval_ns == 0) {
                    /* Kernel poll: answer with the current anchor, latch
                     * nothing. The kernel refreshes its cell from this
                     * reply (poll-on-stale; wall-clock reads stay
                     * wait-free). */
                    rep.value_ns = g_anchor_ns;
                    rep.interval_ns = g_anchor_mono_ns;
                    rep.flags = g_flags;
                } else {
                    /* rtc-pd feed: latch wall + mono, clear DEGRADED
                     * (unless the PD reports RTC_NEVER: the device is
                     * absent, so the anchor stays tick-derived). */
                    if (req.flags & TMR_FLAG_RTC_NEVER) {
                        g_anchor_ns = tmr_now_ns();
                        g_anchor_mono_ns = g_anchor_ns;
                        g_flags |=
                            (TMR_FLAG_DEGRADED | TMR_FLAG_RTC_NEVER);
                    } else {
                        g_anchor_ns = req.value_ns;
                        g_anchor_mono_ns = (req.interval_ns != 0)
                                               ? req.interval_ns
                                               : tmr_now_ns();
                        g_flags &= (uint8_t)~(TMR_FLAG_DEGRADED |
                                              TMR_FLAG_RTC_NEVER);
                    }
                    rep.value_ns = g_anchor_ns;
                    rep.interval_ns = g_anchor_mono_ns;
                    rep.flags = g_flags;
                }
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            case TMR_OP_SLEEP_REQUEST: {
                int i;
                int free_idx = -1;
                for (i = 0; i < TIMER_SRV_MAX_SLEEPS; ++i) {
                    if (!g_sleeps[i].used) {
                        free_idx = i;
                        break;
                    }
                }
                if (free_idx < 0) {
                    /* Table-full: fail-closed without hanging — answer
                     * the waiter immediately with -EAGAIN (the kernel
                     * propagates the fire status, never parks past it). */
                    tmr_send_fire(req.owner_id, req.owner_gen,
                                  TMR_OP_SLEEP_REQUEST, req.timer_id, 0,
                                  -(int)TMR_EAGAIN, g_flags);
                    break;
                }
                {
                    struct SleepSlot *slot = &g_sleeps[free_idx];
                    uint64_t mono = tmr_now_ns();
                    slot->used = 1;
                    slot->owner_id = req.owner_id;
                    slot->owner_gen = req.owner_gen;
                    slot->token = req.timer_id;
                    slot->deadline_ns =
                        tmr_sat_add(mono, req.value_ns);
                }
                /* No reply: the client is already blocked; the fire
                 * event at expiry is the answer. */
                break;
            }
            case TMR_OP_SLEEP_CANCEL: {
                int i;
                int found = 0;
                for (i = 0; i < TIMER_SRV_MAX_SLEEPS; ++i) {
                    if (g_sleeps[i].used &&
                        g_sleeps[i].owner_id == req.owner_id &&
                        g_sleeps[i].owner_gen == req.owner_gen &&
                        g_sleeps[i].token == req.timer_id) {
                        g_sleeps[i].used = 0;
                        g_sleeps[i].slot_gen =
                            tmr_bump_gen(g_sleeps[i].slot_gen);
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    rep.status = -(int16_t)TMR_EINVAL;
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
            default:
                rep.status = -(int16_t)TMR_ENOSYS;
                ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                         0);
                break;
            }
        }
    }
    return 0;
}
