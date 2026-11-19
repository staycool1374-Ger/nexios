/// @file park_lock.cpp
/// @brief Bounded spin → timed park acquisition (M2, issues #545/#546).

#include <kernel/sync/park_lock.hpp>
#include <kernel/test/park_seam.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/task/scheduler_lock_guard.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/task/tick_watchdog.hpp>
#if defined(CONFIG_DEBUG)
#include <lib/logger.hpp>
#endif

namespace kernel {
namespace sync {
namespace {

uint64_t g_park_count = 0;
uint64_t g_spin_fallback_count = 0;
uint64_t g_park_exhausted_count = 0;
uint64_t g_park_denied_reason =
    static_cast<uint64_t>(ParkDenyReason::None);
// M2 follow-up (#545 FIRE): bounded denied-spin block escapes + foreign
// depths admitted to the park (INV-L4).  Diagnostic only.
uint64_t g_denied_escape_count = 0;
uint64_t g_foreign_depth_count = 0;
// Wedge hunt (#545): exhausted-fallback re-arms (timer kept alive) + park
// attempts that observed a clobbered IRQ state (INV-L1' repairs).
uint64_t g_park_rearm_count = 0;
uint64_t g_park_if_clobber_count = 0;

void note_denied(ParkDenyReason reason) noexcept {
    __atomic_fetch_add(&g_spin_fallback_count, 1, __ATOMIC_RELAXED);
    g_park_denied_reason = static_cast<uint64_t>(reason);
}

// Per-site one-shot diagnostic latches (site = string literal, compared
// by pointer).  Slots cover all ParkLockGuard sites with margin; a full
// table stays loud (fail-open diagnostics, never silent).
inline constexpr uint64_t kSiteLatchSlots = 16;

struct SiteLatch {
    const char *site = nullptr;
    bool fired = false;
};

SiteLatch g_escape_latches[kSiteLatchSlots] = {};
SiteLatch g_exhaust_latches[kSiteLatchSlots] = {};
SiteLatch g_budget_latches[kSiteLatchSlots] = {};

bool site_latch_fire(SiteLatch *latches, const char *site) noexcept {
    for (uint64_t i = 0; i < kSiteLatchSlots; ++i) {
        if (latches[i].site == site) {
            if (latches[i].fired)
                return false;
            latches[i].fired = true;
            return true;
        }
        if (latches[i].site == nullptr) {
            latches[i].site = site;
            latches[i].fired = true;
            return true;
        }
    }
    return true;
}

/// @brief Annotate + INV-L1' repair on a successful take.  try_lock and
///        pause-spin never change IF, so the repair only fires on
///        park-window exits (whose sti/hlt loop ends cli'd) — the DEBUG
///        clobber counter then proves the repair ran in-run.
void take_success(SpinLock &lock, const char *site,
                  bool entry_if) noexcept {
    sync::spinlock_annotate_site(&lock, site);
    if (arch::interrupts_enabled() != entry_if) {
        __atomic_fetch_add(&g_park_if_clobber_count, 1,
                           __ATOMIC_RELAXED);
        if (entry_if)
            arch::sti();
        else
            arch::cli();
    }
}

enum class ParkWindowResult : uint8_t { Acquired, Contended, ArmFailed };

/// @brief One park window: arm the wheel timeout, BLOCKED + dequeue,
///        reschedule, sti/hlt wait, cancel, RUNNING + resume unlink
///        (INV-L2), single try.  The Acquired exit repairs INV-L1' via
///        take_success; the Contended exit ends cli'd exactly like the
///        pre-hunt code, so the resume body is byte-identical in IRQ
///        behavior to what the gates already validated.
ParkWindowResult park_window(SpinLock &lock, const char *site,
                             TaskControlBlock &task, uint64_t park_ns,
                             bool entry_if) noexcept {
    if (!time::PosixTime::sleep_arm(task, park_ns)) {
        note_denied(ParkDenyReason::ArmFailed);
        return ParkWindowResult::ArmFailed;
    }
    {
        arch::IrqGuard irq_guard{};
        Scheduler::dequeue_ready(task);
        task.state = TaskState::BLOCKED;
    }
    Scheduler::reschedule();
    // sys_receive shape (unconditional sti/hlt/cli): trap entry owns the
    // IRQ state, so sti is safe here — and mandatory, since IF=0 sleeps
    // through the wheel timeout.
    for (;;) {
        arch::sti();
        arch::hlt();
        arch::cli();
        if (task.state != TaskState::BLOCKED)
            break;
    }
    time::PosixTime::sleep_cancel(task);
    // INV-L2 resume: the tick scan READY+enqueued us; resuming on-CPU
    // without a dispatch would leave RUNNING+queued, so unlink first
    // (dequeue_ready is a safe no-op when not queued), then RUNNING.
    Scheduler::dequeue_ready(task);
    task.state = TaskState::RUNNING;
    tick_watchdog::checkpoint("lock_park_resume");
    if (lock.try_lock()) {
        take_success(lock, site, entry_if);
        return ParkWindowResult::Acquired;
    }
    return ParkWindowResult::Contended;
}

} // namespace

// Note: the own-scope token is arch::irq_guard_owner_token() (current
// task, unconditionally — see irq_guard.hpp for why it must not depend on
// isr_nesting/IF: the token is recorded at take time and compared at check
// time, and IF always flips in between).  No local copy is kept: take and
// check must use the identical source or the comparison fabricates
// foreign owners (observed on rv64).

static ParkDenyReason deny_reason() noexcept {
    if (CONFIG_LOCK_PARK_ENABLE == 0)
        return ParkDenyReason::Disabled;
    // IsrContext arm (M2 §6.2 + #545 FIRE): a hardware ISR must never park
    // (no task to deschedule).  Precision carve-outs, both fail-closed,
    // live in the shared predicate `arch::isr_context_park_denied`
    // (irq_guard.hpp): syscall-trap level is exempt, and a stale nesting
    // count (rv64 suite baseline) denies only while IF==0 — IF==1 proves
    // not-a-hardware-ISR.  IF==0 keeps the deny, covering the QE
    // manual-nesting test which holds IF=0 via IrqGuard.
    if (arch::isr_context_park_denied(arch::interrupts_enabled()))
        return ParkDenyReason::IsrContext;
    if (arch::irq_guard_depth_own() != 0) {
        // #578 test-context waiver: a harness-phantom depth (the suite's
        // outer IrqGuard, not a live critical section — IF==1 proves it)
        // does not deny a marked task under an armed seam. Every other arm
        // below still runs and still denies. Falls through (no early
        // return) so LockHeld + task-validity arms are always enforced.
        auto *waive_task = Scheduler::current_task();
        const bool waived =
            waive_task != nullptr &&
            waive_task->magic == TaskControlBlock::TCB_MAGIC &&
            test::park_ctx_waivable(*waive_task);
        if (!waived) {
            // INV-L4 (#545 FIRE, convicted H1a/V1): a depth owned by a
            // foreign context is diagnostic-only, never a denial when
            // allowed — the syscall trap path holds no IrqGuard, so any
            // depth seen there is foreign/leaked.  Owner 0 (unknown/boot)
            // and the own token stay denied (fail-closed).
            const uint64_t owner = arch::irq_guard_owner_own();
            if (owner != 0 && owner != arch::irq_guard_owner_token())
                return ParkDenyReason::ForeignGuard;
            return ParkDenyReason::IrqGuarded;
        }
    }
    if (spinlock_held_by_current_count() != 0)
        return ParkDenyReason::LockHeld;
    // Issue #582 INV-PARK-2: no context owning a scheduler_lock_ take may
    // enter either park (the tick cannot take the lock, so a parked holder
    // could never be dispatched).  Deny + latch the runner for the release
    // hook / tick service points.  Placed ahead of the task-validity arms:
    // a BLOCKED holder must latch (SelfLock service), not fall into the
    // generic NotRunning refusal.  Class is SelfLock by construction here
    // (depth != 0); diagnostic-only, never decided upon (§4.3).
    if (scheduler_lock_depth_own() != 0) {
        auto *deny_task = Scheduler::current_task();
        if (deny_task != nullptr &&
            deny_task->magic == TaskControlBlock::TCB_MAGIC) {
            gs::lockpark_latch_arm(deny_task->id, deny_task->generation,
                                   static_cast<uint8_t>(
                                       gs::LockParkClass::SelfLock));
        }
        return ParkDenyReason::SchedLock;
    }
    auto *task = Scheduler::current_task();
    if (task == nullptr || task->magic != TaskControlBlock::TCB_MAGIC)
        return ParkDenyReason::NotTask;
    if (Scheduler::is_idle_task(task))
        return ParkDenyReason::IdleTask;
    if (task->state != TaskState::RUNNING)
        return ParkDenyReason::NotRunning;
    if (CONFIG_TICK_HZ == 0)
        return ParkDenyReason::NoTicks;
    return ParkDenyReason::None;
}

bool lock_park_allowed(const SpinLock &lock) noexcept {
    (void)lock;
    const ParkDenyReason reason = deny_reason();
    if (reason == ParkDenyReason::None)
        return true;
#if CONFIG_LOCK_PARK_FOREIGN_OWNER_ALLOW == 1
    if (reason == ParkDenyReason::ForeignGuard)
        return true;
#endif
    return false;
}

/// @brief Bounded, loud denied-spin block: pause-spins up to
///        CONFIG_LOCK_DENIED_SPIN_ROUNDS with watchdog checkpoints, then
///        returns false so the caller re-diagnoses and re-arms.  Never
///        spins silently forever — the TSC watchdog FIRES on the overrun.
static bool denied_spin_block(SpinLock &lock, const char *where) noexcept {
    for (uint64_t i = 0; i < CONFIG_LOCK_DENIED_SPIN_ROUNDS; ++i) {
        arch::pause();
        if ((i & 255U) == 255U)
            tick_watchdog::checkpoint(where);
        if (lock.try_lock())
            return true;
    }
    return false;
}

/// @brief Escape diagnostic: one ERROR per lock site so the soak log
///        convicts each shape without serial spam.  Carries the order-stack
///        holder identity (task/rank) — a holder with no live take site is
///        the wedge-hunt conviction artifact (#545).
static void note_escape(const SpinLock &lock, const char *site,
                        const char *where, ParkDenyReason reason) noexcept {
    const uint64_t escapes =
        __atomic_fetch_add(&g_denied_escape_count, 1, __ATOMIC_RELAXED) + 1;
#if defined(CONFIG_DEBUG)
    (void)escapes; // Latch-gated now (per-site), counter stays for QE.
    if (site_latch_fire(g_escape_latches, site)) {
        uint64_t holder_task = 0;
        uint64_t holder_rank = 0;
        const bool holder_known = sync::spinlock_find_holder(
            &lock, &holder_task, &holder_rank);
        kernel::Logger::error(
            "[PARK] denied-spin escape lock=%lx site=%s where=%s if=%d "
            "irqd=%lu owner=%lx reason=%lu holder=%lx holder_task=%lu "
            "holder_rank=%lu holder_known=%d",
            reinterpret_cast<uint64_t>(&lock), site, where,
            arch::interrupts_enabled() ? 1 : 0, arch::irq_guard_depth_own(),
            arch::irq_guard_owner_own(), static_cast<uint64_t>(reason),
            reinterpret_cast<uint64_t>(lock.holder()), holder_task,
            holder_rank, holder_known ? 1 : 0);
    }
#else
    (void)lock;
    (void)site;
    (void)where;
    (void)escapes;
    (void)reason;
#endif
}

/// @brief One-shot exhaustion dump: the first time a site exhausts its
///        park attempts, name the holder's take site + schedulability.
///        This is the wedge-hunt conviction line — A1 (hold across
///        BLOCKED) vs A2 (READY but starved) vs A3 (READY, not queued).
static void note_exhaustion(const SpinLock &lock, const char *site,
                            bool entry_if) noexcept {
    if (!site_latch_fire(g_exhaust_latches, site))
        return;
#if defined(CONFIG_DEBUG)
    uint64_t addrs[sync::kMaxHeldLocks] = {};
    const char *sites[sync::kMaxHeldLocks] = {};
    uint64_t owners[sync::kMaxHeldLocks] = {};
    uint8_t ranks[sync::kMaxHeldLocks] = {};
    uint8_t cpus[sync::kMaxHeldLocks] = {};
    uint8_t take_ifs[sync::kMaxHeldLocks] = {};
    const uint64_t n = sync::spinlock_dump_held(
        addrs, sites, owners, ranks, cpus, take_ifs, sync::kMaxHeldLocks);
    const uint64_t lock_addr = reinterpret_cast<uint64_t>(&lock);
    const char *holder_site = nullptr;
    uint64_t holder_task = 0;
    uint64_t holder_rank = 0;
    uint64_t holder_cpu = 0;
    uint64_t holder_take_if = 0;
    bool holder_known = false;
    for (uint64_t i = 0; i < n; ++i) {
        if (addrs[i] == lock_addr) {
            holder_site = sites[i];
            holder_task = owners[i];
            holder_rank = ranks[i];
            holder_cpu = cpus[i];
            holder_take_if = take_ifs[i];
            holder_known = true;
        }
    }
    // Scheduler snapshot of the recorded holder (lock-free id probe).
    uint64_t h_valid = 0;
    uint64_t h_state = 99;
    uint64_t h_in_rq = 9;
    uint64_t h_rq_prio = 0;
    uint64_t h_prio = 0;
    uint64_t h_on_q = 0;
    uint64_t h_recv = 0;
    uint64_t h_reply = 0;
    uint64_t h_sleep = 0;
    uint64_t h_reply_dest = 0;
    uint64_t h_mq_lock = 0;
    uint64_t h_qcount = 0;
    if (holder_known && holder_task != 0) {
        auto *h = Scheduler::find_task(holder_task);
        if (h != nullptr && TaskControlBlock::is_valid(h)) {
            h_valid = 1;
            h_state = static_cast<uint64_t>(h->state);
            h_in_rq = h->in_ready_queue_ ? 1 : 0;
            h_rq_prio = h->rq_priority_;
            h_prio = h->priority;
            h_on_q = h->blocked_on_queue != nullptr ? 1 : 0;
            h_recv = h->blocked_in_recv ? 1 : 0;
            h_reply = h->reply_wait ? 1 : 0;
            h_sleep = h->sleep_armed ? 1 : 0;
            h_reply_dest = h->reply_wait ? h->reply_wait_dest_id : 0;
            h_mq_lock = reinterpret_cast<uint64_t>(&h->msg_queue.lock_);
            h_qcount = __atomic_load_n(&h->msg_queue.count, __ATOMIC_RELAXED);
        }
    }
    uint64_t w_mq_lock = 0;
    uint64_t w_qcount = 0;
    uint64_t waiter_id = 0;
    uint64_t waiter_state = 99;
    uint64_t waiter_in_rq = 9;
    {
        auto *w = Scheduler::current_task();
        if (w != nullptr && TaskControlBlock::is_valid(w)) {
            waiter_id = w->id;
            waiter_state = static_cast<uint64_t>(w->state);
            waiter_in_rq = w->in_ready_queue_ ? 1 : 0;
            w_mq_lock = reinterpret_cast<uint64_t>(&w->msg_queue.lock_);
            w_qcount = __atomic_load_n(&w->msg_queue.count, __ATOMIC_RELAXED);
        }
    }
    kernel::Logger::error(
        "[PARK] exhausted lock=%lx site=%s entry_if=%d w_cpu=%lu w_task=%lu "
        "w_state=%lu w_in_rq=%lu held=%lu "
        "holder_site=%s holder_task=%lu holder_rank=%lu holder_cpu=%lu "
        "holder_take_if=%lu holder_known=%d h_valid=%lu h_state=%lu "
        "h_in_rq=%lu h_rq_prio=%lu h_prio=%lu h_on_q=%lu h_recv=%lu "
        "h_reply=%lu h_sleep=%lu h_reply_dest=%lu h_mq_lock=%lx "
        "h_qcount=%lu w_mq_lock=%lx w_qcount=%lu unpaired=%lu overflow=%lu",
        lock_addr, site, entry_if ? 1 : 0,
        static_cast<uint64_t>(arch::cpu_index()),
        waiter_id, waiter_state, waiter_in_rq, n,
        holder_site != nullptr ? holder_site : "?", holder_task,
        holder_rank, holder_cpu, holder_take_if, holder_known ? 1 : 0,
        h_valid, h_state, h_in_rq, h_rq_prio, h_prio, h_on_q, h_recv,
        h_reply, h_sleep, h_reply_dest, h_mq_lock, h_qcount, w_mq_lock,
        w_qcount, sync::spinlock_unpaired_release_count(),
        sync::spinlock_overflow_count());
    for (uint64_t i = 0; i < n; ++i) {
        kernel::Logger::error(
            "[PARK] held lock=%lx idx=%lu site=%s owner=%lu rank=%u "
            "cpu=%u take_if=%u",
            lock_addr, i, sites[i] != nullptr ? sites[i] : "?", owners[i],
            ranks[i], cpus[i], take_ifs[i]);
    }
    // Last take / last release for this lock: who took it last, and did
    // a release run after.  take_ord > rel_ord means an unreleased take
    // is outstanding — the lock is held by take_owner.  Record-based
    // (not order-stack), so torn/missing stack entries cannot hide it.
    {
        uint64_t take_ord = 0;
        uint64_t take_owner = 0;
        uint8_t take_rank = 0;
        uint8_t take_cpu = 0;
        uint8_t take_if = 0;
        uint64_t take_tick = 0;
        uint64_t rel_ord = 0;
        uint64_t rel_owner = 0;
        uint8_t rel_rank = 0;
        uint8_t rel_cpu = 0;
        uint8_t rel_if = 0;
        uint64_t rel_tick = 0;
        uint64_t evictor = 0;
        const bool last_known = sync::spinlock_lock_last(
            &lock, &take_ord, &take_owner, &take_rank, &take_cpu, &take_if,
            &take_tick, &rel_ord, &rel_owner, &rel_rank, &rel_cpu, &rel_if,
            &rel_tick, &evictor);
        kernel::Logger::error(
            "[PARK] last lock=%lx known=%d take_ord=%lu take_owner=%lu "
            "take_rank=%u take_cpu=%u take_if=%u take_tick=%lu rel_ord=%lu "
            "rel_owner=%lu rel_rank=%u rel_cpu=%u rel_if=%u rel_tick=%lu "
            "evictor=%lx now=%lu deny=%lu deny_why=%lx park_n=%lu "
            "park_w=%lu",
            lock_addr, last_known ? 1 : 0, take_ord, take_owner, take_rank,
            take_cpu, take_if, take_tick, rel_ord, rel_owner, rel_rank,
            rel_cpu, rel_if, rel_tick, evictor,
            arch::Timer::ticks(),
            Scheduler::resched_park_denied_count(),
            Scheduler::resched_park_denied_why(),
            Scheduler::resched_park_entries_count(),
            Scheduler::resched_park_wakeups_count());
        // Lock word ground truth: distinguishes poisoned/freed memory
        // (0xDD…), an unpublished acquire (locked_=1, holder_=NULL —
        // preempted between exchange and holder store), and a normal
        // holder PC (always the guard ctor — identifies nothing further).
        kernel::Logger::error(
            "[PARK] word lock=%lx locked=%d holder=%lx",
            lock_addr, lock.is_locked_for_debug(),
            reinterpret_cast<uint64_t>(lock.holder_for_debug()));
    }
#else
    (void)lock;
    (void)site;
    (void)entry_if;
#endif
}

/// @brief Re-arm budget line: the fallback re-arms forever (timer alive),
///        so this fires once per site when the budget is spent — proof the
///        wedge survived past bounded parking, with the holder identity
///        attached for the next conviction round.
static void note_rearm_budget(const SpinLock &lock, const char *site,
                              bool entry_if) noexcept {
    __atomic_fetch_add(&g_spin_fallback_count, 1, __ATOMIC_RELAXED);
    if (!site_latch_fire(g_budget_latches, site))
        return;
#if defined(CONFIG_DEBUG)
    kernel::Logger::error(
        "[PARK] re-arm budget exhausted lock=%lx site=%s entry_if=%d "
        "budget=%lu (continuing re-arm, timer alive)",
        reinterpret_cast<uint64_t>(&lock), site, entry_if ? 1 : 0,
        static_cast<uint64_t>(CONFIG_LOCK_PARK_REARM_BUDGET));
#else
    (void)lock;
    (void)site;
    (void)entry_if;
#endif
}

ParkLockGuard::ParkLockGuard(SpinLock &lock, const char *site) noexcept
    : lock_(lock) {
    // INV-L1': snapshot the caller's IRQ state first (nothing above
    // changes IF) — every exit path restores it via take_success.
    entry_if_ = arch::interrupts_enabled();
    if (lock_.try_lock()) {
        owned_ = true;
        take_success(lock_, site, entry_if_);
        return;
    }
    // Bounded pause-spin (fast path unchanged: uncontended try_lock).
    for (uint64_t i = 0; i < CONFIG_LOCK_SPIN_ROUNDS; ++i) {
        arch::pause();
        if ((i & 255U) == 255U)
            tick_watchdog::checkpoint("lock_spin");
        if (lock_.try_lock()) {
            owned_ = true;
            take_success(lock_, site, entry_if_);
            return;
        }
    }
    if (!lock_park_allowed(lock_)) {
        // Status-quo denial: bounded loud spin (never silent).  Each block
        // is bounded; escapes re-diagnose and re-arm so the TSC watchdog
        // FIRES on a true wedge instead of freezing silently.  A
        // ForeignGuard reason here is a predicate/deny TOCTOU (the owner
        // changed between the two same-thread reads): fall through to the
        // park below instead of spinning on a foreign depth.
        const ParkDenyReason reason = deny_reason();
        if (reason == ParkDenyReason::ForeignGuard) {
            __atomic_fetch_add(&g_foreign_depth_count, 1, __ATOMIC_RELAXED);
        } else {
            note_denied(reason);
            for (;;) {
#if CONFIG_LOCK_DENIED_SPIN_STI_ALLOW == 1
                // Last resort (default OFF): sti window heals a genuine
                // own-guard denial shape.  Never taken for ISR context,
                // held spinlocks, or interrupts-already-on.
                if (reason != ParkDenyReason::IsrContext &&
                    spinlock_held_by_current_count() == 0 &&
                    !arch::interrupts_enabled()) {
                    arch::sti();
                    const bool got =
                        denied_spin_block(lock_, "lock_spin_denied_sti");
                    arch::cli();
                    if (got) {
                        owned_ = true;
                        take_success(lock_, site, entry_if_);
                        return;
                    }
                    note_escape(lock_, site, "lock_spin_denied", reason);
                    continue;
                }
#endif
                if (denied_spin_block(lock_, "lock_spin_denied")) {
                    owned_ = true;
                    take_success(lock_, site, entry_if_);
                    return;
                }
                note_escape(lock_, site, "lock_spin_denied", reason);
            }
        }
    }
    __atomic_fetch_add(&g_park_count, 1, __ATOMIC_RELAXED);
    if (arch::irq_guard_depth_own() != 0) {
        // Admitted on a foreign-owned depth (INV-L4): the depth check in
        // the predicate already proved foreign (allowed + depth live).
        // #578: a waived harness phantom is not a foreign scope — the
        // g_foreign_depth_count diagnostic must not absorb it (it would
        // hide the #545 FIRE signal behind seam traffic).
        auto *park_task = Scheduler::current_task();
        const bool waived =
            park_task != nullptr &&
            park_task->magic == TaskControlBlock::TCB_MAGIC &&
            test::park_ctx_waivable(*park_task);
        if (!waived)
            __atomic_fetch_add(&g_foreign_depth_count, 1, __ATOMIC_RELAXED);
    }
    auto *task = Scheduler::current_task();
    // ns per park cycle, saturating (grace ~ms fits u64 trivially).
    uint64_t park_ns = static_cast<uint64_t>(CONFIG_LOCK_PARK_TICKS);
    park_ns *= 1000000000ULL;
    park_ns /= static_cast<uint64_t>(CONFIG_TICK_HZ);
    bool arm_ok = true;
    for (uint64_t attempt = 0;
         attempt < CONFIG_LOCK_PARK_MAX_ATTEMPTS && arm_ok; ++attempt) {
        const ParkWindowResult result =
            park_window(lock_, site, *task, park_ns, entry_if_);
        if (result == ParkWindowResult::Acquired) {
            owned_ = true;
            return;
        }
        if (result == ParkWindowResult::ArmFailed)
            arm_ok = false;
        // Still contended: next attempt re-arms (fresh wheel timeout).
    }
    __atomic_fetch_add(&g_park_exhausted_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_spin_fallback_count, 1, __ATOMIC_RELAXED);
    note_exhaustion(lock_, site, entry_if_);
    // INV-W1' (#545 wedge hunt): the old fallback spun here with IF=0,
    // starving the tick that would dispatch a live holder — a
    // self-completing wedge.  Re-arm the park instead: every window
    // restores the entry IRQ state at its boundaries, so the timer stays
    // alive and a live holder is always dispatchable.  Deny/state are
    // re-verified per window (INV-L2); a lapsed predicate or a dead timer
    // bridges with an entry-IF-preserving bounded loud spin — never an
    // unbounded IF=0 spin.
    for (uint64_t rearm = 0;; ++rearm) {
        __atomic_fetch_add(&g_park_rearm_count, 1, __ATOMIC_RELAXED);
        if (rearm == CONFIG_LOCK_PARK_REARM_BUDGET)
            note_rearm_budget(lock_, site, entry_if_);
        const ParkDenyReason why = deny_reason();
        if (why != ParkDenyReason::None ||
            task->state != TaskState::RUNNING) {
            // Predicate lapsed mid-hunt: bounded loud spin (pause +
            // watchdog checkpoints; try_lock/pause never touch IF, so the
            // entry state is preserved by construction), then re-verify.
            if (denied_spin_block(lock_, "lock_spin_rearm")) {
                owned_ = true;
                take_success(lock_, site, entry_if_);
                return;
            }
            note_escape(lock_, site, "lock_spin_rearm", why);
            continue;
        }
        switch (park_window(lock_, site, *task, park_ns, entry_if_)) {
            case ParkWindowResult::Acquired:
                owned_ = true;
                return;
            case ParkWindowResult::Contended:
                break;
            case ParkWindowResult::ArmFailed:
                // Timer truly dead: entry-IF-preserving bounded loud
                // spin, then re-verify (never unbounded, never IF=0).
                if (denied_spin_block(lock_, "lock_spin_rearm_armfail")) {
                    owned_ = true;
                    take_success(lock_, site, entry_if_);
                    return;
                }
                note_escape(lock_, site, "lock_spin_rearm_armfail",
                            ParkDenyReason::ArmFailed);
                break;
        }
    }
}

ParkLockGuard::~ParkLockGuard() noexcept {
    if (owned_) {
        owned_ = false;
        lock_.unlock();
    }
}

void ParkLockGuard::release() noexcept {
    if (owned_) {
        owned_ = false;
        lock_.unlock();
    }
}

uint64_t lock_park_count() noexcept {
    return __atomic_load_n(&g_park_count, __ATOMIC_RELAXED);
}

uint64_t lock_spin_fallback_count() noexcept {
    return __atomic_load_n(&g_spin_fallback_count, __ATOMIC_RELAXED);
}

uint64_t lock_park_exhausted_count() noexcept {
    return __atomic_load_n(&g_park_exhausted_count, __ATOMIC_RELAXED);
}

uint64_t lock_park_denied_reason() noexcept {
    return g_park_denied_reason;
}

uint64_t lock_denied_escape_count() noexcept {
    return __atomic_load_n(&g_denied_escape_count, __ATOMIC_RELAXED);
}

uint64_t lock_foreign_depth_count() noexcept {
    return __atomic_load_n(&g_foreign_depth_count, __ATOMIC_RELAXED);
}

uint64_t lock_park_rearm_count() noexcept {
    return __atomic_load_n(&g_park_rearm_count, __ATOMIC_RELAXED);
}

uint64_t lock_park_if_clobber_count() noexcept {
    return __atomic_load_n(&g_park_if_clobber_count, __ATOMIC_RELAXED);
}

void park_lock_reset_for_test() noexcept {
    g_park_count = 0;
    g_spin_fallback_count = 0;
    g_park_exhausted_count = 0;
    g_park_denied_reason = static_cast<uint64_t>(ParkDenyReason::None);
    g_denied_escape_count = 0;
    g_foreign_depth_count = 0;
    g_park_rearm_count = 0;
    g_park_if_clobber_count = 0;
    for (uint64_t i = 0; i < kSiteLatchSlots; ++i) {
        g_escape_latches[i] = SiteLatch{};
        g_exhaust_latches[i] = SiteLatch{};
        g_budget_latches[i] = SiteLatch{};
    }
}

} // namespace sync
} // namespace kernel
