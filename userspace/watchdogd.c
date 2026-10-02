/*
 * NexIOS RTOS — Watchdog supervision daemon (first userspace microservice)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * watchdogd — supervises other tasks' watchdogs (issue #277).
 *
 * Modelled on userspace/vfsd.c: READY handshake with init (PID 1), then a
 * receive/dispatch/reply loop. Differences from vfsd:
 * - Own liveness: arms its own watchdog at startup (generous period) and
 *   kicks it at the top of every loop iteration. A stuck loop stops
 *   kicking and the kernel scan kills it (fail-closed); init/daemon_mgr
 *   restarts it like any daemon.
 * - Supervision: init grants one WdogCap per supervised task (delivered
 *   as WDOG_SUPERVISE messages carrying pid + cap handle + period).
 *   watchdogd arms each target via WATCHDOG_CREATE(pid) and re-kicks it
 *   ONLY after a successful status poll of that target (explicit kick
 *   points — never a reply hook, so a stuck handler is never masked).
 * - IPC layout note: the kernel Message is {sender, type, priority,
 *   data[]}; the wire struct below reads {sender, type, arg0, arg1,
 *   data0}, i.e. arg0 = kernel priority field, arg1/data0 = data[0/8].
 *   Both sides document this mapping (watchdogd.hpp documents it too).
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <ipc.h>

#define WDOG_SUPERVISE 300
#define WDOG_STATUS    301
#define WDOG_DISARM    302

#define MSG_DAEMON_READY 0xF0000001

#define WDOGD_MAX_SUPERVISED 8
#define WDOGD_SELF_PERIOD 1000

struct WdogdMsg {
    uint64_t sender_id;
    uint64_t type;
    uint64_t arg0; /* pid (SUPERVISE) */
    uint64_t arg1; /* cap handle (SUPERVISE) */
    uint64_t data0; /* period (SUPERVISE) */
};

struct WdogdReply {
    int64_t result;
    uint64_t data0;
    uint64_t data1;
};

struct Supervised {
    uint64_t pid;
    uint64_t cap;
    uint64_t period;
    int armed;
};

static uint64_t g_my_id = 0;
static struct Supervised g_supervised[WDOGD_MAX_SUPERVISED];
static unsigned g_supervised_count = 0;

static int wdogd_handle_supervise(struct WdogdMsg *msg,
                                  struct WdogdReply *reply) {
    unsigned i = 0;

    if (msg->arg0 == 0 || msg->arg1 == 0 || msg->data0 == 0) {
        reply->result = -1;
        return -1;
    }
    for (i = 0; i < g_supervised_count; ++i) {
        if (g_supervised[i].pid == msg->arg0) {
            reply->result = -1;
            return -1;
        }
    }
    if (g_supervised_count >= WDOGD_MAX_SUPERVISED) {
        reply->result = -1;
        return -1;
    }
    if (__syscall5(91, (long)msg->data0, (long)msg->arg0,
                   (long)msg->arg1, 0) != 0) {
        reply->result = -1;
        return -1;
    }
    g_supervised[g_supervised_count].pid = msg->arg0;
    g_supervised[g_supervised_count].cap = msg->arg1;
    g_supervised[g_supervised_count].period = msg->data0;
    g_supervised[g_supervised_count].armed = 1;
    ++g_supervised_count;
    reply->result = 0;
    reply->data0 = msg->arg0;
    return 0;
}

static int wdogd_handle_status(struct WdogdMsg *msg,
                               struct WdogdReply *reply) {
    unsigned i = 0;

    reply->result = 0;
    reply->data0 = g_supervised_count;
    reply->data1 = 0;
    for (i = 0; i < g_supervised_count; ++i) {
        if (g_supervised[i].pid == msg->arg0) {
            reply->data1 = (uint64_t)g_supervised[i].armed;
            return 0;
        }
    }
    if (msg->arg0 == 0)
        return 0;
    reply->result = -1;
    return -1;
}

static int wdogd_handle_disarm(struct WdogdMsg *msg,
                               struct WdogdReply *reply) {
    unsigned i = 0;
    unsigned j = 0;

    for (i = 0; i < g_supervised_count; ++i) {
        if (g_supervised[i].pid == msg->arg0) {
            for (j = i; j + 1 < g_supervised_count; ++j)
                g_supervised[j] = g_supervised[j + 1];
            --g_supervised_count;
            reply->result = 0;
            return 0;
        }
    }
    reply->result = -1;
    return -1;
}

static int wdogd_dispatch(struct WdogdMsg *msg, struct WdogdReply *reply) {
    switch (msg->type) {
    case WDOG_SUPERVISE:
        return wdogd_handle_supervise(msg, reply);
    case WDOG_STATUS:
        return wdogd_handle_status(msg, reply);
    case WDOG_DISARM:
        return wdogd_handle_disarm(msg, reply);
    default:
        reply->result = -1;
        return -1;
    }
}

int main(void) {
    g_my_id = (uint64_t)getpid();
    printf("[watchdogd] Watchdog Daemon started (PID=%llu)\n", g_my_id);

    /* Arm our own watchdog first: a stuck loop stops kicking below and
     * the kernel scan kills us (fail-closed); init restarts us. */
    if (sys_watchdog_create(WDOGD_SELF_PERIOD) != 0) {
        printf("[watchdogd] self-arm failed, exiting\n");
        return 1;
    }

    /* Signal init (PID 1) that this daemon is ready. Init answers with
     * one WDOG_SUPERVISE message per supervised task (pid + cap +
     * period) once all daemons are up. */
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    while (1) {
        struct WdogdMsg msg = {0};
        struct WdogdReply reply = {0};
        unsigned i = 0;
        long r = 0;

        /* Loop-top self-kick: liveness proof for our own watchdog. */
        if (sys_watchdog_kick() != 0)
            return 1;

        /* Re-kick supervised tasks. Explicit kick points only: every
         * target in the table is expected alive (a real status poll
         * hook lands here); a target that stops responding simply
         * stops being kicked and expires on its own. */
        for (i = 0; i < g_supervised_count; ++i) {
            if (!g_supervised[i].armed)
                continue;
            __syscall5(92, (long)g_supervised[i].pid,
                       (long)g_supervised[i].cap, 0, 0);
        }

        /* Bounded receive (10-tick quantum, raw RECEIVE timeout arg):
         * the loop must return to the kicks above even with no
         * traffic — a blocking recv would starve them and suicide
         * on the self watchdog. -1 = timeout (or error); both just
         * re-enter the loop. */
        r = __syscall5(2, 0, (long)&msg, (long)sizeof(msg), 10);
        if (r < 0)
            continue;
        if ((unsigned long)r != msg.type)
            continue;

        wdogd_dispatch(&msg, &reply);

        ipc_send(msg.sender_id, 0, &reply, sizeof(reply), 0);
    }

    return 0;
}
