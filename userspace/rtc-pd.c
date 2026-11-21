/* Driver PD: rtc (issue #48 strangler step a, #194 flip).
 *
 * BIND conveys the composed RTC cap (arg0 = encoded handle) plus the
 * timer-srv rendezvous (arg1 = server PID, 0 = absent; arg2 = timer wire
 * version, must be 2). The PD maps its device once (idempotent re-BIND),
 * then feeds the server: a TimerRpc ANCHOR_PUSH (op 8, value_ns = wall
 * ns, interval_ns = mono ns at read, flags = RTC_NEVER when the device
 * is absent) sent to the server PID. DRV_OP_RTC_EPOCH re-triggers the
 * push (restart resync); DRV_OP_RTC_READ serves a polled wall-clock
 * read (no in-tree caller in increment 1 — kernel keeps its RTC HAL
 * until the flip).
 *
 * Per-arch reads port the kernel HAL semantics (hal/rtc.hpp + arch
 * rtc.cpp): x86 CMOS 0x70/0x71 with update-in-progress wait + BCD +
 * century; aarch64 PL031 RTCDR; riscv goldfish TIME latch with the
 * plausibility floor, mtime fallback refused (RTC_NEVER degrade instead
 * of a 1970-epoch lie — BRIEFING gotcha #22).
 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <syscall.h>
#include "driver_pd.h"

#define RTC_WIRE_VERSION 2
#define RTC_EINVAL 22
#define RTC_ENOSYS 38
#define RTC_ENODEV 19

/* TimerRpc wire (feed to timer-srv; layout mirrors timer_srv.hpp). */
#define TMR_OP_ANCHOR_PUSH 8
#define TMR_FLAG_DEGRADED 0x01
#define TMR_FLAG_RTC_NEVER 0x02

struct TmrFeed {
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

static void rtc_assert_shape(void) {
    char check_drv[sizeof(struct DriverRpc) <= 48 ? 1 : -1];
    (void)check_drv;
    char check_feed[sizeof(struct TmrFeed) <= 48 ? 1 : -1];
    (void)check_feed;
}

static uint64_t g_my_id;
static unsigned long g_cap_handle;
static unsigned long g_server_pid;
static int g_mapped;
static int g_rtc_never; /* latched device absence (riscv no-goldfish) */

#if defined(__x86_64__)
#define RTC_CMOS_INDEX 0x70
#define RTC_CMOS_DATA 0x71
#define RTC_POLL_BOUND 100000

static inline void rtc_outb(unsigned long port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"((uint16_t)port));
}

static inline unsigned char rtc_inb(unsigned long port) {
    unsigned char val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)port));
    return val;
}

static unsigned char rtc_reg(unsigned char reg) {
    rtc_outb(RTC_CMOS_INDEX, (unsigned char)(reg | 0x80));
    return rtc_inb(RTC_CMOS_DATA);
}

static unsigned char rtc_bcd_bin(unsigned char bcd) {
    return (unsigned char)((bcd & 0x0F) + ((bcd >> 4) * 10));
}

/* Wait for update-in-progress to clear (bounded; fail-closed 0). */
static int rtc_wait_update(void) {
    unsigned long bound = RTC_POLL_BOUND;
    while (rtc_reg(0x0A) & 0x80) {
        if (--bound == 0)
            return -1;
    }
    return 0;
}

static int rtc_days_month(int year, int month) {
    static const unsigned char dim[12] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    int days = dim[month - 1];
    if (month == 2) {
        int leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
        if (leap)
            days = 29;
    }
    return days;
}

/* CMOS seconds since epoch (0 = invalid read). */
static uint64_t rtc_read_seconds(void) {
    unsigned char sec;
    unsigned char min;
    unsigned char hour;
    unsigned char day;
    unsigned char month;
    unsigned char year;
    unsigned char century;
    unsigned char status_b;
    unsigned int full_year;
    unsigned long days = 0;
    int y;
    int m;
    if (rtc_wait_update() != 0)
        return 0;
    sec = rtc_reg(0x00);
    min = rtc_reg(0x02);
    hour = rtc_reg(0x04);
    day = rtc_reg(0x07);
    month = rtc_reg(0x08);
    year = rtc_reg(0x09);
    century = rtc_reg(0x32);
    if (rtc_wait_update() != 0)
        return 0;
    /* Second read guards rollover (same-as-kernel: use the second set). */
    sec = rtc_reg(0x00);
    min = rtc_reg(0x02);
    hour = rtc_reg(0x04);
    day = rtc_reg(0x07);
    month = rtc_reg(0x08);
    year = rtc_reg(0x09);
    status_b = rtc_reg(0x0B);
    if (!(status_b & 0x04)) {
        sec = rtc_bcd_bin(sec);
        min = rtc_bcd_bin(min);
        hour = rtc_bcd_bin(hour);
        day = rtc_bcd_bin(day);
        month = rtc_bcd_bin(month);
        year = rtc_bcd_bin(year);
        century = rtc_bcd_bin(century);
    }
    full_year = (unsigned int)century * 100 + (unsigned int)year;
    if (full_year < 1970 || month == 0 || month > 12 || day == 0 ||
        day > 31 || hour > 23 || min > 59 || sec > 59)
        return 0;
    if ((int)day > rtc_days_month((int)full_year, (int)month))
        return 0;
    for (y = 1970; (unsigned int)y < full_year; ++y) {
        days += 365;
        if ((y % 4 == 0) && (y % 100 != 0 || y % 400 == 0))
            ++days;
    }
    for (m = 1; m < (int)month; ++m)
        days += (unsigned long)rtc_days_month((int)full_year, m);
    days += (unsigned long)(day - 1);
    return days * 86400ULL + (uint64_t)hour * 3600ULL +
           (uint64_t)min * 60ULL + (uint64_t)sec;
}
#elif defined(__aarch64__)
/* PL031 (QEMU virt 0x09010000, spec-fixed like device_regions.hpp). */
#define RTC_PL031_BASE 0x09010000UL
#define RTC_PL031_DATA 0x00
#define RTC_PL031_PID0 0xFE0
#define RTC_PL031_PID1 0xFE4
#define RTC_PL031_PID2 0xFE8
#define RTC_PL031_PID3 0xFEC

static unsigned long g_rtc_va;

static uint64_t rtc_read_seconds(void) {
    volatile unsigned int *rtc;
    if (!g_mapped)
        return 0;
    rtc = (volatile unsigned int *)g_rtc_va;
    return (uint64_t)rtc[RTC_PL031_DATA / 4];
}
#else
/* riscv64 goldfish (0x101000, spec-fixed). Latch HIGH/LOW/HIGH. */
#define RTC_GOLDFISH_BASE 0x101000UL
#define RTC_GOLDFISH_LOW 0x00
#define RTC_GOLDFISH_HIGH 0x04
#define RTC_GOLDFISH_TRIES 8
/* Plausibility floor (2001-09-09, mirrors the kernel): rejects
 * absent/zero/all-ones reads. */
#define RTC_GOLDFISH_MIN_PLAUSIBLE_NS 1000000000ULL

static unsigned long g_rtc_va;

static uint64_t rtc_read_goldfish_ns(void) {
    volatile unsigned int *base;
    int attempt;
    if (!g_rtc_va)
        return 0;
    base = (volatile unsigned int *)g_rtc_va;
    for (attempt = 0; attempt < RTC_GOLDFISH_TRIES; ++attempt) {
        uint32_t hi_first = base[RTC_GOLDFISH_HIGH / 4];
        uint32_t lo = base[RTC_GOLDFISH_LOW / 4];
        uint32_t hi_second = base[RTC_GOLDFISH_HIGH / 4];
        if (hi_first == hi_second)
            return ((uint64_t)hi_first << 32) | lo;
    }
    return 0;
}

static uint64_t rtc_read_seconds(void) {
    uint64_t nanos = rtc_read_goldfish_ns();
    if (nanos < RTC_GOLDFISH_MIN_PLAUSIBLE_NS * 1000000000ULL)
        return 0;
    return nanos / 1000000000ULL;
}
#endif

static uint64_t rtc_mono_ns(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Map the device through the BIND-conveyed cap (idempotent re-BIND). */
static int rtc_map(unsigned long handle) {
    if (g_mapped && handle == g_cap_handle)
        return 0;
    if (g_mapped)
        return -1;
#if defined(__x86_64__)
    {
        long rc = __syscall5(SYS_IOPORT_GRANT, (long)handle, 0x70, 2, 0);
        if (rc != 0)
            return -1;
    }
#else
    {
        long va = __syscall5(SYS_MMIO_MAP, (long)handle, 0, 0, 0);
        if (va == 0 || va == -1)
            return -1;
        g_rtc_va = (unsigned long)va;
    }
#endif
    g_cap_handle = handle;
    g_mapped = 1;
    return 0;
}

/* Push the epoch feed to the server (TimerRpc ANCHOR_PUSH). Fail-closed:
 * no server PID, or an unmapped/absent device, degrades (the server
 * stays tick-derived; the kernel keeps its boot fallback). */
static void rtc_push_feed(void) {
    struct TmrFeed feed;
    uint64_t wall_ns = 0;
    uint64_t mono_ns;
    uint8_t flags = 0;
    static unsigned char ack[64];
    if (g_server_pid == 0)
        return;
    mono_ns = rtc_mono_ns();
    if (g_mapped && !g_rtc_never) {
        uint64_t secs = rtc_read_seconds();
#if !defined(__x86_64__) && !defined(__aarch64__)
        /* riscv: implausible/absent goldfish latches RTC_NEVER. */
        if (secs == 0)
            g_rtc_never = 1;
#endif
        if (secs != 0)
            wall_ns = secs * 1000000000ULL;
    }
    if (wall_ns == 0) {
        wall_ns = mono_ns;
        flags = (TMR_FLAG_DEGRADED | TMR_FLAG_RTC_NEVER);
    }
    memset(&feed, 0, sizeof(feed));
    feed.op = TMR_OP_ANCHOR_PUSH;
    feed.flags = flags;
    feed.epoch = 1;
    feed.owner_id = (uint32_t)g_my_id;
    feed.owner_gen = 0;
    feed.value_ns = wall_ns;
    feed.interval_ns = mono_ns;
    ipc_send(g_server_pid, TMR_OP_ANCHOR_PUSH, &feed, sizeof(feed), 0);
    /* Collect the server's reply (bounded; discard — the feed is one-way
     * by contract, the wait only keeps our own queue from growing). */
    (void)drv_recv(ack, sizeof(ack), 5000);
}

int main(void) {
    rtc_assert_shape();
    g_my_id = (uint64_t)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = drv_recv(rbuf, sizeof(rbuf), 0);
        struct DriverRpc req;
        struct DriverRpc rep;
        if (rc < 0)
            continue; /* fail-closed: no reply on transport error */
        memset(&req, 0, sizeof(req));
        {
            unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
            memcpy(&req, rbuf, copy);
        }
        if ((unsigned long)rc != req.op)
            continue; /* torn/stale payload: type must match body op */
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        switch (req.op) {
        case DRV_OP_BIND:
            if (req.arg2 != RTC_WIRE_VERSION) {
                rep.status = (uint16_t)-RTC_EINVAL;
                break;
            }
            if (rtc_map((unsigned long)req.arg0) != 0) {
                g_rtc_never = 1;
                rep.status = (uint16_t)-RTC_ENODEV;
                break;
            }
            /* Re-BIND re-probes: a previously absent device may exist
             * after a supervisor re-compose (never latch NEVER across
             * a fresh BIND). */
            g_rtc_never = 0;
            g_server_pid = (unsigned long)req.arg1;
            rep.arg0 = 1;
            rtc_push_feed();
            break;
        case DRV_OP_RTC_EPOCH:
            /* Restart/commanded resync: re-push the feed to the stored
             * server PID (fail-closed when unbound). */
            rtc_push_feed();
            rep.arg0 = (g_server_pid != 0) ? 1 : 0;
            break;
        case DRV_OP_RTC_READ: {
            uint64_t secs = 0;
            if (g_mapped && !g_rtc_never)
                secs = rtc_read_seconds();
            if (secs == 0) {
                rep.status = (uint16_t)-RTC_ENODEV;
            } else {
                rep.arg0 = secs;
            }
            break;
        }
        default:
            rep.status = (uint16_t)-RTC_ENOSYS;
            break;
        }
        /* Reply to the requester (owner_id carried in the RPC body):
         * send_sync_bounded waits on the REQUESTER's own queue, so a
         * reply to PID 1 (init) is a dead letter (issue #441). */
        ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
