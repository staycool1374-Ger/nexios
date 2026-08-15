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

/// @file task.hpp
/// @brief Task control block and state definitions for the kernel scheduler.

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/task_errors.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <signal.hpp>

namespace kernel {

// Forward declaration for init_task_common
struct TaskControlBlock;
/// @brief Initialize common task fields after allocation.
void init_task_common(TaskControlBlock &tcb);

/// @brief Maximum payload size in bytes for an IPC message.
static constexpr size_t IPC_MAX_MSG_SIZE = CONFIG_IPC_MAX_MSG_SIZE;
/// @brief Maximum number of messages in a single queue.
static constexpr size_t IPC_MAX_QUEUE_MSG = CONFIG_IPC_MAX_QUEUE_MSG;
/// @brief Number of priority levels (0 = highest urgency).
static constexpr size_t IPC_PRIORITY_LEVELS = CONFIG_IPC_PRIORITY_LEVELS;

/// @brief A single IPC message with sender ID, type, priority, and payload.
struct Message {
    uint64_t sender_id;
    uint64_t type;
    uint64_t priority;
    uint8_t data[IPC_MAX_MSG_SIZE];
    size_t data_size;
    uint64_t buf_handle = 0;
};

/// @brief Priority-ordered circular message queue embedded in each TCB.
struct MessageQueue {
    Message msgs[IPC_MAX_QUEUE_MSG];
    uint64_t prio_bitmap;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t count;

    TaskControlBlock *blocked_senders_head;
    TaskControlBlock *blocked_senders_tail;

    TaskControlBlock *owner;

    MessageQueue()
        : prio_bitmap(0), head(0), tail(0), count(0),
          blocked_senders_head(nullptr), blocked_senders_tail(nullptr),
          owner(nullptr) {
    }

    ~MessageQueue();

    sync::SpinLock lock_;

    void init();
    bool push(const Message &msg);
    bool pop(Message &msg);

    bool is_empty() const {
        return __atomic_load_n(&count, __ATOMIC_RELAXED) == 0;
    }
    bool is_full() const {
        return __atomic_load_n(&count, __ATOMIC_RELAXED) >= IPC_MAX_QUEUE_MSG;
    }
    bool is_full_locked() {
        SpinLockGuard<sync::SpinLock> guard(lock_);
        return count >= IPC_MAX_QUEUE_MSG;
    }

    size_t highest_priority() const;
};

namespace sync {
class Mutex;
class Semaphore;
class EventGroup;
class Queue;
} // namespace sync

namespace task {
class SporadicServer;
void dmesg_task_main();
} // namespace task

namespace cap {
class CNode;
} // namespace cap

/// @brief States a task can be in during its lifecycle.
enum class TaskState : uint8_t {
    READY,
    RUNNING,
    BLOCKED,
    WAITING,
    TERMINATED,
    REAPED,
};

/// @brief CPU register save area for context switching.
#if defined(CONFIG_ARCH_X86_64)
struct TaskContext {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};
#elif defined(CONFIG_ARCH_AARCH64)
struct TaskContext {
    uint64_t x[31];      // X0-X30
    uint64_t sp_el0;     // SP_EL0 (user stack pointer)
    uint64_t elr_el1;    // ELR_EL1 (return address)
    uint64_t spsr_el1;   // SPSR_EL1 (processor state)
    uint64_t vector;     // exception vector number
    uint64_t error_code; // exception error code
};
#elif defined(CONFIG_ARCH_RISCV64)
struct TaskContext {
    uint64_t ra, sp, gp, tp;
    uint64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint64_t sepc;       // exception return address
    uint64_t sstatus;    // supervisor status register
    uint64_t stvec;      // trap vector base
    uint64_t vector;     // exception vector number
    uint64_t error_code; // exception error code
};
#endif

/// @brief Task control block — represents a single thread of execution.
/// @note Includes scheduling parameters, register context, and stack info.
struct TaskControlBlock {
    static constexpr size_t STACK_SIZE = CONFIG_STACK_SIZE;
    static constexpr uint64_t KERNEL_CS = 0x08;
    static constexpr uint64_t KERNEL_SS = 0x10;
    static constexpr uint64_t USER_CS = 0x1B;
    static constexpr uint64_t USER_SS = 0x23;
    static constexpr uint64_t FLAGS_IF = 0x200;
    static constexpr uint64_t TCB_MAGIC = 0x5443424D41474943ULL;

    /// @brief Sentinel for aperiodic tasks (idle, deadline-monitor, IRQ
    /// threads): no real period/deadline.  Stored in `period_ticks`.
    static constexpr uint64_t NO_PERIOD = 0xFFFFFFFFULL;

    /// @brief True if `t` is a live, valid TCB.
    ///        The address-range check MUST come first: a corrupted list
    ///        pointer (e.g. a freed/overwritten node whose runq_/pri_ links
    ///        hold a low poison value such as 0xDD or 0x55) would otherwise
    ///        fault with a General-Protection Fault the moment we read
    ///        t->magic.  MemPool::free() poisons freed blocks with 0xDD under
    ///        CONFIG_DEBUG, so a freed/reused TCB fails this check and can be
    ///        skipped safely instead of being dereferenced into a #GP.
    static inline bool is_valid(const TaskControlBlock *t) noexcept {
        if (reinterpret_cast<uint64_t>(t) < 0xFFFF800000000000ULL)
            return false;
        return t->magic == TCB_MAGIC;
    }
    /// @brief Human-readable name for logging / debugging.
    char name[CONFIG_TASK_NAME_LEN];

#ifdef CONFIG_DEBUG
    /// @brief One entry in the per-TCB context-switch trace ring buffer.
    struct DebugSwitchRecord {
        uint64_t entry_addr; ///< return address at switch_to_task() call site
        uint64_t exit_rip;   ///< saved RIP from context (where task was last
                             ///< interrupted)
        TaskContext regs;    ///< saved register set from context
        uint64_t thread_id;  ///< this task's ID
        uint64_t consumed_ticks; ///< executed_ticks at switch-out
    };
    static constexpr size_t DEBUG_SWITCH_RING_SIZE = 4;
    DebugSwitchRecord debug_switch_ring[DEBUG_SWITCH_RING_SIZE];
    uint64_t debug_switch_idx;
#endif

    TaskControlBlock()
        :
#ifdef CONFIG_DEBUG
          debug_switch_idx(0),
#endif
          id(0), parent_id(0), state(TaskState::READY), priority(0),
          base_priority(0), period_ticks(0), deadline_ticks(0),
          deadline_missed(false), deadline_miss_count(0), executed_ticks(0),
          remaining_ticks(0), wcet_ticks(0), wcet_overrun_fired(false),
          ss_state_on_deadline_miss(0), ss_budget_on_deadline_miss(0),
          exit_code(0), context({}), kernel_stack(nullptr), kernel_stack_top(0),
          stack_phys_(0), kstack_slot_va_(0), kstack_slot_size_(0),
          page_table_(0), stack_pdpt_phys_(0), user_stack_(0),
          user_stack_size_(0), user_data(nullptr), is_user_(false),
          canary_before{0, 0, 0, 0}, canary_after{0, 0, 0, 0},
          canary_installed(0), fpu_used(false), fpu_state{}, program_break(0),
          program_break_start(0), fd_table({}), cwd_vnode(nullptr),
          runq_next_(nullptr), runq_prev_(nullptr), dl_next_(nullptr),
          dl_prev_(nullptr), pri_next_(nullptr), pri_prev_(nullptr),
          in_ready_queue_(false), rq_priority_(0), all_bucket_(0),
          zombie_next_(nullptr), waiting_child_pid(0),
          waiting_child_status(nullptr), pending_signals(0), alarm_ticks(0),
          alarm_armed(false), sporadic_server(nullptr), cspace_(nullptr),
          buf_list_head(0), task_obj_head_(nullptr), task_obj_tail_(nullptr),
          blocked_next(nullptr), blocked_prev(nullptr),
          blocked_on_queue(nullptr), reply_wait(false),
          waiting_on_mutex(nullptr), waiting_on_semaphore(nullptr),
          waiting_on_eventgroup(nullptr), waiting_on_queue(nullptr),
          held_ceiling_depth_(0), system_ceiling_(0), first_child(nullptr),
          next_sibling(nullptr), prev_sibling(nullptr), num_children(0),
          generation(0) {
    }

    uint64_t magic;
    uint64_t id;
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
    uint64_t
        wcet_ticks; ///< explicit WCET for utilisation calc; 0 = implicit 100%
    bool wcet_overrun_fired; ///< latch to fire WCET handler once per period
    uint8_t ss_state_on_deadline_miss; ///< captured SporadicServer::state() at
                                       ///< deadline miss
    uint64_t ss_budget_on_deadline_miss; ///< captured remaining_budget() at
                                         ///< deadline miss
    uint64_t exit_code;

    TaskContext context;
    uint8_t *kernel_stack;
    uint64_t kernel_stack_top;
    uint64_t stack_phys_;

    /// @brief Virtual address of the kernel-stack window slot (0 = HHDM).
    ///        Includes guard page at slot_va.
    uint64_t kstack_slot_va_;
    /// @brief Total slot size (stack + guard page).  0 if HHDM.
    uint64_t kstack_slot_size_;
    uint64_t page_table_;
    /// @brief Physical address of the private PDPT page allocated in
    /// clone() for the user stack region. Zero when not applicable.
    /// Used by cleanup() to free the private PDPT and its child PD/PT
    /// pages that would otherwise leak.
    uint64_t stack_pdpt_phys_;
    uint64_t user_stack_;
    uint64_t user_stack_size_;
    void *user_data;
    /// @brief True for user-mode tasks (own user half in page_table_).
    ///        v0.4.0 MP-1: every task — kernel AND user — now owns a private
    ///        PML4 whose kernel half is copied from the kernel PML4, so
    ///        `page_table_ != 0` is no longer a valid user-task
    ///        discriminator.  This flag is the authoritative one.
    bool is_user_;

    // ------------------------------------------------------------------
    // Software sentinel canaries (v0.4.0 MP-3)
    // ------------------------------------------------------------------
    /// @brief Base magic for segment-boundary canaries.
    static constexpr uint64_t CANARY_MAGIC = 0x4E45584943414E59ULL;

    /// @brief User-visible segment kinds guarded by a before/after sentinel.
    enum CanarySegment : uint8_t {
        SEG_TEXT = 0,
        SEG_DATA = 1,
        SEG_HEAP = 2,
        SEG_STACK = 3,
        CANARY_SEGMENTS = 4,
    };

    /// @brief VA of the canary slot before each segment (0 = not installed).
    ///        The expected value is derived: CANARY_MAGIC ^ (segment + 1).
    uint64_t canary_before[CANARY_SEGMENTS];
    /// @brief VA of the canary slot after each segment (0 = not installed).
    uint64_t canary_after[CANARY_SEGMENTS];
    /// @brief Bitmask: bit i set when segment i's canaries are installed;
    ///        bit CANARY_SEGMENTS set when the kernel-stack canary is armed.
    uint8_t canary_installed;

    /// @brief FPU/SSE save area (FXSAVE/FXRSTOR — 512 bytes, 16-byte aligned).
    ///        Zeroed on task creation; populated lazily via #NM handler.
    bool fpu_used;
    alignas(16) uint8_t fpu_state[512];

    uint64_t program_break;
    uint64_t program_break_start;

    /// @brief Maximum pages this task may allocate from PMM.  0 = unlimited.
    uint64_t memory_budget_pages_;
    /// @brief Current pages charged to this task.
    uint64_t memory_used_pages_;

    vfs::FdTable fd_table;
    char cwd[CONFIG_VFS_MAX_PATH];
    vfs::Vnode *cwd_vnode;
    /// @brief Guards the cwd_vnode read-modify-write in sys_chdir (VULN-C5/C6).
    sync::SpinLock cwd_lock_;

    /// @brief Intrusive linked-list pointers for O(1) ready queue.
    TaskControlBlock *runq_next_;
    TaskControlBlock *runq_prev_;

    /// @brief Intrusive linked-list pointers for DeadlineList
    /// (sorted by deadline_ticks ascending).
    TaskControlBlock *dl_next_;
    TaskControlBlock *dl_prev_;

    /// @brief Intrusive linked-list pointers for AllTasksRegistry
    /// (per-priority doubly-linked list of all tasks).
    TaskControlBlock *pri_next_;
    TaskControlBlock *pri_prev_;
    /// @brief True if this task is currently in the ready queue.
    /// Used to prevent double-enqueue.
    bool in_ready_queue_;
    /// @brief Priority at which this task was enqueued in the ready queue.
    uint64_t rq_priority_;
    /// @brief Priority bucket in AllTasksRegistry (set at append, never
    /// changes).
    uint64_t all_bucket_;

    /// @brief Singly-linked list pointer for the zombie list.
    /// Non-null only while the TCB is in the zombie list (between
    /// release_zombie and idle cleanup_step).
    TaskControlBlock *zombie_next_ = nullptr;

    uint64_t waiting_child_pid;
    uint64_t *waiting_child_status;

    /// @brief Signal handler table — one handler per signal number (max 32).
    ///        nullptr means default action (terminate on fatal signals).
    sighandler_t signal_handlers[MAX_SIGNAL_HANDLERS];

    /// @brief Pending signal bitmask (bit n set = signal n is pending).
    uint64_t pending_signals;

    /// @brief Alarm expiration tick (absolute tick count when alarm fires).
    uint64_t alarm_ticks;

    /// @brief True if alarm is armed.
    bool alarm_armed;

    /// @brief Embedded message queue (no separate heap allocation).
    MessageQueue msg_queue;

    /// @brief Per-task notification object (embedded).
    sync::Notify notify;

    /// @brief Per-task event-group object (embedded).
    sync::EventGroup event_group;

    /// @brief Optional per-task SporadicServer for aperiodic
    /// daemon budget management (vfsd, iocd).  nullptr for normal tasks.
    /// Allocated from MemPool in init_sporadic_server() and attached to
    /// the intrusive object list (task_obj_head_/task_obj_tail_).  This
    /// pointer is an O(1) read cache kept in sync with the list by
    /// attach_object()/detach_object(); ownership lives in the list.
    task::SporadicServer *sporadic_server;

    /// @brief This task's root CNode (its CSpace), lazily created on first
    ///        capability use (cap::ensure_cspace).  nullptr for tasks that
    ///        never touch capabilities.  Allocated from MemPool, pool-backed,
    ///        and attached to the intrusive object list (task_obj_head_/
    ///        task_obj_tail_); this pointer is an O(1) read cache kept in
    ///        sync with the list by attach_object()/detach_object() — the
    ///        same ownership model as sporadic_server.
    cap::CNode *cspace_;

    /// @brief Head of doubly-linked list of buffer handles owned by
    /// this task. -1 means the list is empty. Used by the BufferPool
    /// for zero-copy IPC.
    int32_t buf_list_head;

    /// @brief Head of the intrusive doubly-linked list of heap-allocated,
    /// pool-backed KernelObject objects owned by this task (e.g. its
    /// SporadicServer).  The list is the single source of truth for
    /// per-task object lifecycle: teardown walks this list and releases
    /// every node; snapshot restore detaches every node without releasing.
    /// Only mutated outside IRQ context (before add_task, or inside
    /// cleanup() under scheduler_lock_ / IrqGuard).
    KernelObject *task_obj_head_;
    KernelObject *task_obj_tail_;

    /// @brief Linked-list pointers for the blocked-sender chain
    /// (singly linked via next).
    TaskControlBlock *blocked_next;
    TaskControlBlock *blocked_prev;

    /// @brief Pointer to the message queue this task is blocked on
    /// (as a sender).  Non-null only while the task is in another
    /// task's blocked_senders list.  Used by cleanup() to detach
    /// before the TCB is freed, preventing dangling pointers in the
    /// queue's list.
    MessageQueue *blocked_on_queue;

    /// @brief True while a task is blocked inside IPC::send_sync waiting for a
    /// reply on its own message queue.  Lets IPC::send wake a reply-waiter
    /// when its reply is delivered (otherwise send_sync senders are never
    /// unblocked and hang forever).
    bool reply_wait;

    /// @brief Pointer to the mutex this task is blocked on (as a waiter).
    /// Used for transitive priority inheritance propagation.
    /// Non-null only while the task is in a Mutex's waiter array.
    /// Set in Mutex::lock(), cleared in Mutex::wake_one().
    sync::Mutex *waiting_on_mutex;

    /// @brief Pointer to the semaphore this task is blocked on (as a waiter).
    /// Non-null only while the task is in a Semaphore's waiter array.
    /// Set in Semaphore::add_waiter(), cleared in Semaphore::wake_one() and
    /// Semaphore::remove_waiter().  Used by cleanup() to detach a reaped task
    /// from a semaphore's waiter list before the TCB is freed, preventing a
    /// dangling waiter entry from being re-queued by a later post().
    sync::Semaphore *waiting_on_semaphore;

    /// @brief Pointer to the event group this task is blocked on (as a waiter).
    /// Non-null only while the task is in an EventGroup's waiter array.
    /// Set in EventGroup::add_waiter(), cleared in EventGroup::wake_matching(),
    /// ~EventGroup() and EventGroup::remove_waiter().  Used by cleanup() to
    /// detach a reaped task before the TCB is freed, preventing a dangling
    /// waiter entry from being re-queued by a later set_bits().
    sync::EventGroup *waiting_on_eventgroup;

    /// @brief Pointer to the message queue this task is blocked on (as a
    /// sender or receiver).  Non-null only while the task is in a Queue's
    /// send- or recv-waiter array.  Set in Queue::add_send_waiter() /
    /// add_recv_waiter(), cleared in the wake/remove paths.  Used by cleanup()
    /// to detach a reaped task before the TCB is freed.
    sync::Queue *waiting_on_queue;

    /// @brief Number of mutexes currently held by this task (for PCP ceiling
    /// tracking).
    size_t held_ceiling_depth_;
    /// @brief Stack of priority_ceiling values of all held mutexes.
    uint64_t held_ceilings_[CONFIG_MAX_HELD_CEILINGS];
    /// @brief Current system ceiling — max priority_ceiling of all held
    /// mutexes. 0 when no mutex is held. Used by PCP to block lock attempts.
    uint64_t system_ceiling_;

    /// @brief Process hierarchy: first child, next sibling, previous sibling.
    TaskControlBlock *first_child;
    TaskControlBlock *next_sibling;
    TaskControlBlock *prev_sibling;

    /// @brief Number of live child processes.
    uint64_t num_children;

    /// @brief Monotonically-increasing generation counter, incremented at
    ///        every task creation.  Stored in waiter arrays alongside the
    ///        task pointer to detect stale references (use-after-reuse).
    uint32_t generation;

    /// @brief Checks if the task has a handler registered for a signal.
    bool has_signal_handler(uint64_t sig) const {
        return sig < MAX_SIGNAL_HANDLERS && signal_handlers[sig] != nullptr;
    }

    /// @brief Registers a signal handler for the given signal.
    void set_signal_handler(uint64_t sig, sighandler_t handler) {
        if (sig < MAX_SIGNAL_HANDLERS)
            signal_handlers[sig] = handler;
    }

    /// @brief Returns the registered handler for the signal, or nullptr.
    sighandler_t get_signal_handler(uint64_t sig) const {
        return (sig < MAX_SIGNAL_HANDLERS) ? signal_handlers[sig] : nullptr;
    }

    /// @brief Creates a new kernel task with the given entry and
    /// scheduling parameters.
    static TaskControlBlock *create(void (*entry)(), uint64_t priority,
                                    uint64_t period_ticks);
    /// @brief Creates a new kernel task with error code.
    /// @param entry Entry function for the task.
    /// @param priority Task priority.
    /// @param period_ticks Task period in ticks.
    /// @param[out] out_tcb Pointer to the created TCB on success.
    /// @return TaskError code.
    static errors::TaskError create_err(void (*entry)(), uint64_t priority,
                                        uint64_t period_ticks,
                                        TaskControlBlock *&out_tcb);

    /// @brief Creates a new user task running in ring 3 with its own
    /// page table.
    static TaskControlBlock *create_user(void (*entry)(), uint64_t priority,
                                         uint64_t period_ticks,
                                         size_t user_stack_size = 32_KiB);
    /// @brief Creates a new user task with error code.
    /// @param entry Entry function for the task.
    /// @param priority Task priority.
    /// @param period_ticks Task period in ticks.
    /// @param user_stack_size Size of the user stack (default 32 KiB).
    /// @param[out] out_tcb Pointer to the created TCB on success.
    /// @return TaskError code.
    static errors::TaskError create_user_err(void (*entry)(), uint64_t priority,
                                             uint64_t period_ticks,
                                             size_t user_stack_size,
                                             TaskControlBlock *&out_tcb);

    /// @brief Clones the current task — creates a child with copied
    /// context.
    /// @param regs Register save area from the interrupt (to copy
    /// RIP/RSP/regs).
    /// @return Pointer to the new child TaskControlBlock, or nullptr
    /// on failure.
    static TaskControlBlock *clone(uint64_t *regs);
    /// @brief Clones the current task with error code.
    /// @param regs Register save area from the interrupt.
    /// @param[out] out_tcb Pointer to the created TCB on success.
    /// @return TaskError code.
    static errors::TaskError clone_err(uint64_t *regs,
                                       TaskControlBlock *&out_tcb);

    /// @brief Save the current register context into this TCB.
    /// @param rsp Reference to current stack pointer (updated on save).
    void save_context(uint64_t &rsp) noexcept;
    /// @brief Restore the saved register context into CPU registers.
    /// @param rsp Reference to restore the stack pointer into.
    void restore_context(uint64_t &rsp) noexcept;

    /// @brief Frees all resources owned by this task (FDs, pages, page tables).
    ///        Called by WAITPID after reaping a TERMINATED child.
    void cleanup() noexcept;

    /// @brief Unregisters this TCB from the scheduler (tasks_[] / id_table_ /
    ///        ready queue) and returns its memory to the MemPool.
    ///        Mandatory before the block is reused: a TCB freed via `delete`
    ///        without unregistration leaves a dangling tasks_[] entry that
    ///        aliases the next allocation (use-after-free dispatch).
    void operator delete(void *ptr) noexcept;

    /// @brief Allocates and initialises a SporadicServer for this task
    ///        (e.g. a Ring 3 daemon like vfsd / iocd).
    /// @param budget_c           Execution budget per period (ticks).
    /// @param period_t           Replenishment period (ticks).
    /// @param bg_prio            Priority level when budget exhausted.
    /// @param budget_granularity Ticks per budget unit (default:
    /// CONFIG_SPORADIC_SERVER_BUDGET_GRANULARITY).
    void init_sporadic_server(
        uint64_t budget_c, uint64_t period_t, uint64_t bg_prio,
        uint64_t budget_granularity =
            CONFIG_SPORADIC_SERVER_BUDGET_GRANULARITY) noexcept;

    /// @brief Attaches @p obj to this task's intrusive object list.
    ///        The task takes ownership of exactly one reference
    ///        (refcount == 1 at attach).  O(1), head insert.
    void attach_object(KernelObject *obj) noexcept;

    /// @brief Unlinks @p obj from this task's object list WITHOUT releasing
    ///        it.  O(1).  Used when ownership transfers or when snapshot
    ///        restore must drop links without freeing (the pool restore has
    ///        already rewound the block).
    void detach_object(KernelObject *obj) noexcept;

    /// @brief Unlinks every node from this task's object list WITHOUT
    ///        releasing any of them.  Bounded by CONFIG_MAX_PER_TASK_OBJECTS
    ///        pop-head iterations (runaway-link protection).  Used by
    ///        snapshot restore where blocks are reclaimed by the pool rewind,
    ///        never by release().
    void detach_all_objects() noexcept;

    /// @brief Releases every node on this task's object list (teardown).
    ///        Runs the kind-specific hook (e.g. Scheduler::dec_sporadic_count)
    ///        then drops the ownership reference, freeing pool-backed blocks.
    ///        Called exactly once from cleanup().
    void release_all_objects() noexcept;

    /// @brief Returns this task's SporadicServer (or nullptr).  O(1) read
    ///        cache kept in sync with the intrusive object list; safe to
    ///        call from ISR context (never mutates).
    task::SporadicServer *get_sporadic_server() const noexcept {
        return sporadic_server;
    }

    /// @brief Returns this task's root CNode (CSpace), or nullptr if it has
    ///        never touched capabilities.  O(1) read cache kept in sync with
    ///        the intrusive object list; safe to call from ISR context.
    cap::CNode *get_cspace() const noexcept {
        return cspace_;
    }

    /// @brief Lazily creates and attaches this task's root CNode (CSpace).
    ///        Idempotent: a second call with an existing CSpace is a no-op.
    ///        Called from task context on first capability use.
    void ensure_cspace() noexcept;

    /// @brief Adds a child to this task's process hierarchy.
    void add_child(TaskControlBlock *child) noexcept;

    /// @brief Removes a child from this task's process hierarchy.
    void remove_child(TaskControlBlock *child) noexcept;

    /// @brief Finds a child by PID.
    TaskControlBlock *find_child(uint64_t pid) noexcept;
};

/// @brief Total snapshot bytes for the kernel-stack window state
///        (v0.4.0 MP-6.3): 8 PT page contents + slot bookkeeping.
/// @return Byte count consumed by kslot_snapshot_capture/restore.
size_t kslot_snapshot_bytes();

/// @brief Capture the kstack-window PT contents and slot bookkeeping into
///        @p dst (must hold >= kslot_snapshot_bytes() bytes).
void kslot_snapshot_capture(uint8_t *dst);

/// @brief Restore the kstack-window PT contents and slot bookkeeping from
///        @p src, and flush the TLB for every window VA.
void kslot_snapshot_restore(const uint8_t *src);

// --- Software sentinel canaries (v0.4.0 MP-3) ---
/// @brief Write an 8-byte canary value at @p va through @p pml4.
void canary_write_at(uint64_t va, uint64_t value, uint64_t pml4);
/// @brief Install the user-stack and initial-heap canaries for @p t.
void canary_install_user_segments(TaskControlBlock *t);
/// @brief Install the kernel-stack canary at kernel_stack[0..8).
void canary_install_kernel_stack(TaskControlBlock *t);
/// @brief Verify all installed user-segment canaries (pure reads).
bool canary_verify_user_segments(const TaskControlBlock *t,
                                 uint8_t &bad_segment, uint64_t &bad_va);
/// @brief Verify the kernel-stack canary (pure read).
bool canary_verify_kernel_stack(const TaskControlBlock *t);

} // namespace kernel
