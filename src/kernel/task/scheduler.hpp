#pragma once

/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
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

/// @file scheduler.hpp
/// @brief Rate-monotonic scheduler for task management and dispatching.

#pragma once

#include <types.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler_config.hpp>
#include <kernel/task/test_context.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/scheduler_errors.hpp>
#include <kernel/task/task_errors.hpp>
#include <kernel/task/ready_queue_manager.hpp>
#include <kernel/task/deadline_list.hpp>
#include <kernel/task/all_tasks_registry.hpp>

namespace kernel {

/// @brief Test-only override for the NOTIFY_MONITOR action target PID.
///        When CONFIG_DEADLINE_MONITOR_PID == 0 (default), the deadline-miss
///        handler's action=4 path delivers SIGUSR1 to the task whose id equals
///        this value instead.  PfA-A: this now lives in the injected
///        TestContext (TestContext::deadline_monitor_pid); see
///        Scheduler::get_test_context().  Harmless in production (0).

/// @brief Preemptive, rate-monotonic scheduler managing up to MAX_TASKS tasks.
/// @note Scheduler is tick-driven and supports periodic and aperiodic tasks.
class Scheduler {
  public:
    /// @brief Initialises the scheduler and creates the idle task.
    /// @param cfg Boot-time configuration injected from kernel_init (PfA-A).
    ///        Defaults match the pre-refactor behaviour.
    static void init(const SchedulerConfig &cfg = SchedulerConfig{});
    /// @brief Error-returning overload for init().
    static errors::SchedulerError init_err(
        const SchedulerConfig &cfg = SchedulerConfig{});

    /// @brief Injects the test-runner context (PfA-A).  Production keeps
    ///        nullptr so all test flags resolve to false; the harness sets it
    ///        for the duration of a test cycle and clears it afterwards.
    static void set_test_context(TestContext *ctx) noexcept {
        test_context_ = ctx;
    }
    /// @brief Returns the injected test context, or nullptr.
    static TestContext *get_test_context() noexcept {
        return test_context_;
    }

    /// @brief Finds a task by its ID (hash table, O(1) amortized).
    /// @param id Task ID to find.
    /// @return Pointer to the TaskControlBlock, or nullptr.
    static TaskControlBlock *find_task(uint64_t id) noexcept;

    /// @brief Register a task in scheduler tables without making it runnable.
    ///        Registers in all_tasks_, deadline_list_, and id_table_ so the task
    ///        is findable via find_task(), but does NOT enqueue in the ready
    ///        queue.  The task's state must NOT be READY — use for test tasks
    ///        that need setup before they can run.
    static void register_task(TaskControlBlock &task);

    /// @brief Adds a task to the scheduler's run queue.
    /// @param task Reference to the task to add.
    static void add_task(TaskControlBlock &task);
    /// @brief Error-returning overload for add_task().
    /// @return SCHED_ERR_OK on success, SCHED_ERR_TABLE_FULL if task table is
    /// full,
    ///         SCHED_ERR_DUPLICATE_ID if task ID already exists.
    static errors::SchedulerError add_task_err(TaskControlBlock &task);

    /// @brief Removes a task from the scheduler's run queue.
    /// @param task Reference to the task to remove.
    static void remove_task(TaskControlBlock &task);

    /// @brief Best-effort unregister of a TCB from the scheduler's live tables.
    /// Used by TaskControlBlock::cleanup() so that every free path (delete,
    /// MemPool::free, reaper) unregisters — preventing dangling
    /// tasks_[]/id_table_ entries that alias a later allocation (use-after-free
    /// in add_task). Uses try_lock: if the scheduler lock is already held by
    /// the current context (e.g. reap_orphans, which unregisters manually),
    /// this returns false and does nothing. Callers should disable IRQs around
    /// this so the timer ISR (on_tick) cannot transiently hold the lock and
    /// cause a skip.
    /// @return true if the lock was taken (and removal attempted), false if it
    /// was already held by the current context.
    static bool unregister_task(TaskControlBlock &task) noexcept;
    /// @brief Error-returning overload for remove_task().
    /// @return SCHED_ERR_OK on success, SCHED_ERR_NOT_FOUND if task not in
    /// table.
    static errors::SchedulerError remove_task_err(TaskControlBlock &task);

    /// @brief Returns the currently running task (RSP-authoritative).
    /// @return Pointer to the current TaskControlBlock.
    static TaskControlBlock *current_task() noexcept;
    /// @brief Returns the total number of managed tasks.
    /// @return Task count.
    static uint64_t task_count() noexcept;
    /// @brief Returns the task at a given index in the task array.
    /// @param index Index into the task array.
    /// @return Pointer to the TaskControlBlock, or nullptr.
    static TaskControlBlock *task_at(uint64_t index) noexcept;

    /// @brief DEBUG-only (#019/#020): returns true if the given block address
    ///    is still referenced by the scheduler's id_table_ (a stale entry
    ///    pointing at a freed TCB).  Used by TaskControlBlock::create() to
    ///    catch TCB-blocks being reused while still aliased by the id table.
    static bool debug_id_table_references(void *block) noexcept;

    /// @brief Called on each timer tick; updates scheduling state.
    static void on_tick() noexcept;

    /// @brief Forces a reschedule — selects the next task and sets
    /// up context switch.
    static void reschedule() noexcept;

    /// @brief Change a task's scheduling priority at runtime.
    /// Re-buckets the task in the O(1) ready queue (if queued) so the change
    /// takes effect on the next tick, and updates base_priority.  Safe to
    /// call from task context; IRQs are disabled around the move so the
    /// timer ISR cannot read a half-updated bucket.
    /// @param task Target TCB.
    /// @param new_prio New scheduling priority.
    static void set_priority(TaskControlBlock &task,
                             uint64_t new_prio) noexcept;

    /// @brief Reaps orphan TERMINATED tasks (no parent to WAITPID them).
    ///        Single-pass scan: identifies all eligible tasks, destroys them
    ///        without compaction, then compacts the task array once at the end.
    ///        If the idle task is reaped it is immediately recreated and placed
    ///        at index 0 of the compacted array.
    static void reap_orphans() noexcept;

    /// @brief Terminates and removes all non-idle tasks from the scheduler.
    ///        Called after the boot-time test suite to clean up test leftovers
    ///        before production tasks (shell, idle) are created.
    static void cleanup_test_tasks() noexcept;

    /// @brief Stores a pointer to the shell task.
    static void set_shell_task(TaskControlBlock *task) noexcept {
        shell_task_ptr_ = task;
    }

    /// @brief Stores a pointer to the harness (init_task, PID 1) — the task
    ///        that runs the test-cycling code.  Used by rate_monotonic_schedule
    ///        to exempt the harness from preemption during the test cycle
    ///        (BUGS.md#021), while still allowing it to yield voluntarily.
    static void set_harness_task(TaskControlBlock *task) noexcept {
        harness_task_ptr_ = task;
    }

    /// @brief Returns the harness (init_task) TCB, or nullptr if not set.
    static TaskControlBlock *get_harness_task() noexcept {
        return harness_task_ptr_;
    }

    /// @brief Suppress or re-enable the "task terminated" log message.
    ///        Used by reboot_from_table() during intentional teardown.
    static void set_suppress_terminated_log(bool v) noexcept {
        suppress_terminated_log_ = v;
    }

    /// @brief Increments the sporadic server task counter (called from
    /// init_sporadic_server).
    static void inc_sporadic_count() noexcept {
        if (sporadic_task_count_ < MAX_TASKS)
            __atomic_add_fetch(&sporadic_task_count_, 1UL, __ATOMIC_RELAXED);
    }
    /// @brief Decrements the sporadic server task counter (called from
    /// cleanup).
    static void dec_sporadic_count() noexcept {
        if (sporadic_task_count_ > 0)
            __atomic_sub_fetch(&sporadic_task_count_, 1UL, __ATOMIC_RELAXED);
    }
    /// @brief Returns the current sporadic server task count.
    static uint64_t sporadic_count() noexcept {
        return sporadic_task_count_;
    }

    /// @brief Marks a task as READY and adds it to the O(1) ready queue.
    ///        Call this instead of directly setting `task.state = READY`
    ///        to keep the ready-queue bitmap in sync.
    static void set_task_ready(TaskControlBlock &task) noexcept;
    /// @brief Deadline scan entry — runs in task context when the monitor
    ///        task is enabled.  Identical deadline detection logic as the
    ///        inline scan in on_tick() but can hold scheduler_lock_ and
    ///        perform inline KILL cleanup.  Guarded by
    ///        #if CONFIG_DEADLINE_MONITOR_TASK.
    static void scan_deadlines() noexcept;
    /// @brief Ensures the deadline-monitor task exists and is valid.
    ///        Re-spawns it if the TCB was killed (e.g. by reload_daemon_tasks
    ///        during snapshot restore).  Safe to call multiple times.
    static void ensure_monitor() noexcept;
#if CONFIG_DEADLINE_MONITOR_TASK
    /// @brief Forces a fresh deadline-monitor task on the next ensure_monitor().
    ///        The monitor's TCB is killed by reboot_from_table (which rebuilds
    ///        the task table from g_task_defs and does not include the monitor);
    ///        s_monitor_task_ then dangles to a freed/reused block whose magic
    ///        reads are unreliable, so ensure_monitor()'s validity check can
    ///        wrongly decide the monitor still exists.  Resetting the pointer
    ///        forces a clean re-spawn.
    static void reset_monitor_task() noexcept { s_monitor_task_ = nullptr; }
    /// @brief Resets the scan-requested flag (used by snapshot_restore to
    ///        clear stale flags).
    static void reset_scan_requested() noexcept {
        __atomic_store_n(&s_scan_requested_, 0, __ATOMIC_RELEASE);
    }
    /// @brief Returns the deadline-monitor task pointer, or nullptr.
    static TaskControlBlock *get_monitor_task() noexcept {
        return s_monitor_task_;
    }
    /// @brief Sets/clears the test-active flag.  When true, on_tick() skips
    ///        the monitor-wake path to prevent spurious context switches.
    ///        PfA-A: stored in the injected TestContext; no-op (false) when
    ///        no test context is set (production).
    static void set_test_active(bool v) noexcept {
        if (test_context_)
            test_context_->test_active = v;
    }
    static bool is_test_active() noexcept {
        return test_context_ ? test_context_->test_active : false;
    }
#endif
    /// @brief Clears assembly-level context-switch globals
    ///        (scheduler_save_rsp_to, scheduler_load_rsp_from, etc.).
    ///        Safe to call multiple times.  Used after on_tick() in test
    ///        context to prevent a later timer ISR from consuming stale
    ///        globals and performing an unintended context switch.
    static void clear_switch_globals() noexcept;

    /// @brief Rebuild AllTasksRegistry per-priority linked lists using
    ///        each task's current priority.  Called after test-isolation
    ///        snapshot restore to fix stale priority-grouping.
    static void rebuild_all_tasks() noexcept;
    /// @brief Transitions a task to TERMINATED state and removes it from the
    ///        ready queue.  Call this instead of directly setting
    ///        `task.state = TERMINATED` to keep the ready queue consistent.
    static void terminate(TaskControlBlock &task, uint64_t exit_code) noexcept;
    /// @brief Entry point for the deadline-monitor task (priority 127).
    ///        Waits on an atomic handoff flag, calls scan_deadlines() when
    ///        woken by on_tick().  Only compiled when
    ///        CONFIG_DEADLINE_MONITOR_TASK > 0.
    static void monitor_task_entry() noexcept;

    /// @brief Checks if a context switch is needed (reschedule flag).
    /// @return True if a switch is pending.
    static bool needs_switch() noexcept;
    /// @brief Selects the next task to run according to RM policy.
    /// @return Pointer to the next TaskControlBlock.
    static TaskControlBlock *next_task() noexcept;
    /// @brief Sets the current running task.
    /// @param task Reference to the task to set as current.
    static void set_current(TaskControlBlock &task) noexcept;

    /// @brief Charge @p pages to the current task's memory budget.
    /// Returns false if charging would exceed the task's budget cap.
    /// When budget_pages_ == 0 (unlimited), charging always succeeds.
    static bool charge_task_memory(uint64_t pages);

    /// @brief Credit @p pages back to the current task's memory usage.
    static void credit_task_memory(uint64_t pages);
    /// @brief Safely switches the CPU off a task that is being terminated while
    ///        it is the current task, WITHOUT calling switch_to_task (which
    ///        resolves the live RSP owner and corrupts contexts when invoked
    ///        from ISR context, e.g. sys_exit).  Publishes the deferred-switch
    ///        slot directly: the save target is the exiting task's own
    ///        `context.rsp` (a dead slot — the task never resumes, so the ISR
    ///        epilogue's `mov [save], rsp` landing there is harmless), and the
    ///        load target is the next runnable task's `context.rsp`.  The next
    ///        ISR epilogue applies the switch.  Caller must hold no scheduler
    ///        lock; this takes scheduler_lock_ internally.
    static void switch_away_from_terminating(TaskControlBlock &exiting) noexcept;
    /// @brief Allocate a unique task ID from the ID table.
    /// @return A new task ID, or TASK_INVALID if the table is full.
    [[nodiscard]] static uint64_t alloc_id() noexcept;
    /// @brief Error-returning overload for alloc_id().
    /// @param out_id Output parameter for the allocated ID.
    /// @return SCHED_ERR_OK on success, SCHED_ERR_TABLE_FULL if ID table is
    /// full.
    static errors::SchedulerError alloc_id_err(uint64_t &out_id);

    /// @brief Returns the index of the current task (computed from pointer,
    ///         O(n) for snapshot compatibility; prefer current_task()
    ///         directly).
    static uint64_t current_index() noexcept;
    static void set_current_index(uint64_t idx) noexcept;
    /// @brief Set the current task directly by pointer (unambiguous; avoids
    ///        the index-convention mismatch between
    ///        current_index()/set_current_index() (raw AllTasksRegistry order)
    ///        and task_at() (idle-reserved)).
    static void set_current_task(TaskControlBlock *t) noexcept;
    static TaskControlBlock *get_idle_task() noexcept {
        return idle_task_;
    }
    static const AllTasksRegistry &all_tasks() noexcept {
        return all_tasks_;
    }
    static TaskControlBlock *get_shell_task() noexcept {
        return shell_task_ptr_;
    }

    /// @brief Returns whether the scheduler can be preempted.
    /// @return True if preemption is enabled.
    static bool is_preemptible() noexcept {
        return preempt_enabled_;
    }
#if CONFIG_MEMORY_BUDGET
    /// @brief Initialises the global memory budget from available PMM pages.
    static void init_memory_budget(uint64_t total_pages) noexcept;
    /// @brief Reserves pages from the global budget. Returns false if
    ///        insufficient capacity.
    static bool reserve_memory_pages(uint64_t count) noexcept;
    /// @brief Releases pages back to the global budget.
    static void release_memory_pages(uint64_t count) noexcept;
    /// @brief Returns the remaining budget in pages.
    static uint64_t remaining_memory_budget() noexcept;
#endif
    /// @brief Enables or disables preemption.
    /// @param en True to enable preemption.
    static void set_preemptible(bool en) noexcept {
        preempt_enabled_ = en;
    }

    /// @brief Returns the effective scheduling priority, accounting for
    ///        sporadic-server budget state.
    static uint64_t effective_priority(const TaskControlBlock *t) noexcept;
    /// @brief Enqueues a task into the O(1) ready queue at its effective
    /// priority.
    static void enqueue_ready(TaskControlBlock &task) noexcept;
    /// @brief Removes a task from the O(1) ready queue.
    static void dequeue_ready(TaskControlBlock &task) noexcept;
    /// @brief Moves a task from one priority queue to another (re-index).
    static void move_priority(TaskControlBlock &task, uint64_t old_prio,
                              uint64_t new_prio) noexcept;

    /// @brief Lightweight forward iterator over all tasks in the registry.
    ///        Iterates by priority (highest to lowest), then insertion order
    ///        within each priority.
    struct TaskIter {
        uint64_t idx;
        TaskControlBlock *cur_;
        explicit TaskIter(uint64_t start = 0)
            : idx(start), cur_(Scheduler::all_tasks_.first_ptr()) {
            for (uint64_t i = 0; i < start; ++i) {
                cur_ = Scheduler::all_tasks_.next_ptr(cur_);
                if (!cur_)
                    break;
            }
        }
        TaskControlBlock *next(TaskControlBlock *exclude = nullptr) {
            while (cur_) {
                auto *t = cur_;
                cur_ = Scheduler::all_tasks_.next_ptr(cur_);
                ++idx;
                if (t && t->magic == TaskControlBlock::TCB_MAGIC &&
                    t != exclude)
                    return t;
            }
            return nullptr;
        }
    };

    /// @name Test-isolation helpers
    static uint64_t snapshot_max_tasks() {
        return MAX_TASKS;
    }
    static uint64_t snapshot_id_size() {
        return ID_TABLE_SIZE;
    }

    /// @brief Per-task plain fields that are deep-copied into the snapshot.
    ///        Pointers to heap sub-objects (kernel_stack, page_table_,
    ///        msg_queue, etc.) are NOT included — they survive via the
    ///        pointer array restore and MemPool/PMM restoration.
    struct TaskFields {
        uint64_t magic; ///< TCB_MAGIC for validity check
        uint64_t id;    ///< Task ID for matching on restore
        uint64_t parent_id;
        TaskState state;
        uint64_t priority;
        uint64_t base_priority;
        uint64_t period_ticks;
        uint64_t deadline_ticks;
        bool deadline_missed;
        uint64_t deadline_miss_count;
        uint64_t executed_ticks;
        uint64_t remaining_ticks;
        uint64_t exit_code;
        TaskContext context;       ///< Full register context (critical: rsp)
        uint64_t kernel_stack;     ///< Kernel stack base pointer
        uint64_t kernel_stack_top; ///< For RSP-range validation
        uint64_t waiting_child_pid;
        uint64_t waiting_child_status;
        uint64_t pending_signals;
        uint64_t alarm_ticks;
        bool alarm_armed;
        /// @brief Ready-queue intrusive list pointers (POD copy).
        ///        These form doubly-linked lists; TCBs are in-place across
        ///        snapshot cycles so pointer values remain valid.
        TaskControlBlock *runq_next;
        TaskControlBlock *runq_prev;
        bool in_ready_queue;
        uint64_t rq_priority;
    };
    static uint64_t snapshot_task_fields_size() {
        return sizeof(TaskFields) * MAX_TASKS;
    }

    /// @brief Capture per-task plain fields into the snapshot buffer.
    ///        Called from test_isolate's snapshot_create.
    static void capture_task_fields(TaskFields *out);

    /// @brief Restore per-task plain fields from the snapshot buffer onto
    ///        existing task objects (matched by ID).  Called from
    ///        test_isolate's snapshot_restore after restore_state().
    static void restore_task_fields(const TaskFields *saved);
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static void capture_state(TaskControlBlock **tasks_out,
                              TaskControlBlock **id_table_out,
                              uint64_t &task_count_out,
                              uint64_t &current_idx_out, uint64_t &next_id_out,
                              TaskControlBlock *&idle_out, bool &preempt_out,
                              uint64_t *rq_bitmap_hi = nullptr,
                              uint64_t *rq_bitmap_lo = nullptr,
                              uint64_t *sporadic_count_out = nullptr);
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static void restore_state(TaskControlBlock *const *tasks_in,
                              TaskControlBlock *const *id_table_in,
                              uint64_t task_count_in, uint64_t current_idx_in,
                              uint64_t next_id_in, TaskControlBlock *idle_in,
                              bool preempt_in, uint64_t rq_bitmap_hi = 0,
                              uint64_t rq_bitmap_lo = 0,
                              uint64_t sporadic_count_in = 0);

    /// @brief Capture the full ReadyQueueManager POD into @p out.
    static void capture_rqpod(ReadyQueuePOD &out) noexcept;
    /// @brief Restore the full ReadyQueueManager POD from @p src.
    static void restore_rqpod(const ReadyQueuePOD &src) noexcept;
    /// @brief Clear all ready-queue entries.  Called after reap_orphans()
    ///        in reload_daemon_tasks() to remove any stale TCB pointers
    ///        before restarting daemons.
    static void reset_ready_queue() noexcept;
    /// @brief Rebuild the ready queue from tasks_[], enqueuing all
    ///        tasks with state == READY.  Used after restore_rqpod()
    ///        to ensure in_ready_queue_ flags match the actual queue.
    static void rebuild_ready_queue() noexcept;
    /// @brief Reset the task-ID counter (used by reboot_from_table()
    ///        so init gets PID 1 after the test suite finishes).
    static void reset_next_task_id(uint64_t id) noexcept;

    /// @brief Release a terminated task into the zombie list for deferred
    ///        cleanup by the idle task.  Removes from all scheduler tables
    ///        (all_tasks_, deadline_list_, id_table_) but does NOT free
    ///        resources — that happens in cleanup_step().
    ///        Caller must hold scheduler_lock_.
    ///        Precondition: task.state == TERMINATED, task NOT in ready queue.
    static void release_zombie(TaskControlBlock &task) noexcept;

    /// @brief Invalidate the pending deferred-switch arm (clear all switch
    ///        atoms + bump the switch generation).  Used by the apply-side
    ///        liveness re-check (scheduler_validate_pending_switch) and by
    ///        release_zombie/reap_orphans when the armed target is removed.
    static void cancel_pending_switch() noexcept;

    /// @brief Drain up to @p max_flush zombies from the zombie list,
    ///        calling cleanup() + MemPool::free() on each.
    ///        Used by the on_tick watchdog when zombie_count_ exceeds
    ///        the starvation limit, and by drain_zombie_list().
    ///        Caller must hold scheduler_lock_ or have IRQs disabled.
    static void flush_zombies(uint64_t max_flush) noexcept;

    /// @brief Drain ALL zombies from the list synchronously.
    ///        Takes scheduler_lock_ internally.
    static void drain_zombie_list() noexcept;

    /// @brief Idle-task cleanup step: pop one zombie from the list and
    ///        free its resources.  Called from the idle main loop once
    ///        per iteration.  Safe to call with IRQs enabled — the pop
    ///        is IRQ-guarded internally.
    static void cleanup_step() noexcept;

    /// @brief Clear the zombie list (used by snapshot_restore to prevent
    ///        dangling pointers after MemPool restoration).
    /// @brief Return the current zombie count (for tests).
    static uint64_t zombie_count() noexcept {
        return zombie_count_;
    }

    static void reset_zombie_list() noexcept {
        zombie_head_ = nullptr;
        zombie_tail_ = nullptr;
        zombie_count_ = 0;
    }

    /// @brief Deferred-kill: add a task to the deferred kill list for
    ///        safe cleanup outside the try_lock critical section.
    static void defer_kill(TaskControlBlock *task) noexcept;
    /// @brief Process all deferred kills: remove_task, cleanup, free.
    ///        Called from on_tick() after the deadline scan lock is released.
    static void process_deferred_kills() noexcept;

  private:
    static constexpr uint64_t MAX_TASKS = CONFIG_MAX_TASKS;
    static constexpr uint64_t ID_TABLE_SIZE =
        static_cast<uint64_t>(CONFIG_MAX_TASKS) * 2;
    static constexpr uint64_t ID_TABLE_MASK = ID_TABLE_SIZE - 1;

    /// @brief Sentinel value for a removed hash-table entry.
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static TaskControlBlock *const ID_TOMBSTONE;

    /// @brief All registered tasks, grouped by priority in per-priority
    ///        intrusive doubly-linked lists with O(1) bitmap lookup.
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static AllTasksRegistry all_tasks_;
    /// @brief Pointer to the currently executing task.  PfA-B: backed by
    ///        CpuContext::current (per-CPU).  Reads use current_task();
    ///        writes use set_current_ptr().  INV-1: the RSP-ownership scan in
    ///        switch_to_task remains authoritative.
    static void set_current_ptr(TaskControlBlock *t) noexcept {
        current_cpu().current = t;
    }
    static constinit TaskControlBlock *id_table_[ID_TABLE_SIZE];
    static constinit uint64_t next_task_id_;
    static constinit uint64_t sporadic_task_count_;
    static constinit bool preempt_enabled_;
#if CONFIG_MEMORY_BUDGET
    static constinit uint64_t memory_budget_pages_;
#endif
    static constinit bool suppress_terminated_log_;

    /// @brief Injected test-runner context (PfA-A).  nullptr in production so
    ///        all test flags resolve to their compile-time-false defaults.
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static TestContext *test_context_;

    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static sync::SpinLock scheduler_lock_;
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static ReadyQueueManager ready_queue_;
    /// @brief Deadline-ordered intrusive list for O(1) expired-task detection.
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static DeadlineList deadline_list_;
    static constinit TaskControlBlock *idle_task_;
    static constinit TaskControlBlock *shell_task_ptr_;
    static constinit TaskControlBlock *harness_task_ptr_;

    /// @brief Zombie list — intrusive singly-linked list of terminated
    ///        tasks awaiting deferred cleanup by the idle task.
    ///        push = release_zombie (under scheduler_lock_), pop =
    ///        cleanup_step (IRQ-guarded) or flush_zombies (under lock).
    static constinit TaskControlBlock *zombie_head_;
    static constinit TaskControlBlock *zombie_tail_;
    static constinit uint64_t zombie_count_;
#if CONFIG_DEADLINE_MONITOR_TASK
    /// @brief Pointer to the deadline-monitor task (nullptr if not spawned).
    static constinit TaskControlBlock *s_monitor_task_;
    /// @brief Atomic handoff flag — on_tick() sets it, monitor_task_entry()
    ///        consumes it via atomic exchange (lock-free, no spinlock needed).
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static bool s_scan_requested_;
    /// @brief Set by the test runner before executing test functions; cleared
    ///        afterward.  When true, on_tick() skips the monitor-wake path
    ///        (preventing the timer ISR from switching to the monitor during
    ///        a test, which would hang the test framework).
    ///        PfA-A: stored in the injected TestContext (see set_test_active).
#endif

    /// @brief Performs rate-monotonic scheduling decision.
    static void rate_monotonic_schedule() noexcept;

    /// @brief Hash-table helpers for O(1) task-ID→TCB lookup.
    static uint64_t id_table_probe(uint64_t id);
    static bool id_table_insert(uint64_t id, TaskControlBlock *tcb);
    static void id_table_remove(TaskControlBlock *task);
    static TaskControlBlock *id_table_find(uint64_t id);
};

extern "C" {
/// @brief Address to save the current RSP into during context switch.
///        Accessed atomically from C++; read/written by isr_stubs.asm.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t *scheduler_save_rsp_to;
/// @brief RSP value to load during context switch restore.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_load_rsp_from;
/// @brief CR3 value to load during context switch restore (0 = don't load).
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_load_cr3_from;
/// @brief Task ID to set as current after the context switch completes.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_next_task_id;
/// @brief Kernel-stack range of the task being dispatched.  isr_stubs.asm
///        verifies scheduler_load_rsp_from lies within
///        [scheduler_load_kstack_base, scheduler_load_kstack_top) before iretq.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_load_kstack_base;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_load_kstack_top;
/// @brief Generation sequence counter for the deferred-switch pair.  A
///        publisher bumps it after writing load_rsp_from / load_cr3_from and
///        before arming scheduler_save_rsp_to; isr_stubs.asm captures it and
///        re-verifies before applying so a timer ISR never applies a
///        half-written / superseded pair.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_switch_generation;
/// @brief Static kernel PML4 (physical).  isr_stubs.asm falls back to this when
///        scheduler_load_cr3_from is null/outdated while returning to the
///        kernel/harness context.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_kernel_cr3;
/// @brief Reschedule request flag.  Set by task-context callers (reschedule(),
///        terminate(), yield_as) to ask the timer ISR to (re)publish a deferred
///        switch.  The timer ISR is the SOLE publisher of the deferred switch
///        (see rate_monotonic_schedule), so there is exactly one writer of the
///        switch buffer per tick — eliminating the two-publisher race where a
///        task-context reschedule() and the timer ISR's rate_monotonic_schedule
///        both called switch_to_task and the last writer before the epilogue
///        won, starving the other's selection.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern bool scheduler_need_resched;
/// @brief Current ISR nesting depth.  Incremented at each ISR entry,
///        decremented before iretq.  Checked by on_tick() to detect
///        nested timer interrupts and skip re-entrant scheduler ops.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t isr_nesting_depth;
/// @brief Monotonic counter incremented on every detected scheduler corruption
///        (invalid TCB magic, RSP outside kernel-stack range, etc).
///        Reset to zero in test_isolate restore; test framework fails any test
///        where the counter advanced.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t scheduler_corruption_count;
/// @brief Monotonic counter incremented on each successful deadline-
///        detection scan in on_tick().  Used by tests to verify that
///        the scan completes without aborting.  If a test expects the
///        scan to run (e.g. after setting up a deadline miss) but the
///        counter did not advance, the try_lock was contended or a
///        corruption panic aborted the scan.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern uint64_t deadline_detection_integrity;
/// @brief Tracks which task's FPU state is currently in the registers.
///        nullptr means no task has used FPU since boot.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern TaskControlBlock *fpu_owner;
}

#if CONFIG_DEADLINE_MISS_DETECTION
/// @brief Weak callback invoked when a task misses its deadline.
/// Called from ISR context (on_tick) or task context (deadline-monitor).
/// In ISR context: must not block; KILL action defers via defer_kill().
/// In task context (CONFIG_DEADLINE_MONITOR_TASK): may hold scheduler_lock_
/// and perform inline cleanup.
__attribute__((weak)) void
deadline_miss_handler(TaskControlBlock &task,
                      uint64_t missed_by_ticks) noexcept;
#endif

#if CONFIG_WCET_OVERRUN_DETECTION
/// @brief Weak callback invoked when a task exceeds its WCET.
/// Called from ISR context (on_tick) — must not block or allocate.
__attribute__((weak)) void
wcet_overrun_handler(TaskControlBlock *task,
                     uint64_t overrun_by_ticks) noexcept;
#endif

} // namespace kernel
