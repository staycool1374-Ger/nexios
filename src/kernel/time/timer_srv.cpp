#include <kernel/time/timer_srv.hpp>

#if CONFIG_TIMER_SRV

/*
 * NexIOS RTOS — Timer/Event externalization proxy (issue #241, #194 flip).
 * Whole translation unit is `#if CONFIG_TIMER_SRV`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#241 tree.
 *
 * Contract (INV-T1..T9, see audits/plan-194.md):
 * - Sync ops (create/settime/gettime/delete, timerfd create/settime,
 *   anchor poll, BIND) go through timer_srv_rpc (bounded, fail-closed).
 * - Nanosleep is an async pair: nsleep_send (one-way) + nsleep_wait
 *   (fire-event wait, §11.1/§11.2 shape, no kernel timer — INV-T8).
 * - Timerfd reads block in timerfd_deliver (INV-T5 tag-vs-token compare).
 * - Clock reads never block: MONOTONIC from the HRT clock, REALTIME from
 *   the lock-free anchor cell (server polls only while invalid).
 * - Signal-EINTR for recv-parked sleepers is a carried limitation (#194):
 *   the OFF leg probes pending_signals, but under ON the sleeper parks in
 *   IPC::recv with sleep_armed == false so delivery wakes cannot reach it.
 *   Server death still wakes with -EINTR.
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/driver/driver_pds.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/arch/hal/rtc.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/vfs/vfs.hpp>

namespace kernel::time {
namespace timer_srv {
namespace {

/// @brief Per-op sleep token source (opaque to the server, echoed in the
/// fire event; distinguishes consecutive sleeps of one task in one epoch).
uint64_t g_sleep_seq = 1;
/// @brief BIND completed this epoch (version gate passed).
bool g_bound = false;
/// @brief Cached BIND failure (surfaced as -EPROTO/-EAGAIN, never retried
/// past the epoch bump).
int64_t g_bind_status = 0;

uint64_t g_timer_srv_pid = 0;
uint32_t g_timer_srv_epoch = 1;

struct Tombstone {
    uint32_t owner_id;
    uint32_t owner_gen;
};

Tombstone g_tombstones[16] = {};
uint64_t g_tombstone_count = 0;
/// @brief Lifetime tombstone drops past the ring (§8.4/§10 D6 counter).
uint64_t g_tombstone_drops_total = 0;

/// @brief Bounded wait budget for one server RPC (coarse ticks).
/// Fail-closed `-EAGAIN` past this; never parks forever on a dead server.
constexpr uint64_t kRpcGuardTicks = 5000;

/// @brief Lock-free wall-clock anchor cell (INV-T9: __atomic only).
uint64_t g_anchor_ns = 0;
uint64_t g_anchor_mono_ns = 0;
uint8_t g_anchor_flags = 0;
bool g_anchor_valid = false;

/// @brief User ABI timespec (matches src/libc/time.h layout).
struct AbiTimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
static_assert(sizeof(AbiTimespec) == 16, "timespec width");

/// @brief Fill the invariant RPC fields from the calling task.
/// @return false when no current task (no owner identity available).
bool fill_owner(TimerRpc &req) {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    req.epoch = __atomic_load_n(&g_timer_srv_epoch, __ATOMIC_ACQUIRE);
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    return true;
}

/// @brief Validated-reply core (existing #241 shape): epoch + owner.
bool check_reply(const TimerRpc &req, const TimerRpc &reply) {
    return reply.epoch == timer_srv_epoch() &&
           reply.owner_id == req.owner_id &&
           reply.owner_gen == req.owner_gen;
}

/// @brief Single validated RPC round (no tombstone/bind/anchor side work).
bool rpc_once(const TimerRpc &req, TimerRpc &reply) {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0 || !timer_srv_alive()) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    Message msg{};
    msg.type = static_cast<uint64_t>(req.op);
    msg.priority = cur->priority;
    __builtin_memcpy(msg.data, &req, sizeof(req));
    msg.data_size = sizeof(req);
    Message raw{};
    if (!IPC::send_sync_bounded(pid, msg, raw, kRpcGuardTicks)) {
        return false;
    }
    if (raw.data_size != sizeof(reply)) {
        return false;
    }
    __builtin_memcpy(&reply, raw.data, sizeof(reply));
    // Reply identity: epoch + owner (id+gen). A mismatch is
    // dropped by the caller (bounded re-wait), never consumed.
    if (raw.sender_id != pid || !check_reply(req, reply)) {
        return false;
    }
    return true;
}

/// @brief Best-effort tombstone flush (piggyback path, INV-T6): one
/// CANCEL_OWNERS datagram per latched owner. One-way NONBLOCK sends —
/// the server sends no reply to CANCEL_OWNERS ("no reply expected"),
/// so a bounded wait would only burn kRpcGuardTicks per tombstone and
/// litter the client queue with unconsumable replies. Leftovers stay
/// ringed for the next proxy call (table-full stays fail-closed).
void drain_tombstones() {
    const uint64_t pending =
        __atomic_load_n(&g_tombstone_count, __ATOMIC_ACQUIRE);
    if (pending == 0) {
        return;
    }
    const uint64_t server_pid = get_timer_srv_pid();
    if (server_pid == 0 || !timer_srv_alive()) {
        return;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return;
    }
    for (uint64_t idx = 0; idx < pending && idx < 16; ++idx) {
        TimerRpc req{};
        req.op = static_cast<uint8_t>(TimerSrvOp::CANCEL_OWNERS);
        req.epoch = timer_srv_epoch();
        req.owner_id = g_tombstones[idx].owner_id;
        req.owner_gen = g_tombstones[idx].owner_gen;
        Message msg{};
        msg.type = static_cast<uint64_t>(req.op);
        msg.priority = cur->priority;
        __builtin_memcpy(msg.data, &req, sizeof(req));
        msg.data_size = sizeof(req);
        (void)IPC::send(server_pid, msg, IpcFlags::NONBLOCK);
    }
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELEASE);
}

/// @brief Lazy BIND (S2-4 version gate): the first proxy RPC per epoch
/// stamps kSrvWireVersion; a mismatched server answers -EPROTO and the
/// proxy refuses the epoch (fail-closed, never guesses the wire).
bool ensure_bound() {
    if (__atomic_load_n(&g_bound, __ATOMIC_ACQUIRE)) {
        return __atomic_load_n(&g_bind_status, __ATOMIC_ACQUIRE) == 0;
    }
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::BIND);
    req.value_ns = kSrvWireVersion;
    if (!fill_owner(req)) {
        return false;
    }
    TimerRpc reply{};
    if (!rpc_once(req, reply)) {
        return false;
    }
    if (reply.status != 0) {
        __atomic_store_n(&g_bind_status,
                         static_cast<int64_t>(reply.status),
                         __ATOMIC_RELEASE);
        return false;
    }
    if (reply.timer_id != req.timer_id) {
        __atomic_store_n(&g_bind_status, -kPosixErrAgain, __ATOMIC_RELEASE);
        return false;
    }
    __atomic_store_n(&g_bind_status, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_bound, true, __ATOMIC_RELEASE);
    // Feed rendezvous (issue #194 step 31): (re-)BIND rtc-pd with the
    // server PID, then command an epoch push so the server's anchor
    // resyncs after every epoch bump. Task context (syscall), bounded,
    // fail-closed — the server stays DEGRADED when the PD is absent.
    // Never from the tick path (restart hooks only invalidate the cell).
    (void)kernel::driver_pds::pd_bind_rtc();
    (void)kernel::driver_pds::pd_rtc_push_epoch();
    return true;
}

/// @brief Validate one queued server message as the awaited fire event.
/// Pure payload check (no locks): type + sender + epoch + owner + token.
bool is_fire_event(const Message &raw, uint64_t server_pid,
                   TimerSrvOp expect_op, uint64_t expect_token) {
    if (raw.type != kTimerSrvReplyType) {
        return false;
    }
    if (raw.sender_id != server_pid) {
        return false;
    }
    if (raw.data_size != sizeof(TimerRpc)) {
        return false;
    }
    TimerRpc event{};
    __builtin_memcpy(&event, raw.data, sizeof(event));
    if (event.op != static_cast<uint8_t>(expect_op)) {
        return false;
    }
    if (event.epoch != timer_srv_epoch()) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    if (event.owner_id != static_cast<uint32_t>(cur->id) ||
        event.owner_gen != cur->generation) {
        return false;
    }
    if (event.timer_id != expect_token) {
        return false;
    }
    // Identity only: a validated fire with status != 0 (e.g. server-side
    // sleep-table-full -EAGAIN) is a real answer, not a stray — the
    // status propagates to the waiter instead of hanging it.
    return true;
}

/// @brief Pop one server message and validate it as the awaited fire.
/// Non-matching traffic is re-queued untouched (never dropped: a stale
/// sync reply belongs to a future RPC retry, not to this waiter).
/// @return true with *out_count/*out_status filled on a validated fire.
bool drain_fire_once(uint64_t server_pid, TimerSrvOp expect_op,
                     uint64_t expect_token, uint64_t &out_count,
                     int64_t &out_status) {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    Message raw{};
    if (!cur->msg_queue.pop_from_sender(raw, server_pid)) {
        return false;
    }
    if (!is_fire_event(raw, server_pid, expect_op, expect_token)) {
        (void)cur->msg_queue.push(raw);
        return false;
    }
    TimerRpc event{};
    __builtin_memcpy(&event, raw.data, sizeof(event));
    out_count = event.value_ns;
    out_status = static_cast<int64_t>(event.status);
    return true;
}

/// @brief §11.1/§11.2 fire-event wait (INV-T8): no kernel timer, no locks
/// across the park. Wakers: (a) the server's expiry message (arrival wakes
/// via blocked_in_recv, never cleared by send), (b) server death (death-wake
/// via reply_wait tags → -EINTR). Stale traffic is re-queued, never eaten.
/// @return The fire event's status (0 or the server's negated errno).
int64_t wait_for_fire(uint64_t server_pid, TimerSrvOp expect_op,
                      uint64_t expect_token, uint64_t &out_count) {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return -kPosixErrAgain;
    }
    bool was_blocked = false;
    for (;;) {
        if (cur->state == TaskState::BLOCKED) {
            // INV-4b: a BLOCKED task takes no locks. Straight back to
            // reschedule + hlt-wait (send_sync_err shape).
            Scheduler::reschedule();
            arch::sti();
            arch::hlt();
            arch::cli();
            continue;
        }
        // Bounded drain: at most one queue-full of pops per wakeup; a
        // re-queued stale message ends the pass (the fire, if queued,
        // is found before the pass ends — no park with fire pending).
        for (uint32_t attempt = 0; attempt < IPC_MAX_QUEUE_MSG; ++attempt) {
            int64_t fire_status = 0;
            if (drain_fire_once(server_pid, expect_op, expect_token,
                                out_count, fire_status)) {
                cur->blocked_in_recv = false;
                __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_id, 0,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&cur->reply_wait_dest_gen, 0,
                                 __ATOMIC_RELEASE);
                if (was_blocked) {
                    cur->remaining_ticks = cur->period_ticks;
                    arch::sti(); // Issue #513: restore IRQ-on (slow path).
                }
                cur->state = TaskState::RUNNING;
                return fire_status;
            }
            if (!cur->msg_queue.has_from(server_pid)) {
                break;
            }
        }
        // Destination death: the fire will never come (INV-T8 waker b).
        // M1/F7 (§12.2): generation check gated on the armed wait.
        auto *dest = Scheduler::find_task(server_pid);
        const bool dest_gone =
            dest == nullptr || dest->state == TaskState::TERMINATED ||
            dest->state == TaskState::REAPED ||
            (was_blocked &&
             dest->generation !=
                 __atomic_load_n(&cur->reply_wait_dest_gen,
                                 __ATOMIC_ACQUIRE));
        if (dest_gone) {
            cur->blocked_in_recv = false;
            __atomic_store_n(&cur->reply_wait, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen, 0, __ATOMIC_RELEASE);
            if (was_blocked && cur->in_ready_queue_) {
                Scheduler::dequeue_ready(*cur);
            }
            cur->state = TaskState::RUNNING;
            if (was_blocked) {
                arch::sti(); // Issue #513: restore IRQ-on (slow path).
            }
            return -kPosixErrIntr;
        }
        // Publish both wait channels, then Dekker-recheck under the same
        // guard (sys_receive shape): an arrival between the drain pass
        // and the publish is consumed, never slept through. reply_wait
        // tags arm the death-wake; blocked_in_recv arms the arrival wake
        // (send never clears it, unlike reply_wait).
        bool late_hit = false;
        int64_t late_status = 0;
        {
            arch::IrqGuard publish_guard{};
            cur->blocked_in_recv = true;
            __atomic_store_n(&cur->reply_wait_dest_id, server_pid,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&cur->reply_wait_dest_gen,
                             dest != nullptr ? dest->generation : 0,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&cur->reply_wait, true, __ATOMIC_RELEASE);
            using QP = MessageQueue::QueueProbe;
            Message probe_raw{};
            const QP probe =
                cur->msg_queue.probe_pop_from_sender(probe_raw, server_pid);
            if (probe == QP::Consumed) {
                if (is_fire_event(probe_raw, server_pid, expect_op,
                                  expect_token)) {
                    TimerRpc event{};
                    __builtin_memcpy(&event, probe_raw.data, sizeof(event));
                    out_count = event.value_ns;
                    late_status = static_cast<int64_t>(event.status);
                    cur->blocked_in_recv = false;
                    __atomic_store_n(&cur->reply_wait, false,
                                     __ATOMIC_RELEASE);
                    __atomic_store_n(&cur->reply_wait_dest_id, 0,
                                     __ATOMIC_RELEASE);
                    __atomic_store_n(&cur->reply_wait_dest_gen, 0,
                                     __ATOMIC_RELEASE);
                    late_hit = true;
                } else {
                    (void)cur->msg_queue.push(probe_raw);
                    cur->state = TaskState::BLOCKED;
                    Scheduler::note_block_transition("timer_srv_recv_wait",
                                                     cur);
                    Scheduler::dequeue_ready(*cur);
                }
            } else {
                cur->state = TaskState::BLOCKED;
                Scheduler::note_block_transition("timer_srv_call_wait", cur);
                Scheduler::dequeue_ready(*cur);
            }
        }
        if (late_hit) {
            cur->remaining_ticks = cur->period_ticks;
            arch::sti();
            cur->state = TaskState::RUNNING;
            return late_status;
        }
        was_blocked = true;
        Scheduler::reschedule();
        // Issue #513 face A/C hang: unconditional yield (send_sync shape).
        arch::sti();
        arch::hlt();
        arch::cli();
    }
}

} // namespace

void set_timer_srv_pid(uint64_t pid) {
    __atomic_store_n(&g_timer_srv_pid, pid, __ATOMIC_RELAXED);
}

uint64_t get_timer_srv_pid() {
    return __atomic_load_n(&g_timer_srv_pid, __ATOMIC_RELAXED);
}

bool timer_srv_alive() {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0) {
        return false;
    }
    auto *srv = Scheduler::find_task(pid);
    if (srv == nullptr) {
        return false;
    }
    return srv->state != TaskState::TERMINATED &&
           srv->state != TaskState::REAPED;
}

uint32_t timer_srv_epoch() {
    return __atomic_load_n(&g_timer_srv_epoch, __ATOMIC_RELAXED);
}

void timer_srv_bump_epoch() {
    __atomic_fetch_add(&g_timer_srv_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bound, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_bind_status, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_anchor_valid, false, __ATOMIC_RELEASE);
}

void on_restart(uint64_t pid) {
    set_timer_srv_pid(pid);
    timer_srv_bump_epoch();
}

void note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
    // Lock-free ring write (teardown context: no locks, no IPC).
    // Drop-newest past 16 (code is truth; the spec matched in #194):
    // overflow is counted in g_tombstone_drops_total_ (§8.4/§10 D6).
    uint64_t count =
        __atomic_load_n(&g_tombstone_count, __ATOMIC_RELAXED);
    if (count >= 16) {
        __atomic_fetch_add(&g_tombstone_drops_total, 1ULL,
                           __ATOMIC_RELAXED);
        return;
    }
    g_tombstones[count].owner_id = owner_id;
    g_tombstones[count].owner_gen = owner_gen;
    __atomic_store_n(&g_tombstone_count, count + 1, __ATOMIC_RELAXED);
}

bool timer_srv_rpc(const TimerRpc &req, TimerRpc &reply) {
    if (!ensure_bound()) {
        return false;
    }
    drain_tombstones();
    if (!rpc_once(req, reply)) {
        return false;
    }
    if (!anchor_valid()) {
        refresh_anchor();
    }
    return true;
}

void publish_anchor(uint64_t anchor_ns, uint64_t mono_at_anchor_ns,
                    uint8_t flags) noexcept {
    // Monotone guard (INV-T4 kernel side): a backward sample holds the
    // latch (never acts on it) — same discipline as the server.
    if (__atomic_load_n(&g_anchor_valid, __ATOMIC_ACQUIRE)) {
        const uint64_t cur_ns =
            __atomic_load_n(&g_anchor_ns, __ATOMIC_ACQUIRE);
        if (anchor_ns < cur_ns) {
            return;
        }
    }
    __atomic_store_n(&g_anchor_ns, anchor_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&g_anchor_mono_ns, mono_at_anchor_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&g_anchor_flags, flags, __ATOMIC_RELAXED);
    __atomic_store_n(&g_anchor_valid, true, __ATOMIC_RELEASE);
}

uint64_t anchor_realtime_ns() noexcept {
    if (!__atomic_load_n(&g_anchor_valid, __ATOMIC_ACQUIRE)) {
        // Boot fallback (open question default, #194): the kernel keeps
        // one RTC read until rtc-pd's first push, so early boot/dmesg
        // timestamps are not epoch-1970.
        const uint64_t boot_sec = arch::RTC::read_seconds();
        constexpr uint64_t NS_PER_SEC = 1000000000ULL;
        constexpr uint64_t MAX_U64 = 0xFFFFFFFFFFFFFFFFULL;
        const uint64_t boot_ns = (boot_sec > MAX_U64 / NS_PER_SEC)
                                     ? MAX_U64
                                     : boot_sec * NS_PER_SEC;
        publish_anchor(boot_ns, arch::Timer::ns_monotonic(), 0);
    }
    const uint64_t anchor = __atomic_load_n(&g_anchor_ns, __ATOMIC_ACQUIRE);
    const uint64_t mono_at =
        __atomic_load_n(&g_anchor_mono_ns, __ATOMIC_ACQUIRE);
    const uint64_t now = arch::Timer::ns_monotonic();
    uint64_t delta = 0;
    if (now >= mono_at) {
        delta = now - mono_at;
    }
    return PosixTime::sat_add(anchor, delta);
}

bool anchor_valid() noexcept {
    return __atomic_load_n(&g_anchor_valid, __ATOMIC_ACQUIRE);
}

void anchor_invalidate() noexcept {
    __atomic_store_n(&g_anchor_valid, false, __ATOMIC_RELEASE);
}

void refresh_anchor() noexcept {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0 || !timer_srv_alive()) {
        return;
    }
    // Issue #194 (S2-A: restart resync): the cell is refreshed only while
    // invalid — exactly the state pd_restart_rtc leaves behind. Re-rendezvous
    // first (task context here; never from the tick path): a respawned rtc-pd
    // waits for its rendezvous BIND and never feeds otherwise, so polling the
    // server without this would re-latch a stale anchor with clear flags.
    // Both calls are bounded fail-closed; a dead/absent PD simply leaves the
    // server (and the cell) DEGRADED.
    (void)kernel::driver_pds::pd_bind_rtc();
    (void)kernel::driver_pds::pd_rtc_push_epoch();
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::ANCHOR_PUSH);
    if (!fill_owner(req)) {
        return;
    }
    TimerRpc reply{};
    if (!rpc_once(req, reply)) {
        return;
    }
    if (reply.op != static_cast<uint8_t>(TimerSrvOp::ANCHOR_PUSH) ||
        reply.timer_id != req.timer_id) {
        return;
    }
    publish_anchor(reply.value_ns, reply.interval_ns, reply.flags);
}

bool nsleep_send(uint64_t value_ns, uint64_t &out_token) noexcept {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0 || !timer_srv_alive()) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::SLEEP_REQUEST);
    req.epoch = timer_srv_epoch();
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.value_ns = value_ns;
    const uint64_t token =
        __atomic_fetch_add(&g_sleep_seq, 1ULL, __ATOMIC_RELAXED);
    req.timer_id = token;
    Message msg{};
    msg.type = static_cast<uint64_t>(req.op);
    msg.priority = cur->priority;
    __builtin_memcpy(msg.data, &req, sizeof(req));
    msg.data_size = sizeof(req);
    if (!IPC::send(pid, msg)) {
        return false;
    }
    out_token = token;
    return true;
}

int64_t nsleep_wait(uint64_t sleep_token) noexcept {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0) {
        return -kPosixErrAgain;
    }
    uint64_t fired = 0;
    const int64_t waited = wait_for_fire(pid, TimerSrvOp::SLEEP_REQUEST,
                                         sleep_token, fired);
    if (waited == 0) {
        return 0;
    }
    // Server death: best-effort SLEEP_CANCEL against the live cell (the
    // restart already purged by epoch, so this is usually a no-op), then
    // -EINTR (the carried #194 limitation pins the signal behavior).
    TimerRpc cancel{};
    cancel.op = static_cast<uint8_t>(TimerSrvOp::SLEEP_CANCEL);
    if (fill_owner(cancel)) {
        cancel.timer_id = sleep_token;
        TimerRpc cancel_reply{};
        (void)timer_srv_rpc(cancel, cancel_reply);
    }
    return -kPosixErrIntr;
}

int64_t timerfd_deliver(uint32_t index, uint32_t slot_gen,
                        uint64_t &out_count) noexcept {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0 || !timer_srv_alive()) {
        return -kPosixErrAgain;
    }
    // INV-T5 drop site: the vnode tag travels opaquely, never as kernel
    // deadline state (INV-T2 holds); the event token must equal the tag
    // before anybody wakes — stale reuse and post-delete in-flight events
    // die here, not in a race.
    const uint64_t tag =
        (static_cast<uint64_t>(slot_gen) << 32) | index;
    uint64_t count = 0;
    const int64_t waited = wait_for_fire(pid, TimerSrvOp::TIMERFD_EXPIRED,
                                         tag, count);
    if (waited == 0) {
        out_count = count;
        return 0;
    }
    // -EINTR (death) degrades to -EAGAIN on the read path: fail-closed
    // and retryable, never a hang (INV-T8 waker b).
    return -kPosixErrAgain;
}

void timerfd_disarm(uint32_t index, uint32_t slot_gen) noexcept {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return;
    }
    // SETTIME value=0: disarm AND emit the STALE_TOKEN cancel marker
    // (server bumps slot_gen; in-flight events mismatch the tag).
    // No tag update: the vnode is being torn down.
    uint32_t dropped_gen = slot_gen;
    (void)timerfd_settime_rpc(index, slot_gen, 0, 0, cur->id,
                              cur->generation, dropped_gen);
}

int64_t timer_create_rpc(uint64_t clock_id, uint64_t owner_id,
                         uint32_t owner_gen, uint64_t &out_timer_id) noexcept {
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::TIMER_CREATE);
    req.clock_id = static_cast<uint16_t>(clock_id);
    if (!fill_owner(req)) {
        return -kPosixErrAgain;
    }
    req.owner_id = static_cast<uint32_t>(owner_id);
    req.owner_gen = owner_gen;
    TimerRpc reply{};
    if (!timer_srv_rpc(req, reply)) {
        return -kPosixErrAgain;
    }
    if (reply.status != 0) {
        return static_cast<int64_t>(reply.status);
    }
    out_timer_id = reply.timer_id;
    return 0;
}

int64_t timer_settime_rpc(uint64_t timer_id, uint64_t value_ns,
                          uint64_t interval_ns, uint64_t owner_id,
                          uint32_t owner_gen) noexcept {
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::TIMER_SETTIME);
    req.timer_id = timer_id;
    req.value_ns = value_ns;
    req.interval_ns = interval_ns;
    if (!fill_owner(req)) {
        return -kPosixErrAgain;
    }
    req.owner_id = static_cast<uint32_t>(owner_id);
    req.owner_gen = owner_gen;
    TimerRpc reply{};
    if (!timer_srv_rpc(req, reply)) {
        return -kPosixErrAgain;
    }
    if (reply.status != 0) {
        return static_cast<int64_t>(reply.status);
    }
    if (reply.timer_id != timer_id) {
        return -kPosixErrAgain;
    }
    return 0;
}

int64_t timer_gettime_rpc(uint64_t timer_id, uint64_t owner_id,
                          uint32_t owner_gen, uint64_t &out_value_ns,
                          uint64_t &out_interval_ns) noexcept {
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::TIMER_GETTIME);
    req.timer_id = timer_id;
    if (!fill_owner(req)) {
        return -kPosixErrAgain;
    }
    req.owner_id = static_cast<uint32_t>(owner_id);
    req.owner_gen = owner_gen;
    TimerRpc reply{};
    if (!timer_srv_rpc(req, reply)) {
        return -kPosixErrAgain;
    }
    if (reply.status != 0) {
        return static_cast<int64_t>(reply.status);
    }
    if (reply.timer_id != timer_id) {
        return -kPosixErrAgain;
    }
    out_value_ns = reply.value_ns;
    out_interval_ns = reply.interval_ns;
    return 0;
}

int64_t timer_delete_rpc(uint64_t timer_id, uint64_t owner_id,
                         uint32_t owner_gen) noexcept {
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::TIMER_DELETE);
    req.timer_id = timer_id;
    if (!fill_owner(req)) {
        return -kPosixErrAgain;
    }
    req.owner_id = static_cast<uint32_t>(owner_id);
    req.owner_gen = owner_gen;
    TimerRpc reply{};
    if (!timer_srv_rpc(req, reply)) {
        return -kPosixErrAgain;
    }
    if (reply.status != 0) {
        return static_cast<int64_t>(reply.status);
    }
    if (reply.timer_id != timer_id) {
        return -kPosixErrAgain;
    }
    return 0;
}

int64_t timerfd_create_rpc(uint64_t clock_id, uint64_t owner_id,
                           uint32_t owner_gen, uint32_t &out_index,
                           uint32_t &out_gen) noexcept {
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::TIMERFD_CREATE);
    req.clock_id = static_cast<uint16_t>(clock_id);
    if (!fill_owner(req)) {
        return -kPosixErrAgain;
    }
    req.owner_id = static_cast<uint32_t>(owner_id);
    req.owner_gen = owner_gen;
    TimerRpc reply{};
    if (!timer_srv_rpc(req, reply)) {
        return -kPosixErrAgain;
    }
    if (reply.status != 0) {
        return static_cast<int64_t>(reply.status);
    }
    out_index = static_cast<uint32_t>(reply.timer_id & 0xFFFFFFFFULL);
    out_gen = static_cast<uint32_t>((reply.timer_id >> 32) & 0xFFFFFFFFULL);
    return 0;
}

int64_t timerfd_settime_rpc(uint32_t index, uint32_t slot_gen,
                            uint64_t value_ns, uint64_t interval_ns,
                            uint64_t owner_id, uint32_t owner_gen,
                            uint32_t &out_new_gen) noexcept {
    TimerRpc req{};
    req.op = static_cast<uint8_t>(TimerSrvOp::TIMERFD_SETTIME);
    req.timer_id = (static_cast<uint64_t>(slot_gen) << 32) | index;
    req.value_ns = value_ns;
    req.interval_ns = interval_ns;
    if (!fill_owner(req)) {
        return -kPosixErrAgain;
    }
    req.owner_id = static_cast<uint32_t>(owner_id);
    req.owner_gen = owner_gen;
    TimerRpc reply{};
    if (!timer_srv_rpc(req, reply)) {
        return -kPosixErrAgain;
    }
    if (reply.status != 0) {
        return static_cast<int64_t>(reply.status);
    }
    // The server bumps slot_gen on disarm (STALE_TOKEN cancel marker);
    // the caller re-tags the vnode so re-arm stays valid.
    const uint32_t new_gen =
        static_cast<uint32_t>((reply.timer_id >> 32) & 0xFFFFFFFFULL);
    const uint32_t new_index =
        static_cast<uint32_t>(reply.timer_id & 0xFFFFFFFFULL);
    if (new_index != index) {
        return -kPosixErrAgain;
    }
    out_new_gen = new_gen;
    return 0;
}

int64_t sys_clock_gettime_srv(uint64_t clock_id, uint64_t out_timespec_ptr) {
    uint64_t now_ns = 0;
    if (clock_id == static_cast<uint64_t>(PosixClock::MONOTONIC)) {
        // Hardware fact (INV-T1): no RPC, rewind-proof by construction.
        now_ns = arch::Timer::ns_monotonic();
    } else if (clock_id == static_cast<uint64_t>(PosixClock::REALTIME)) {
        if (!anchor_valid()) {
            refresh_anchor();
        }
        now_ns = anchor_realtime_ns();
    } else {
        return -kPosixErrInvalid;
    }
    if (out_timespec_ptr == 0) {
        return -kPosixErrFault;
    }
    const TimespecU split = PosixTime::ns_to_timespec(now_ns);
    AbiTimespec local{};
    local.tv_sec = split.sec;
    local.tv_nsec = split.nsec;
    const bool is_user = syscall_is_user_task();
    if (is_user) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto out_ptr = checked(reinterpret_cast<AbiTimespec *>(
            out_timespec_ptr));
        if (!out_ptr.valid()) {
            return -kPosixErrFault;
        }
        if (!safe_copy_to_user(out_ptr.unsafe_ptr(), &local, 1)) {
            return -kPosixErrFault;
        }
    } else {
        if (out_timespec_ptr == 0) {
            return -kPosixErrFault;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        *reinterpret_cast<AbiTimespec *>(out_timespec_ptr) = local;
    }
    return 0;
}

int64_t sys_nanosleep_srv(uint64_t req_ptr, uint64_t rem_ptr) {
    auto *cur = syscall_task();
    if (cur == nullptr) {
        return -kPosixErrAgain;
    }
    const bool is_user = syscall_is_user_task();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto *req_abi = reinterpret_cast<const AbiTimespec *>(req_ptr);
    TimespecU req{};
    if (req_abi == nullptr) {
        return -kPosixErrFault;
    }
    if (is_user) {
        auto checked_ptr = checked(req_abi);
        if (!checked_ptr.valid()) {
            return -kPosixErrFault;
        }
        AbiTimespec local{};
        if (!safe_copy_from_user(&local, checked_ptr.unsafe_ptr(), 1)) {
            return -kPosixErrFault;
        }
        req.sec = local.tv_sec;
        req.nsec = local.tv_nsec;
    } else {
        req.sec = req_abi->tv_sec;
        req.nsec = req_abi->tv_nsec;
    }
    uint64_t total_ns = 0;
    if (!PosixTime::timespec_to_ns(req, total_ns)) {
        return -kPosixErrInvalid;
    }
    // Zero timeout returns immediately: no arm, no block (§11.3).
    if (total_ns == 0) {
        return 0;
    }
    uint64_t sleep_token = 0;
    const uint64_t send_mono_ns = arch::Timer::ns_monotonic();
    if (!nsleep_send(total_ns, sleep_token)) {
        return -kPosixErrAgain;
    }
    const int64_t waited = nsleep_wait(sleep_token);
    if (waited != 0) {
        // EINTR path: report the remainder (saturating, never negative).
        if (rem_ptr != 0) {
            const uint64_t now_ns = arch::Timer::ns_monotonic();
            const uint64_t deadline_ns =
                PosixTime::sat_add(send_mono_ns, total_ns);
            uint64_t remaining_ns = 0;
            if (deadline_ns > now_ns) {
                remaining_ns = deadline_ns - now_ns;
            }
            const TimespecU rem = PosixTime::ns_to_timespec(remaining_ns);
            AbiTimespec rem_local{};
            rem_local.tv_sec = rem.sec;
            rem_local.tv_nsec = rem.nsec;
            if (is_user) {
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                auto rem_out = checked(reinterpret_cast<AbiTimespec *>(
                    rem_ptr));
                if (!rem_out.valid()) {
                    return -kPosixErrFault;
                }
                if (!safe_copy_to_user(rem_out.unsafe_ptr(), &rem_local,
                                       1)) {
                    return -kPosixErrFault;
                }
            } else {
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                *reinterpret_cast<AbiTimespec *>(rem_ptr) = rem_local;
            }
        }
        return waited;
    }
    return 0;
}

int64_t sys_timer_create_srv(uint64_t op, uint64_t arg1, uint64_t arg2,
                             uint64_t arg3) {
    auto *cur = syscall_task();
    if (cur == nullptr) {
        return -kPosixErrAgain;
    }
    const bool is_user = syscall_is_user_task();
    if (op == static_cast<uint64_t>(PosixTimerOp::CREATE)) {
        uint64_t timer_id = 0;
        const int64_t created =
            timer_create_rpc(arg1, cur->id, cur->generation, timer_id);
        if (created != 0) {
            return created;
        }
        if (arg2 == 0) {
            (void)timer_delete_rpc(timer_id, cur->id, cur->generation);
            return -kPosixErrFault;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *out_ptr = reinterpret_cast<uint64_t *>(arg2);
        if (is_user) {
            auto checked_ptr = checked(out_ptr);
            if (!checked_ptr.valid()) {
                (void)timer_delete_rpc(timer_id, cur->id, cur->generation);
                return -kPosixErrFault;
            }
            checked_ptr.write(timer_id, 0);
        } else {
            *out_ptr = timer_id;
        }
        return 0;
    }
    if (op == static_cast<uint64_t>(PosixTimerOp::SETTIME)) {
        const bool is_timerfd = (arg3 & 0x1ULL) != 0;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto *spec_ptr = reinterpret_cast<const AbiTimespec *>(arg2);
        // Layout check: AbiItimerspec is value+interval (see the
        // handlers file for the ABI pair order).
        uint64_t value_ns = 0;
        uint64_t interval_ns = 0;
        if (spec_ptr == nullptr) {
            return -kPosixErrFault;
        }
        if (is_user) {
            auto checked_ptr = checked(spec_ptr);
            if (!checked_ptr.valid()) {
                return -kPosixErrFault;
            }
            AbiTimespec pair[2] = {};
            if (!safe_copy_from_user(pair, checked_ptr.unsafe_ptr(), 2)) {
                return -kPosixErrFault;
            }
            // AbiItimerspec layout is {it_interval, it_value} (handlers
            // file): pair[0] is the period, pair[1] the expiry.
            TimespecU value{pair[1].tv_sec, pair[1].tv_nsec};
            TimespecU interval{pair[0].tv_sec, pair[0].tv_nsec};
            if (!PosixTime::timespec_to_ns(value, value_ns) ||
                !PosixTime::timespec_to_ns(interval, interval_ns)) {
                return -kPosixErrInvalid;
            }
        } else {
            const auto *words =
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                reinterpret_cast<const AbiTimespec *>(arg2);
            TimespecU value{words[1].tv_sec, words[1].tv_nsec};
            TimespecU interval{words[0].tv_sec, words[0].tv_nsec};
            if (!PosixTime::timespec_to_ns(value, value_ns) ||
                !PosixTime::timespec_to_ns(interval, interval_ns)) {
                return -kPosixErrInvalid;
            }
        }
        if (!is_timerfd) {
            return timer_settime_rpc(arg1, value_ns, interval_ns, cur->id,
                                     cur->generation);
        }
        // Timerfd SETTIME via the timer multiplex (a1 = fd, a3 = 1):
        // resolve fd→tag, RPC, then re-tag the vnode with the server's
        // post-op generation (STALE_TOKEN cancel marker on disarm).
        const int timerfd_fd = static_cast<int>(arg1);
        auto *desc = cur->fd_table.get(timerfd_fd);
        if (desc == nullptr || !is_timerfd_vnode(desc->vnode)) {
            return -kPosixErrBadFd;
        }
        // The vnode tag is (slot_gen << 32) | index (handlers TU layout).
        const uint64_t tag =
            reinterpret_cast<uint64_t>(desc->vnode->private_data);
        const uint32_t resolved_index =
            static_cast<uint32_t>(tag & 0xFFFFFFFFULL);
        const uint32_t resolved_gen =
            static_cast<uint32_t>((tag >> 32) & 0xFFFFFFFFULL);
        uint32_t new_gen = resolved_gen;
        const int64_t armed = timerfd_settime_rpc(
            resolved_index, resolved_gen, value_ns, interval_ns, cur->id,
            cur->generation, new_gen);
        if (armed != 0) {
            return armed;
        }
        if (new_gen != resolved_gen) {
            desc->vnode->private_data = reinterpret_cast<void *>(
                (static_cast<uint64_t>(new_gen) << 32) | resolved_index);
        }
        return 0;
    }
    if (op == static_cast<uint64_t>(PosixTimerOp::GETTIME)) {
        if (arg2 == 0) {
            return -kPosixErrFault;
        }
        uint64_t value_ns = 0;
        uint64_t interval_ns = 0;
        const int64_t read = timer_gettime_rpc(arg1, cur->id, cur->generation,
                                              value_ns, interval_ns);
        if (read != 0) {
            return read;
        }
        const TimespecU value = PosixTime::ns_to_timespec(value_ns);
        const TimespecU interval = PosixTime::ns_to_timespec(interval_ns);
        AbiTimespec pair[2] = {};
        pair[0].tv_sec = interval.sec;
        pair[0].tv_nsec = interval.nsec;
        pair[1].tv_sec = value.sec;
        pair[1].tv_nsec = value.nsec;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *out_ptr = reinterpret_cast<AbiTimespec *>(arg2);
        if (is_user) {
            auto checked_ptr = checked(out_ptr);
            if (!checked_ptr.valid()) {
                return -kPosixErrFault;
            }
            if (!safe_copy_to_user(checked_ptr.unsafe_ptr(), pair, 2)) {
                return -kPosixErrFault;
            }
        } else {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *dst = reinterpret_cast<AbiTimespec *>(arg2);
            dst[0] = pair[0];
            dst[1] = pair[1];
        }
        return 0;
    }
    if (op == static_cast<uint64_t>(PosixTimerOp::DELETE)) {
        return timer_delete_rpc(arg1, cur->id, cur->generation);
    }
    return -kPosixErrInvalid;
}

int64_t sys_timerfd_create_srv(uint64_t clock_id, uint64_t flags) {
    auto *cur = syscall_task();
    if (cur == nullptr) {
        return -kPosixErrAgain;
    }
    if ((flags & ~static_cast<uint64_t>(vfs::O_NONBLOCK)) != 0) {
        // Only O_NONBLOCK is known (mirrors the OFF leg).
        return -kPosixErrInvalid;
    }
    uint32_t index = 0;
    uint32_t gen = 0;
    const int64_t allocated =
        timerfd_create_rpc(clock_id, cur->id, cur->generation, index, gen);
    if (allocated != 0) {
        return allocated;
    }
    // Pack the server-issued tag for the handlers TU vnode wiring.
    return static_cast<int64_t>((static_cast<uint64_t>(gen) << 32) | index);
}

int64_t timerfd_try_consume(uint32_t index, uint32_t slot_gen,
                             uint64_t &out_count) noexcept {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0 || !timer_srv_alive()) {
        return -kPosixErrAgain;
    }
    const uint64_t tag =
        (static_cast<uint64_t>(slot_gen) << 32) | index;
    int64_t poll_status = -kPosixErrAgain;
    if (drain_fire_once(pid, TimerSrvOp::TIMERFD_EXPIRED, tag, out_count,
                        poll_status)) {
        return poll_status;
    }
    return -kPosixErrAgain;
}

void snapshot_reset() noexcept {
    timer_srv_bump_epoch();
}

uint64_t tombstone_drops_total() noexcept {
    return __atomic_load_n(&g_tombstone_drops_total, __ATOMIC_RELAXED);
}

int64_t bind_error() noexcept {
    return __atomic_load_n(&g_bind_status, __ATOMIC_ACQUIRE);
}

} // namespace timer_srv
} // namespace kernel::time

#endif // CONFIG_TIMER_SRV
