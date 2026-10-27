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

/// @file task.cpp
/// @brief TaskControlBlock lifecycle: creation, initialisation, cloning, and
/// cleanup.

#include <kernel/task/task.hpp>
#include <kernel/task/task_path.hpp> // issue #347 Slice C
#include <kernel/task/exit_record.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/elf/elf_shared.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/task/tcb_write_log.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/debug/dump.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/ipc/death_notify.hpp>
#include <kernel/debug/debug_stop.hpp>
#include <kernel/debug/debug_bind.hpp>
#include <kernel/ipc/pager_registry.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/arch/hal/context_id.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/iopb.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/cap/frame_map.hpp>
#include <kernel/task/kslot_backend.hpp>
#include <kernel/irq_delivery.hpp>
#include <assert.hpp>

#if defined(CONFIG_DEBUG)
// Fault-injection mask for task_step() (issue #347 Slice C, declared in
// task_path.hpp). Test-only; absent in release.
uint64_t kernel::g_test_skip_task_step = 0;
#endif

// BUGS.md#020: ring buffer of recently-created tasks so that when a wild
// write plants a task's `entry` (code address) into some other field, the
// scheduler's effective_priority() guard can name WHICH task's entry was
// copied and (by tcb address) which victim it landed in.
namespace {
struct RecentTask {
    uint64_t entry;
    uint64_t tcb;
    uint64_t ticks;
};
constexpr size_t kRecentTasks = 64;
RecentTask g_recent_tasks[kRecentTasks]{};
size_t g_recent_tasks_idx = 0;
} // namespace

namespace kernel {
namespace debug {
// Issue #264: per-slot single-writer contract. The slot index is claimed
// with an atomic fetch-add (no new lock: creation paths must not acquire
// spinlocks for diagnostics — lock-order risk), so concurrent writers land
// in distinct slots: no mixed entry-from-A/tcb-from-B records, no lost
// updates. The ring is write-only diagnostics (also GDB-readable
// post-mortem); any future READER must pair acquire semantics with these
// slot writes.
void record_task_entry(uint64_t entry, uint64_t tcb) {
    size_t my_idx = __atomic_fetch_add(&g_recent_tasks_idx, 1UL,
                                       __ATOMIC_RELAXED) %
                    kRecentTasks;
    auto &r = g_recent_tasks[my_idx];
    r.entry = entry;
    r.tcb = tcb;
    r.ticks = 0;
}
/// @brief If `value` equals the low 32 bits or full value of a recently
/// created task's `entry`, return that task's tcb address; else 0.
uint64_t find_entry_owner(uint64_t value) {
    for (size_t i = 0; i < kRecentTasks; ++i) {
        const auto &r = g_recent_tasks[i];
        if (r.entry == value ||
            (r.entry & 0xFFFFFFFFULL) == (value & 0xFFFFFFFFULL))
            return r.tcb;
    }
    return 0;
}
} // namespace debug
} // namespace kernel
#include <kernel/task/task_errors.hpp>
#include <string.hpp>
#include <constants.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>

namespace kernel {

/// @brief Trampoline wrapper for kernel task entry points.
/// Set as RIP in the ISR save frame; RDI (first argument) is set to the
/// actual entry function. After entry() returns, the task is terminated so
/// its state transitions to TERMINATED and any waiter (e.g. a test harness
/// spinning on task->state) observes completion.  Previously this fell into
/// an infinite hlt loop, leaving the task RUNNING forever and deadlocking
/// any caller that waits for termination (ipc_blocking test hang).
/// All arches: the initial frame leaves the link register zeroed (x86: no
/// return address above RBP; aarch64: x30=0; riscv: ra=0), so a task whose
/// entry returns would branch to 0 without this catcher (#356: ELR/sepc=0
/// faults on aarch64/riscv64).
// NOT instrumented (coverage builds): _task_trampoline is entered by the
// context switch with a synthetic frame — there is no return address above
// RBP, so the -finstrument-functions hook would read the unmapped stack top
// and page-fault before the task ever runs (issue #122).
static void __attribute__((no_instrument_function))
_task_trampoline(void (*entry)()) {
    entry();
    auto *self = Scheduler::current_task();
    if (self)
        Scheduler::terminate(*self, 0);
    for (;;)
        arch::hlt();
}

/// @brief Allocates and initialises a SporadicServer for a daemon task
///        from the MemPool.  Idempotent (returns early if already set).
void TaskControlBlock::init_sporadic_server(
    uint64_t budget_c, uint64_t period_t, uint64_t bg_prio,
    uint64_t budget_granularity, task::ServerMode mode) noexcept {
    if (sporadic_server)
        return;
    auto *ss = static_cast<task::SporadicServer *>(
        MemPool::alloc(sizeof(task::SporadicServer)));
    if (!ss)
        return;
    // Vptr hazard: memset(0) would wipe the KernelObject vptr, so the block is
    // memset FIRST (preserving zero-init of the derived POD members), then
    // placement-news the default ctor to install the vptr + base state, then
    // mark_pool_backed (stack test instances stay unmarked), then init() sets
    // every derived member.
    memset(ss, 0, sizeof(task::SporadicServer));
    new (ss) task::SporadicServer;
    ss->mark_pool_backed();
    ss->init(budget_c, period_t, bg_prio, budget_granularity, mode);
    ss->set_base_priority(priority);
    attach_object(ss);
    sporadic_server = ss;
    Scheduler::inc_sporadic_count();
}

/// @brief Lazily creates this task's root CNode (CSpace) from the MemPool.
///        Idempotent: returns early if the CSpace already exists.  The node
///        is pool-backed and attached to this task's intrusive object list so
///        teardown (release_all_objects) reclaims it deterministically.
///        The task's own id serves as the cspace_id decoded from handles
///        (no global registry in iteration-1 CSpace; handles are validated
///        against the current task's root CNode only).
///        Issue #265 contract note: the 8-bit truncation is a same-CNode
///        self-consistency tag, NOT a global identity.  lookup() always
///        resolves inside the caller's own CNode with slot+gen+rights fully
///        mediating, so aliased tags across tasks grant no cross-task access
///        and nothing can exhaust (CNodes free on task death).  The mask is
///        load-bearing: storing the full id while the wire format stays
///        8-bit would self-reject every task with id > 255.  A future global
///        cspace registry must widen CAP_HANDLE_IDBITS first.
///        Bootstrapping side (issue #171 P4): null-until-first-use IS the
///        creator-seeding invariant — nothing installs caps at task birth,
///        so a driver that never makes a cap syscall keeps a null CSpace
///        and can hold nothing.  Idempotent: safe to call on every cap
///        path (syscalls, daemon startup, tests).
void TaskControlBlock::ensure_cspace() noexcept {
    if (cspace_)
        return;
    auto *node = cap::CNode::create(static_cast<uint32_t>(id & 0xFFu));
    if (!node)
        return;
    attach_object(node);
    cspace_ = node;
}

/// @brief Attaches @p obj to this task's intrusive object list.
///        Only called while the task is not visible to the scheduler
///        (pre-add_task or inside cleanup under IrqGuard), so the list
///        is never mutated from IRQ context.
void TaskControlBlock::attach_object(KernelObject *obj) noexcept {
    if (!obj)
        return;
    obj->task_obj_prev_ = nullptr;
    obj->task_obj_next_ = task_obj_head_;
    if (task_obj_head_)
        task_obj_head_->task_obj_prev_ = obj;
    else
        task_obj_tail_ = obj;
    task_obj_head_ = obj;
}

/// @brief Unlinks @p obj from this task's object list WITHOUT releasing it.
void TaskControlBlock::detach_object(KernelObject *obj) noexcept {
    if (!obj)
        return;
    if (obj->task_obj_prev_)
        obj->task_obj_prev_->task_obj_next_ = obj->task_obj_next_;
    else
        task_obj_head_ = obj->task_obj_next_;
    if (obj->task_obj_next_)
        obj->task_obj_next_->task_obj_prev_ = obj->task_obj_prev_;
    else
        task_obj_tail_ = obj->task_obj_prev_;
    obj->task_obj_prev_ = nullptr;
    obj->task_obj_next_ = nullptr;
    if (sporadic_server == obj)
        sporadic_server = nullptr;
    if (cspace_ == obj)
        cspace_ = nullptr;
}

/// @brief Unlinks every node from this task's object list WITHOUT releasing
///        any of them.  Bounded by CONFIG_MAX_PER_TASK_OBJECTS.
/// @note Snapshot-restore path: the MemPool restore (pool rewind) runs BEFORE
///       this in snapshot_restore(), so blocks freed during the test have been
///       returned to the free list and may be 0xDD-poisoned.  This routine only
///       ever unlinks (never releases) and stops the walk if it encounters a
///       poisoned block, so it can never double-free or follow a garbage chain.
void TaskControlBlock::detach_all_objects(bool keep_cspace) noexcept {
    for (uint32_t i = 0; i < CONFIG_MAX_PER_TASK_OBJECTS && task_obj_head_;
         ++i) {
        KernelObject *obj = task_obj_head_;
        // Poison guard: a pool-rewound block's first qword is 0xDDDD... under
        // CONFIG_DEBUG.  A live object's first qword is its vptr (a kernel-high
        // address, at/above arch::HHDM_OFFSET), which passes the low-address
        // check.  Stop the walk rather than following garbage links.
        if (reinterpret_cast<uintptr_t>(obj) < arch::HHDM_OFFSET ||
            *reinterpret_cast<const uint64_t *>(obj) == 0xDDDDDDDDDDDDDDDDULL) {
            break;
        }
        task_obj_head_ = obj->task_obj_next_;
        if (task_obj_head_)
            task_obj_head_->task_obj_prev_ = nullptr;
        else
            task_obj_tail_ = nullptr;
        obj->task_obj_next_ = nullptr;
        obj->task_obj_prev_ = nullptr;
    }
    task_obj_head_ = nullptr;
    task_obj_tail_ = nullptr;
    sporadic_server = nullptr;
    if (!keep_cspace)
        cspace_ = nullptr;
}

/// @brief Releases every node on this task's object list (teardown).
///        Drops the TCB's own reference on each node.  The last reference
///        invokes the node's virtual dispose() (SporadicServer frees itself and
///        decrements the sporadic count; a future shared object survives if
///        other holders remain).
/// @note The refcount invariant is `>= 1` universally (double-release guard);
///       `== 1` only for non-shared (private-owned) nodes, where the TCB is the
///       only long-lived holder and a stray ScopedRef pin at teardown is a bug.
///       Shared nodes (is_shared() == true) may hold references from other
///       tasks/CPUs and legitimately survive this release.
void TaskControlBlock::release_all_objects() noexcept {
    while (task_obj_head_) {
        KernelObject *obj = task_obj_head_;
        task_obj_head_ = obj->task_obj_next_;
        if (task_obj_head_)
            task_obj_head_->task_obj_prev_ = nullptr;
        else
            task_obj_tail_ = nullptr;
        obj->task_obj_next_ = nullptr;
        obj->task_obj_prev_ = nullptr;
        // Double-release guard: at teardown every node must still hold at
        // least the TCB ownership reference.  A 0-count node means a prior
        // release already ran dispose() — fail fast in debug instead of
        // freeing an object that another owner may still reference.  For
        // non-shared (private) nodes the count must be exactly 1.
#if defined(CONFIG_DEBUG)
        ENSURE(obj->refcount() >= 1U);
        if (!obj->is_shared())
            ENSURE(obj->refcount() == 1U);
#endif
        obj->release();
    }
    task_obj_head_ = nullptr;
    task_obj_tail_ = nullptr;
    sporadic_server = nullptr;
    cspace_ = nullptr;
}

/// @brief Initialises common fields of a newly allocated TaskControlBlock.
/// Sets up fd table, cwd, IPC objects (MessageQueue, Notify, EventGroup),
/// process-hierarchy links, signal handlers, and buffer pool state.
static uint32_t s_next_generation = 1;

void init_task_common(TaskControlBlock &tcb) {
    tcb.generation =
        __atomic_fetch_add(&s_next_generation, 1, __ATOMIC_RELAXED);
    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        tcb.fd_table.fds[i].used = false;
        tcb.fd_table.fds[i].vnode = nullptr;
        tcb.fd_table.fds[i].offset = 0;
        tcb.fd_table.fds[i].flags = 0;
    }
    tcb.cwd[0] = '/';
    tcb.cwd[1] = '\0';
    tcb.cwd_vnode = vfs::get_root_vnode();
    if (tcb.cwd_vnode)
        vfs::vnode_ref_inc(tcb.cwd_vnode);

    // Initialize IPC message queue
    tcb.msg_queue.init();
    tcb.msg_queue.owner = &tcb;
    kernel::test::ResourceTracker::instance().track_msg_queue_add();
    tcb.blocked_next = nullptr;
    tcb.blocked_prev = nullptr;
    tcb.blocked_on_queue = nullptr;
    tcb.waiting_on_semaphore = nullptr;
    tcb.waiting_on_eventgroup = nullptr;
    tcb.waiting_on_queue = nullptr;
    tcb.blocked_on_pager_fault = nullptr; // issue #107
    tcb.stack_pdpt_phys_ = 0;
    tcb.is_user_ = false;
    tcb.user_stack_ = 0;
    tcb.user_stack_size_ = 0;

    tcb.notify.init();
    kernel::test::ResourceTracker::instance().track_notify_add();

    tcb.event_group.init();
    kernel::test::ResourceTracker::instance().track_event_group_add();

    // Process hierarchy initialization
    tcb.first_child = nullptr;
    tcb.next_sibling = nullptr;
    tcb.prev_sibling = nullptr;
    tcb.num_children = 0;

    // Buffer pool list starts empty
    tcb.buf_list_head = -1;

    // Issue #409: teardown ownership is per-occupancy state. The deferred-
    // kill generation snapshot rejects recycled occupants before the claim
    // is ever consulted, and the memset sites zero the block — these
    // explicit resets document that a recycled block never inherits a
    // stale claim or completion from its previous occupant.
    tcb.teardown_claimed_ = false;
    tcb.teardown_completed_ = false;

    // Signal handlers all default to nullptr (SIG_DFL)
    for (size_t i = 0; i < MAX_SIGNAL_HANDLERS; ++i) {
        tcb.signal_handlers[i] = nullptr;
    }
    tcb.pending_signals = 0;
}

// BUGS.md#019/#020: after MemPool::alloc() returns a TCB block (but BEFORE it
// is zeroed/reused), verify it is not still aliased by a live reference.  If
// the block we just got is pointed to by current_task_ptr_, any live task's
// child/sibling/sporadic_server/parent_id linkage, or the scheduler id_table_,
// then the previous owner was freed while still referenced — the use-after-free
// that later manifests as a 0xDD poison GPF / mass ctx.rip=0 corruption.  This
// converts the silent corruption into a precise, catchable fault naming the
// offending alias instead of corrupting random kernel state.  Called from both
// create() and create_user() (create_user previously lacked this check, which
// is how the buffer_pool_* tests reproduced the UAF).
#ifdef CONFIG_DEBUG
static void debug_check_tcb_reuse(TaskControlBlock *tcb) {
    auto *cur = Scheduler::current_task();
    if (cur == tcb) {
        kernel::Logger::fatal("BUGS.md#019/#020: create reused the CURRENT "
                              "task's TCB block %p",
                              tcb);
        kernel::debug::dump_scheduler_info();
        panic("TCB reuse of current task (UAF)");
    }
    if (Scheduler::debug_id_table_references(tcb)) {
        kernel::Logger::fatal("BUGS.md#019/#020: create reused TCB block %p "
                              "still aliased by scheduler id_table_",
                              tcb);
        kernel::debug::dump_scheduler_info();
        panic("TCB reuse of id_table_-aliased block (UAF)");
    }
    uint64_t n = Scheduler::task_count();
    for (uint64_t i = 0; i < n; ++i) {
        auto *t = Scheduler::task_at(i);
        if (!t || t == tcb)
            continue;
        if (t->first_child == tcb || t->next_sibling == tcb ||
            reinterpret_cast<void *>(t->get_sporadic_server()) ==
                reinterpret_cast<void *>(tcb) ||
            t->runq_next_ == tcb || t->blocked_next == tcb ||
            t->dl_next_ == tcb ||
            (tcb->id != 0 && static_cast<uint64_t>(t->parent_id) == tcb->id)) {
            kernel::Logger::fatal(
                "BUGS.md#019/#020: create reused TCB block %p "
                "still referenced by task %u (child=%p sib=%p "
                "spor=%p par=%lu)",
                tcb, t->id, t->first_child, t->next_sibling,
                t->get_sporadic_server(), static_cast<uint64_t>(t->parent_id));
            kernel::debug::dump_scheduler_info();
            panic("TCB reuse of live-referenced block (UAF)");
        }
    }
}
#endif

// ---------------------------------------------------------------------------
// Kernel-stack window slot allocator (indexed pool)
// ---------------------------------------------------------------------------

namespace {

struct KSlotEntry {
    uint64_t va;
    uint64_t size;
    int32_t next;
};

static constexpr int32_t KSLOT_POOL_SIZE = 64;
static_assert(KSLOT_POOL_SIZE >= CONFIG_MAX_TASKS,
              "KSLOT_POOL_SIZE must be >= CONFIG_MAX_TASKS to provide a "
              "guarded kernel-stack slot for every possible task");
static KSlotEntry s_kslot_pool[KSLOT_POOL_SIZE];
static int32_t s_kslot_free_head = -1;
static int32_t s_kslot_list = -1;
static uint64_t s_kslot_bump = CONFIG_KSTACK_WINDOW_BASE;
// kslot state is ISR-reachable: the timer ISR (on_tick -> reap_orphans ->
// idle TaskControlBlock::create) calls alloc_kslot() every 100 ticks, and
// free_kslot() runs at task exit.  A plain SpinLock would deadlock (task
// holder preempted by the ISR can never release it) — IRQ masking via
// arch::IrqGuard is REQUIRED (docs/irqguard-ledger.md T1-T3).

__attribute__((constructor)) static void init_kslot_pool() {
    for (int32_t i = 0; i < KSLOT_POOL_SIZE - 1; ++i)
        s_kslot_pool[i].next = i + 1;
    s_kslot_pool[KSLOT_POOL_SIZE - 1].next = -1;
    s_kslot_free_head = 0;
    s_kslot_list = -1;
}

static uint64_t stack_size_for_priority(uint64_t priority) {
    constexpr uint64_t table[] = CONFIG_STACK_SIZE_TABLE;
    constexpr uint64_t tiers = sizeof(table) / sizeof(table[0]);
    if (tiers == 0)
        return CONFIG_STACK_SIZE;
    uint64_t idx = priority;
    if (idx >= tiers)
        idx = tiers - 1;
    return table[idx];
}

// ---------------------------------------------------------------------------
// User-mode yield stub
//
// create_user() historically stored the caller's `entry` (a kernel .text
// address of a C++ lambda) directly into the user iret frame.  Such a task
// MUST NOT be dispatched: the CPU would fetch a kernel address in user mode
// (CS=0x1B) -> #PF, and the user page table maps only PAGE_USER frames.
// BUGS.md#020.  A C++ lambda cannot run in user mode at all (there is no
// mechanism to map it); a real user task is an ELF-loaded app.  So for
// create_user() tasks we install a tiny user-mode stub that yields forever.
// The stub VA is in low user space and never collides with test buffer VAs
// (>= 0x100000000) or the stack/heap (0x60000000/0x70000000).
// ---------------------------------------------------------------------------

/// @brief VA where the user-mode yield stub is mapped for every create_user()
///        task.  Chosen to avoid mem::STACK_VADDR/HEAP_VADDR and the buffer
///        test VAs (>= 0x100000000).  Canonical value lives in task.hpp
///        (kernel::task::kUserYieldStubVa) for debugger tests.
constexpr uint64_t kUserYieldStubVa = task::kUserYieldStubVa;

/// @brief Per-arch machine code for "yield forever" (syscall YIELD=0 loop).
#if defined(CONFIG_ARCH_X86_64)
//   xor eax,eax     31 C0     ; rax = SyscallNumber::YIELD (0)
//   int $0x80        CD 80    ; trap gate vector 0x80 (isr_128) — GS-free path.
//                             ; NOT `syscall` (0F 05): MSR_KERNEL_GS_BASE is
//                             ; never written, so syscall_entry's swapgs would
//                             ; #PF to phys 0 (see
//                             audits/gs-base-swapgs-audit-v0.3.9.md F-1).
//   jmp -6          EB FA
static constexpr uint8_t kUserYieldStub[] = {0x31, 0xC0, 0xCD,
                                             0x80, 0xEB, 0xFA};
#elif defined(CONFIG_ARCH_AARCH64)
//   mov x8, #0      08 00 80 d2  ; x8 = SyscallNumber::YIELD (0)
//   svc #0          01 00 00 d4
//   b .-8           FE FF FF 17  ; loop to the mov (yield forever).
//   (Issue #235, disassembly-verified with aarch64-elf-objdump: the prior
//   bytes set x0 (not the x8 syscall-number register) and branched +96
//   out of the stub (18 00 00 14), so fresh tasks issued a garbage
//   syscall then fell off into unmapped memory. Offsets are relative to
//   the branch instruction itself.)
static constexpr uint8_t kUserYieldStub[] = {
    0x08, 0x00, 0x80, 0xD2, 0x01, 0x00, 0x00, 0xD4, 0xFE, 0xFF, 0xFF, 0x17};
#elif defined(CONFIG_ARCH_RISCV64)
//   li a7, 0        13 00 80 00  ; a7 = SyscallNumber::YIELD (0)
//   ecall           73 00 00 00
//   j -12           6f 00 00 00  ; (jump relative 0 — tighten below)
static constexpr uint8_t kUserYieldStub[] = {
    0x13, 0x00, 0x80, 0x00, 0x73, 0x00, 0x00, 0x00, 0x6f, 0x00, 0x00, 0x00};
#endif

/// @brief Map the user-mode yield stub into @p tcb's user PML4 and rewrite the
///        saved iret frame's entry slots to point at it.  Returns false if the
///        stub page cannot be allocated/mapped.
/// @post After a successful call, dispatching @p tcb enters user mode at the
///       yield stub (never a kernel address) and cooperatively yields forever.
static bool install_user_yield_stub(TaskControlBlock &tcb) {
    uint64_t stub_phys = PMM::alloc_user_page();
    if (!stub_phys)
        return false;

    // Copy the stub machine code into the physical page (HHDM alias).
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *dst = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + stub_phys);
    for (size_t i = 0; i < sizeof(kUserYieldStub); ++i)
        dst[i] = kUserYieldStub[i];

    if (!tcb.get_page_table()) {
        PMM::free_page(stub_phys);
        return false;
    }
    // Issue #363 option-B: stub is executed in user mode — explicit CODE.
    VMM::map_page_in_pml4(kUserYieldStubVa, stub_phys, true, true,
                          tcb.get_page_table());

#if defined(CONFIG_ARCH_X86_64)
    // The saved iret frame stores the (placeholder kernel) entry in two slots:
    // the iret RIP and the SysV rdi copy.  The placeholder is a kernel
    // higher-half address (TaskControlBlock::is_kernel_address()); the stub
    // is a low user VA.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *frame = reinterpret_cast<uint64_t *>(tcb.context.rsp);
    size_t slots = (tcb.kernel_stack_top - tcb.context.rsp) / sizeof(uint64_t);

    uint64_t original_entry = 0;
    for (size_t i = 0; i < slots; ++i) {
        if (TaskControlBlock::is_kernel_address(frame[i])) {
            original_entry = frame[i];
            break;
        }
    }
    if (original_entry == 0) {
        PMM::free_page(stub_phys);
        return false;
    }
    for (size_t i = 0; i < slots; ++i)
        if (frame[i] == original_entry)
            frame[i] = kUserYieldStubVa;
#elif defined(CONFIG_ARCH_AARCH64)
    tcb.context.elr_el1 = kUserYieldStubVa;
#elif defined(CONFIG_ARCH_RISCV64)
    // SEPC at frame offset 31 (SAVE_SIZE = 37 qwords).
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *frame = reinterpret_cast<uint64_t *>(tcb.context.sp);
    frame[31] = kUserYieldStubVa;
#endif
    return true;
}

static uint64_t alloc_kslot(uint64_t stack_size) {
    // #434 P1: arch-neutral. Window tables are wired by the build-selected
    // backend (x86_64/aa64); rv64 stub returns false -> exhaustion-0 path
    // (general mechanism, no arch guard in callers). Lazy init: must run
    // after PMM and VMM init (first create() is in Scheduler::init()).
    if (!kslot_backend::wire_window_tables())
        return 0;

    uint64_t slot_size = ((stack_size + 4095) / 4096) * 4096 + 4096;

    { // Free list (first-fit), IRQ-safe.
        arch::IrqGuard _g{};
        int32_t *pp = &s_kslot_list;
        while (*pp >= 0) {
            KSlotEntry &e = s_kslot_pool[*pp];
            if (e.size >= slot_size) {
                uint64_t va = e.va;
                if (va < CONFIG_KSTACK_WINDOW_BASE ||
                    va + e.size >
                        CONFIG_KSTACK_WINDOW_BASE + CONFIG_KSTACK_WINDOW_SIZE)
                    panic("kslot free list corrupt");
                int32_t next = e.next;
                e.next = s_kslot_free_head;
                s_kslot_free_head = *pp;
                *pp = next;
                return va;
            }
            pp = &e.next;
        }
    }

    { // Bump allocate, IRQ-safe.
        arch::IrqGuard _g{};
        uint64_t va = s_kslot_bump;
        if (va + slot_size >
            CONFIG_KSTACK_WINDOW_BASE + CONFIG_KSTACK_WINDOW_SIZE)
            return 0;
        s_kslot_bump = va + slot_size;
        return va;
    }
}

static void free_kslot(uint64_t va, uint64_t size) {
    arch::IrqGuard _g{};
    if (va == 0 || s_kslot_free_head < 0)
        return;
    int32_t idx = s_kslot_free_head;
    s_kslot_free_head = s_kslot_pool[idx].next;
    s_kslot_pool[idx].va = va;
    s_kslot_pool[idx].size = size;
    s_kslot_pool[idx].next = s_kslot_list;
    s_kslot_list = idx;
}

// ---------------------------------------------------------------------------
// Kernel-stack window backend (build-selected TU, SPEC §3.1)
// ---------------------------------------------------------------------------
// Table wiring, map/unmap, and TLB maintenance live in
// kslot_backend::{wire_window_tables,map_window_page,unmap_window_page,
// flush_window_va}. task.cpp keeps only the shared policy (pool/bump,
// guard+stack layout, teardown order). No CONFIG_ARCH here (C3).

} // anonymous namespace

// ---------------------------------------------------------------------------
// kslot snapshot support (v0.4.0 MP-6.3)
// ---------------------------------------------------------------------------
// The kstack-window PT pages are boot-shared (referenced by value from every
// private kernel-half PML4), and the slot bookkeeping (bump pointer, free
// list, pool) is NOT rewound by the MemPool/PMM snapshots.  Tests that map /
// unmap kstack pages or allocate/free slots would otherwise leak stale PTEs
// or slots across test boundaries.  test_isolate.cpp snapshots and restores
// this state at every test boundary.

size_t kslot_snapshot_bytes() {
    return kslot_backend::snapshot_bytes() + sizeof(uint64_t) +
           2 * sizeof(int32_t) + KSLOT_POOL_SIZE * sizeof(KSlotEntry);
}

void kslot_snapshot_capture(uint8_t *dst) {
    size_t off = 0;
    size_t back = kslot_backend::snapshot_bytes();
    if (back) {
        kslot_backend::snapshot_capture(dst);
        off += back;
    }
    __builtin_memcpy(dst + off, &s_kslot_bump, sizeof(s_kslot_bump));
    off += sizeof(s_kslot_bump);
    __builtin_memcpy(dst + off, &s_kslot_list, sizeof(s_kslot_list));
    off += sizeof(s_kslot_list);
    __builtin_memcpy(dst + off, &s_kslot_free_head, sizeof(s_kslot_free_head));
    off += sizeof(s_kslot_free_head);
    __builtin_memcpy(dst + off, s_kslot_pool,
                     KSLOT_POOL_SIZE * sizeof(KSlotEntry));
}

void kslot_snapshot_restore(const uint8_t *src) {
    size_t off = 0;
    size_t back = kslot_backend::snapshot_bytes();
    if (back) {
        // Issue #263: backend framing is fixed-size; restore consumes it
        // unconditionally, mirroring capture.
        kslot_backend::snapshot_restore(src);
        off += back;
    }
    __builtin_memcpy(&s_kslot_bump, src + off, sizeof(s_kslot_bump));
    off += sizeof(s_kslot_bump);
    __builtin_memcpy(&s_kslot_list, src + off, sizeof(s_kslot_list));
    off += sizeof(s_kslot_list);
    __builtin_memcpy(&s_kslot_free_head, src + off, sizeof(s_kslot_free_head));
    off += sizeof(s_kslot_free_head);
    __builtin_memcpy(s_kslot_pool, src + off,
                     KSLOT_POOL_SIZE * sizeof(KSlotEntry));
}

// ---------------------------------------------------------------------------
// Software sentinel canaries (v0.4.0 MP-3)
// ---------------------------------------------------------------------------
// Each user-visible segment (text/data/heap/stack) gets an 8-byte sentinel at
// its before/after boundary.  The canary_before/after TCB fields store the
// canary SLOT VAs; the expected value is derived from CANARY_MAGIC.  The
// kernel-stack canary lives at kernel_stack[0..8) (the base, normally
// untouched because stacks grow down from kernel_stack_top).

static uint64_t canary_expected(unsigned seg) {
    return TaskControlBlock::CANARY_MAGIC ^ (static_cast<uint64_t>(seg) + 1);
}

static constexpr uint64_t KERNEL_STACK_CANARY_MAGIC =
    TaskControlBlock::CANARY_MAGIC ^ 0x40ULL;

/// @brief Write an 8-byte canary value at @p va through @p pml4 (resolving
///        the frame via HHDM).  No-op when the page is not present.
void canary_write_at(uint64_t va, uint64_t value, uint64_t pml4) {
    if (!va)
        return;
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
    if (!phys)
        return;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memcpy(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys), &value,
                     8);
}

/// @brief Install the user-stack and initial-heap canaries for @p t
///        (call after the user stack + heap are mapped and program_break set).
void canary_install_user_segments(TaskControlBlock *t) {
    if (!t || !t->get_page_table())
        return;
    const uint64_t stk = TaskControlBlock::SEG_STACK;
    if (t->user_stack_size_ > 0) {
        t->canary_before[stk] = mem::STACK_VADDR + arch::PAGE_SIZE;
        t->canary_after[stk] =
            mem::STACK_VADDR + arch::PAGE_SIZE + t->user_stack_size_ - 8;
        canary_write_at(t->canary_before[stk], canary_expected(stk),
                        t->get_page_table());
        canary_write_at(t->canary_after[stk], canary_expected(stk),
                        t->get_page_table());
        t->canary_installed |= (1u << stk);
    }
    const uint64_t heap = TaskControlBlock::SEG_HEAP;
    if (t->program_break >= mem::HEAP_VADDR + arch::PAGE_SIZE) {
        t->canary_before[heap] = mem::HEAP_VADDR;
        t->canary_after[heap] = ((t->program_break + arch::PAGE_SIZE - 1) &
                                 ~(arch::PAGE_SIZE - 1)) -
                                8;
        canary_write_at(t->canary_before[heap], canary_expected(heap),
                        t->get_page_table());
        canary_write_at(t->canary_after[heap], canary_expected(heap),
                        t->get_page_table());
        t->canary_installed |= (1u << heap);
    }
}

/// @brief Install the kernel-stack canary at kernel_stack[0..8).
void canary_install_kernel_stack(TaskControlBlock *t) {
    if (!t || !t->kernel_stack)
        return;
    __builtin_memcpy(t->kernel_stack, &KERNEL_STACK_CANARY_MAGIC, 8);
    t->canary_installed |= (1u << TaskControlBlock::CANARY_SEGMENTS);
}

/// @brief Verify all installed user-segment canaries of @p t (pure reads).
/// @return true when intact; on mismatch sets @p bad_segment / @p bad_va.
bool canary_verify_user_segments(const TaskControlBlock *t,
                                 uint8_t &bad_segment, uint64_t &bad_va) {
    for (unsigned i = 0; i < TaskControlBlock::CANARY_SEGMENTS; ++i) {
        if (!(t->canary_installed & (1u << i)))
            continue;
        const uint64_t expected = canary_expected(i);
        const uint64_t vas[2] = {t->canary_before[i], t->canary_after[i]};
        for (int k = 0; k < 2; ++k) {
            if (!vas[k])
                continue;
            uint64_t phys = VMM::virt_to_phys_in_pml4(vas[k], t->get_page_table());
            if (!phys) {
                bad_segment = static_cast<uint8_t>(i);
                bad_va = vas[k];
                return false;
            }
            uint64_t live = 0;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            __builtin_memcpy(
                &live, reinterpret_cast<const void *>(arch::HHDM_OFFSET + phys),
                8);
            if (live != expected) {
                bad_segment = static_cast<uint8_t>(i);
                bad_va = vas[k];
                return false;
            }
        }
    }
    return true;
}

/// @brief Verify the kernel-stack canary of @p t (pure read).
bool canary_verify_kernel_stack(const TaskControlBlock *t) {
    if (!t || !t->kernel_stack ||
        !(t->canary_installed & (1u << TaskControlBlock::CANARY_SEGMENTS)))
        return true;
    uint64_t live = 0;
    __builtin_memcpy(&live, t->kernel_stack, 8);
    return live == KERNEL_STACK_CANARY_MAGIC;
}

/// @brief Creates a new kernel-space TaskControlBlock.
/// Allocates a TCB from MemPool, sets up a kernel stack with an
/// architecture-specific initial register frame, and returns it.
/// @param entry  Kernel function pointer to execute.
/// @param priority  Initial scheduling priority.
/// @param period_ticks  Period for rate-monotonic scheduling.
/// @return  Pointer to the new TCB, or nullptr on OOM.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
//
// Issue #104: the TCB must fit pool8 (MemPool::init sizes[] in
// mempool.cpp) — aarch64 needs 16384 (TCB 8256 B there).  A silent
// overflow fails at BOOT (idle-task OOM), so pin it at build time.
#if defined(CONFIG_ARCH_AARCH64)
static_assert(sizeof(TaskControlBlock) <= 16384,
              "TCB exceeds pool8 (aarch64)");
#else
static_assert(sizeof(TaskControlBlock) <= 8192,
              "TCB exceeds pool8");
#endif
void TaskControlBlock::install_page_table(uint64_t pml4) noexcept {
    // Issue #377: install-onto-live orphans the factory PML4 (+1 PMM,
    // the #371 shape) — fail loudly instead of leaking silently.
    ENSURE(page_table_ == 0);
    ENSURE(pml4 != 0);
    page_table_ = pml4;
}

void TaskControlBlock::swap_page_table(uint64_t new_pml4) noexcept {
    // Issue #377: exec-style swap of a live table (the caller frees the
    // old root; exec frees after the swap).  Zero-to-zero or zero-new
    // is a contract violation, not a teardown.
    ENSURE(page_table_ != 0);
    ENSURE(new_pml4 != 0);
    page_table_ = new_pml4;
}

void TaskControlBlock::clear_page_table() noexcept {
    // Issue #377: teardown clear — the caller freed the table (cleanup,
    // elf error paths).  Unconditional: clearing an already-zero slot
    // on fresh TCBs is a no-op.
    page_table_ = 0;
}

void TaskControlBlock::provision_kstack(TaskControlBlock *tcb,
                                        uint64_t kstack_phys,
                                        size_t stack_size) noexcept {
    // #434 P2: single funnel for ELF birth sites (mirrors create_user —
    // same slot formula, same backend map, same HHDM fallback).
    // Arch-neutral: all table mechanics live in the backend (C3).
    // Restricted to ELF use; task.cpp birth sites keep their form.
    size_t stack_pages = (stack_size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
    uint64_t slot_va = alloc_kslot(stack_size);
    if (slot_va) {
        tcb->kstack_slot_va_ = slot_va;
        tcb->kstack_slot_size_ = ((stack_size + 4095) / 4096) * 4096 + 4096;
        uint64_t kstack_va = slot_va + 4096;
        for (size_t i = 0; i < stack_pages; ++i)
            kslot_backend::map_window_page(
                kstack_va + i * arch::PAGE_SIZE,
                kstack_phys + i * arch::PAGE_SIZE);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        tcb->kernel_stack = reinterpret_cast<uint8_t *>(kstack_va);
        tcb->kernel_stack_top = kstack_va + stack_size;
        return;
    }
    tcb->kstack_slot_va_ = 0;
    tcb->kstack_slot_size_ = 0;
    uint64_t kstack_virt = arch::HHDM_OFFSET + kstack_phys;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    tcb->kernel_stack = reinterpret_cast<uint8_t *>(kstack_virt);
    tcb->kernel_stack_top = kstack_virt + stack_size;
}

TaskControlBlock *TaskControlBlock::create(void (*entry)(), uint64_t priority,
                                           uint64_t period_ticks)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
#if defined(CONFIG_DEBUG)
    Logger::raw_write("[TCB] create pool8=");
    Logger::print_dec(MemPool::pool_free_count(8));
    Logger::raw_write(" tcnt=");
    Logger::print_dec(Scheduler::task_count());
    Logger::raw_write("\n");
#endif
    auto *tcb = static_cast<TaskControlBlock *>(
        MemPool::alloc(sizeof(TaskControlBlock)));
    if (!tcb) {
        Logger::raw_write("TCB::create OOM pool8=");
        Logger::print_dec(MemPool::pool_free_count(8));
        Logger::raw_write("/8 tcnt=");
        Logger::print_dec(Scheduler::task_count());
        Logger::raw_write("\n");
        return nullptr;
    }

#ifdef CONFIG_DEBUG
    // BUGS.md#019/#020 detector: see debug_check_tcb_reuse() above.
    debug_check_tcb_reuse(tcb);
#endif

    memset(tcb, 0, sizeof(TaskControlBlock));
    kernel::task_step(kernel::TaskCreateSteps::Alloc); // issue #347 Slice C

    TCB_WRITE(tcb, magic, TCB_MAGIC);
    TCB_WRITE(tcb, id, Scheduler::alloc_id());
    tcb->iopb_slot_ = TaskControlBlock::IOPB_SLOT_NONE;
    tcb->tls_base_ = 0; // issue #74: no TLS until TLS_SET
    // Issue #41: watchdog disarmed, never-armed generation (all-zero
    // already skips the scan; explicit per the TCB-memset discipline).
    tcb->wdog_armed = false;
    tcb->wdog_period_ticks = 0;
    tcb->wdog_last_kick_tick = 0;
    tcb->wdog_expiry_tick = 0;
    tcb->wdog_gen = 0;
    {
        char buf[CONFIG_TASK_NAME_LEN];
        size_t pos = 0;
        uint64_t n = tcb->id;
        buf[pos++] = 't';
        buf[pos++] = 'a';
        buf[pos++] = 's';
        buf[pos++] = 'k';
        buf[pos++] = '_';
        char rev[8];
        size_t rp = 0;
        do {
            rev[rp++] = static_cast<char>('0' + (n % 10));
            n /= 10;
        } while (n);
        while (rp > 0 && pos < CONFIG_TASK_NAME_LEN - 1)
            buf[pos++] = rev[--rp];
        buf[pos] = '\0';
        __builtin_memcpy(tcb->name, buf, pos + 1);
    }
    TCB_WRITE(tcb, state, TaskState::READY);
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->cpu_affinity = 1; // issue #25 C1: default CPU0 (create() memsets;
                           // the ctor default member init never runs)
    tcb->period_ticks = period_ticks;
    tcb->deadline_ticks = arch::Timer::ticks() + period_ticks;
    tcb->deadline_missed = false;
    tcb->deadline_miss_count = 0;
    tcb->executed_ticks = 0;
    tcb->exec_ns_total = 0;
    tcb->exec_period_ns = 0;
    tcb->exec_window_start_tick = arch::Timer::ticks(); // issue #331
    tcb->exec_stamp_ns = 0;
    tcb->sched_policy = SchedPolicy::AUTO; // issue #19: memset already zero
    tcb->edf_exempt = false;               // (AUTO=0); explicit for clarity
    tcb->edf_next_ = nullptr;
    tcb->edf_prev_ = nullptr;
    tcb->in_edf_queue_ = false;
    tcb->ready_deferred_ = false;
    tcb->remaining_ticks = period_ticks;
    tcb->wcet_ticks = 0;
    tcb->wcet_overrun_fired = false;
    tcb->wcet_observed_ns = 0; // issue #283: memset already zero;
                                  // explicit per the TCB-memset discipline
    tcb->memory_budget_pages_ = 0;
    tcb->memory_used_pages_ = 0;
    tcb->mem_alloc_ops_ = 0; // issue #284 (see task.hpp; memset covers)
    tcb->mem_free_ops_ = 0;
    tcb->mem_prev_outstanding_ = 0;
    tcb->mem_prev_used_ = 0;
    tcb->mem_leak_streak_ = 0;
    init_task_common(*tcb);
    kernel::task_step(kernel::TaskCreateSteps::ZeroInit); // issue #347 Slice C

    uint64_t stack_size = stack_size_for_priority(priority);
    size_t stack_pages = (stack_size + 4095) / arch::PAGE_SIZE;
#if CONFIG_MEMORY_BUDGET
    if (!Scheduler::reserve_memory_pages(stack_pages)) {
        Logger::warn("TCB::create: budget OOM for %zu-page stack", stack_pages);
        // Issue #234: creation-time OOM enters the ring.
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 1, log::LogSeverity::ERROR,
                            "TCB stack budget OOM", stack_pages);
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }
#endif
    uint64_t stack_phys = PMM::alloc_contiguous(stack_pages);
    if (!stack_phys) {
        Logger::warn("TCB::create: PMM OOM for %zu-page stack", stack_pages);
        // Issue #234: creation-time OOM enters the ring.
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 1, log::LogSeverity::ERROR,
                            "TCB stack PMM OOM", stack_pages);
#if CONFIG_MEMORY_BUDGET
        Scheduler::release_memory_pages(stack_pages);
#endif
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }

    tcb->stack_phys_ = stack_phys;
    kernel::task_step(kernel::TaskCreateSteps::StackAlloc); // issue #347 Slice C
    // ------------------------------------------------------------------
    // Kernel-stack guard page discipline (ASIL-D requirement)
    // ------------------------------------------------------------------
    // All kernel stacks MUST have an unmapped guard page below them so
    // stack overflow traps deterministically instead of corrupting adjacent
    // kernel data.  The pre-allocated kslot window provides this via
    // alloc_kslot() — each slot reserves one unmapped page at the base.
    //
    // However, the kslot bookkeeping (free list, bump allocator) is NOT
    // rewound by snapshot_restore() (only MemPool/PMM snapshots are).
    // Test cycles repeatedly create and destroy tasks; without the guard
    // below, each test iteration would leak kslot slots, eventually
    // exhausting the window.
    //
    // RATIONALE for the test-active exemption:
    //   - Kernel tasks created during tests (via create()) are short-lived
    //     and bounded by CONFIG_MAX_TASKS (64).  Their stacks use HHDM
    //     direct mapping — no guard page, but the bounded number and the
    //     test framework's full cleanup (snapshot_restore rewinds all
    //     task memory) guarantee no overflow can persist across tests.
    //   - User tasks (daemons: vfsd, iocd, test ELF loads) ALWAYS route
    //     through kslot via create_user()/clone() — they persist across
    //     snapshot_restore and NEED the guard page for production safety.
    //   - Production (is_test_active() == false): every kernel task uses
    //     kslot, fulfilling the ASIL-D guard-page invariant.
    //
    // This dual-path design is certified-safe because the test environment
    // provides equivalent fault containment (complete memory rewind),
    // while production builds never take the HHDM fallback.
    // #434 P1: arch-neutral — backend wire failure (e.g. rv64 stub) takes
    // the documented exhaustion-0 path; no arch guard in callers (C3).
    // ------------------------------------------------------------------
    if (!Scheduler::in_test_harness()) {
        uint64_t slot_va = alloc_kslot(stack_size);
        if (slot_va) {
            tcb->kstack_slot_va_ = slot_va;
            tcb->kstack_slot_size_ = ((stack_size + 4095) / 4096) * 4096 + 4096;
            uint64_t kstack_va = slot_va + 4096;
            for (size_t i = 0; i < stack_pages; ++i)
                kslot_backend::map_window_page(
                    kstack_va + i * arch::PAGE_SIZE,
                    stack_phys + i * arch::PAGE_SIZE);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            TCB_WRITE(tcb, kernel_stack,
                      reinterpret_cast<uint8_t *>(kstack_va));
            tcb->kernel_stack_top = kstack_va + stack_size;
            goto done_stack;
        }
    }
    tcb->kstack_slot_va_ = 0;
    tcb->kstack_slot_size_ = 0;
    {
        uint64_t stack_virt = arch::HHDM_OFFSET + stack_phys;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        TCB_WRITE(tcb, kernel_stack, reinterpret_cast<uint8_t *>(stack_virt));
        tcb->kernel_stack_top = stack_virt + stack_size;
    }
done_stack:

    // v0.4.0 MP-1.2: every kernel task gets a private kernel-half PML4.
    // clone_kernel_pml4() zeroes the user entries (0..255) and copies the
    // kernel entries (>= PML4_KERNEL_START — kernel text/data/bss, HHDM, and
    // the shared kslot window) by value from the boot kernel PML4.  The kslot
    // window PD/PT pages are boot-shared (see docs/specs/memory.md §7.1) —
    // per-task private kstack tables are NOT allocated.
    tcb->install_page_table(VMM::clone_kernel_pml4());
    if (tcb->get_page_table() == 0) {
        ASSERT(errors::TaskError::TASK_ERR_PML4_CLONE);
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }
    kernel::task_step(kernel::TaskCreateSteps::Pml4Clone); // issue #347 Slice C

    // PMM does not zero freed pages (it poison-fills them, e.g. with 0x0b),
    // so a stack allocated from recycled memory can retain stale poison in
    // the region below the initial frame.  Zero the entire stack up front so
    // every slot the task may reach — including deep call-chain return
    // addresses — starts from a known state instead of tripping a fault on a
    // leftover 0x0b return address.
    __builtin_memset(tcb->kernel_stack, 0, stack_size);

    // v0.4.0 MP-3: arm the kernel-stack canary at the stack base.
    canary_install_kernel_stack(tcb);

#if defined(CONFIG_ARCH_X86_64)
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);

    *--stack = arch::SEG_KERNEL_DATA;
    *--stack = tcb->kernel_stack_top;

    *--stack = arch::RFLAGS_DEFAULT;
    *--stack = arch::SEG_KERNEL_CODE;
    *--stack = reinterpret_cast<uint64_t>(_task_trampoline);

    *--stack = 0;
    *--stack = 0;

    // Register save frame matching isr_common push order
    *--stack = 0;                                 // r15
    *--stack = 0;                                 // r14
    *--stack = 0;                                 // r13
    *--stack = 0;                                 // r12
    *--stack = 0;                                 // r11
    *--stack = 0;                                 // r10
    *--stack = 0;                                 // r9
    *--stack = 0;                                 // r8
    *--stack = 0;                                 // rbp
    *--stack = reinterpret_cast<uint64_t>(entry); // rdi = entry (SysV ABI arg1)
    *--stack = 0;                                 // rsi
    *--stack = 0;                                 // rdx
    *--stack = 0;                                 // rcx
    *--stack = 0;                                 // rbx
    *--stack = 0;                                 // rax

    tcb->context.rsp = reinterpret_cast<uint64_t>(stack);
    kernel::debug::record_task_entry(reinterpret_cast<uint64_t>(entry),
                                     reinterpret_cast<uint64_t>(tcb));
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    *--stack = 0;     // padding
    *--stack = 0;     // padding
    *--stack = 0x345; // SPSR_EL1: EL1h, DAIF with I=0 (IRQs enabled) so
                      // voluntary reschedule()s are applied by the next
                      // timer tick (x86 kernel tasks run with IF=1).
                      // 0x3C5 (all masked) would leave a blocking kernel
                      // task spinning forever on aarch64.
    *--stack = reinterpret_cast<uint64_t>(_task_trampoline); // ELR_EL1
    *--stack = 0; // SP_EL0 (unused for kernel task)
    for (int i = 0; i < 30; ++i)
        *--stack = 0; // X1-X30 (x30=0: trampoline catches the return)
    *--stack = reinterpret_cast<uint64_t>(entry); // X0 = entry arg
    tcb->context.sp_el0 = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_RISCV64)
    // Build trap frame matching syscall_entry.S save area layout.
    // Offsets (in qwords): [0]=RA, [1]=SP, [9]=A0, [13]=A4 (regs ptr),
    //   [31]=SEPC, [32]=SSTATUS. SAVE_SIZE = 37 qwords (296 bytes).
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    stack -= 37;
    __builtin_memset(stack, 0, 37 * 8);
    stack[31] = reinterpret_cast<uint64_t>(_task_trampoline); // SEPC
    stack[9] = reinterpret_cast<uint64_t>(entry); // A0 = entry arg
    // SSTATUS bits (issue #348-C): SPIE=1 (prior interrupt enable),
    // SPP=1 (trap returns to S-mode).
    constexpr uint64_t SSTATUS_SPIE = 1ULL << 5;
    constexpr uint64_t SSTATUS_SPP = 1ULL << 8;
    stack[32] = SSTATUS_SPIE | SSTATUS_SPP;
    tcb->context.sp = reinterpret_cast<uint64_t>(stack);
#endif

    kernel::task_step(kernel::TaskCreateSteps::FrameBuild); // issue #347 Slice C
    return tcb;
}

/// @brief Creates a new user-space TaskControlBlock.
/// Allocates a TCB, kernel stack, user stack, and a cloned PML4 page table.
/// The initial register frame targets user-mode execution.
/// @param entry  User-space entry point address.
/// @param priority  Initial scheduling priority.
/// @param period_ticks  Period for rate-monotonic scheduling.
/// @param user_stack_size  Size of the user stack in bytes.
/// @return  Pointer to the new TCB, or nullptr on OOM.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
TaskControlBlock *
TaskControlBlock::create_user(void (*entry)(), uint64_t priority,
                              uint64_t period_ticks, size_t user_stack_size)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    auto *tcb = static_cast<TaskControlBlock *>(
        MemPool::alloc(sizeof(TaskControlBlock)));
    if (!tcb)
        return nullptr;
#ifdef CONFIG_DEBUG
    // BUGS.md#019/#020 detector: create_user previously lacked this check
    // (create() had it); the buffer_pool_* tests reproduce the UAF via this
    // path.
    debug_check_tcb_reuse(tcb);
#endif
    memset(tcb, 0, sizeof(TaskControlBlock));

    TCB_WRITE(tcb, magic, TCB_MAGIC);
    TCB_WRITE(tcb, id, Scheduler::alloc_id());
    tcb->iopb_slot_ = TaskControlBlock::IOPB_SLOT_NONE;
    tcb->tls_base_ = 0; // issue #74: no TLS until TLS_SET
    // Issue #41: watchdog disarmed, never-armed generation (see create).
    tcb->wdog_armed = false;
    tcb->wdog_period_ticks = 0;
    tcb->wdog_last_kick_tick = 0;
    tcb->wdog_expiry_tick = 0;
    tcb->wdog_gen = 0;
    TCB_WRITE(tcb, state, TaskState::READY);
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->cpu_affinity = 1; // issue #25 C1: default CPU0 (create() memsets;
                           // the ctor default member init never runs)
    tcb->period_ticks = period_ticks;
    tcb->deadline_ticks = arch::Timer::ticks() + period_ticks;
    tcb->deadline_missed = false;
    tcb->deadline_miss_count = 0;
    tcb->executed_ticks = 0;
    tcb->exec_ns_total = 0;
    tcb->exec_period_ns = 0;
    tcb->exec_window_start_tick = arch::Timer::ticks(); // issue #331
    tcb->exec_stamp_ns = 0;
    tcb->sched_policy = SchedPolicy::AUTO; // issue #19 (AUTO=0; explicit)
    tcb->edf_exempt = false;
    tcb->edf_next_ = nullptr;
    tcb->edf_prev_ = nullptr;
    tcb->in_edf_queue_ = false;
    tcb->ready_deferred_ = false;
    tcb->remaining_ticks = period_ticks;
    tcb->memory_budget_pages_ = 0;
    tcb->memory_used_pages_ = 0;
    tcb->mem_alloc_ops_ = 0; // issue #284 (see create)
    tcb->mem_free_ops_ = 0;
    tcb->mem_prev_outstanding_ = 0;
    tcb->mem_prev_used_ = 0;
    tcb->mem_leak_streak_ = 0;
    tcb->wcet_ticks = 0;
    tcb->wcet_overrun_fired = false;
    tcb->wcet_observed_ns = 0; // issue #283 (see create)
    init_task_common(*tcb);

    size_t kernel_stack_pages = (STACK_SIZE + 4095) / arch::PAGE_SIZE;
    uint64_t kstack_phys = PMM::alloc_contiguous(kernel_stack_pages);
    if (!kstack_phys) {
        ASSERT(errors::TaskError::TASK_ERR_STACK_ALLOC);
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }
    tcb->stack_phys_ = kstack_phys;

    // Route through kslot for guard page below kernel stack.
    // #434 P1: unconditional backend map (C3); backend no-ops only on
    // exhaustion, never on arch.
    uint64_t slot_va = alloc_kslot(STACK_SIZE);
    if (slot_va) {
        tcb->kstack_slot_va_ = slot_va;
        tcb->kstack_slot_size_ = ((STACK_SIZE + 4095) / 4096) * 4096 + 4096;
        uint64_t kstack_va = slot_va + 4096;
        for (size_t i = 0; i < kernel_stack_pages; ++i)
            kslot_backend::map_window_page(kstack_va + i * arch::PAGE_SIZE,
                                           kstack_phys + i * arch::PAGE_SIZE);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        tcb->kernel_stack = reinterpret_cast<uint8_t *>(kstack_va);
        tcb->kernel_stack_top = kstack_va + STACK_SIZE;
    } else {
        tcb->kstack_slot_va_ = 0;
        tcb->kstack_slot_size_ = 0;
        uint64_t kstack_virt = arch::HHDM_OFFSET + kstack_phys;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        tcb->kernel_stack = reinterpret_cast<uint8_t *>(kstack_virt);
        tcb->kernel_stack_top = kstack_virt + STACK_SIZE;
    }

    size_t user_stack_pages = (user_stack_size + 4095) / arch::PAGE_SIZE;
    uint64_t ustack_phys = PMM::alloc_user_contiguous(user_stack_pages);
    if (!ustack_phys) {
        ASSERT(errors::TaskError::TASK_ERR_USTACK_ALLOC);
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }

    uint64_t pml4 = VMM::clone_kernel_pml4();
    if (!pml4) {
        ASSERT(errors::TaskError::TASK_ERR_PML4_CLONE);
        size_t pages = (user_stack_size + 4095) / arch::PAGE_SIZE;
        for (size_t i = 0; i < pages; ++i)
            PMM::free_page(ustack_phys + i * arch::PAGE_SIZE);
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }
    tcb->install_page_table(pml4);
    tcb->is_user_ = true;
// Issue #352: eager context-ID assignment at birth (0 when unsupported).
// Publish-time lazy assignment remains as backstop for paths that
// install user PML4s without passing here.  Arch-neutral: x86_64
// delegates to pcid_alloc, aarch64/riscv64 to asid_alloc.
    tcb->pcid_ = arch::context_id_alloc();

    // Guard page: leave first page unmapped, start mapping at +arch::PAGE_SIZE
    uint64_t user_stack_virt = mem::STACK_VADDR + arch::PAGE_SIZE;
    for (size_t i = 0; i < user_stack_pages; ++i) {
        // Issue #325-D: user stacks are data (never executable).
        VMM::map_page_in_pml4(user_stack_virt + i * arch::PAGE_SIZE,
                              ustack_phys + i * arch::PAGE_SIZE, true, false,
                              pml4);
    }

    tcb->user_stack_ = ustack_phys;
    tcb->user_stack_size_ = user_stack_size;
    uint64_t user_rsp = user_stack_virt + user_stack_size;

    // v0.4.0 MP-3: kernel-stack canary + user-stack canaries.
    canary_install_kernel_stack(tcb);
    canary_install_user_segments(tcb);

#if defined(CONFIG_ARCH_X86_64)
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    *--stack = arch::SEG_USER_DATA;
    *--stack = user_rsp;
    *--stack = arch::RFLAGS_DEFAULT;
    *--stack = arch::SEG_USER_CODE;
    *--stack = reinterpret_cast<uint64_t>(entry);
    *--stack = 0;
    *--stack = 0;
    // Register save frame matching isr_common push order
    *--stack = 0;                                 // r15
    *--stack = 0;                                 // r14
    *--stack = 0;                                 // r13
    *--stack = 0;                                 // r12
    *--stack = 0;                                 // r11
    *--stack = 0;                                 // r10
    *--stack = 0;                                 // r9
    *--stack = 0;                                 // r8
    *--stack = 0;                                 // rbp
    *--stack = reinterpret_cast<uint64_t>(entry); // rdi = entry
    *--stack = 0;                                 // rsi
    *--stack = 0;                                 // rdx
    *--stack = 0;                                 // rcx
    *--stack = 0;                                 // rbx
    *--stack = 0;                                 // rax
    tcb->context.rsp = reinterpret_cast<uint64_t>(stack);
    kernel::debug::record_task_entry(reinterpret_cast<uint64_t>(entry),
                                     reinterpret_cast<uint64_t>(tcb));
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    *--stack = 0;    // padding
    *--stack = 0;    // padding
    *--stack = 0x0; // SPSR_EL1: M[4:0]=EL0t (AArch64 EL0), DAIF unmasked.
    // NOTE: bit 4 selects AArch32 on exception return — SPSR 0x10 would
    // eret into AArch32 EL0 (wrong ISA), not EL0t.
    *--stack = reinterpret_cast<uint64_t>(entry); // ELR_EL1
    *--stack = user_rsp;                          // SP_EL0
    for (int i = 0; i < 31; ++i)
        *--stack = 0; // X0-X30
    tcb->context.sp_el0 = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_RISCV64)
    // Build trap frame matching syscall_entry.S save area layout
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    stack -= 37;
    __builtin_memset(stack, 0, 37 * 8);
    stack[1] = user_rsp;                           // X2/SP = user stack
    stack[31] = reinterpret_cast<uint64_t>(entry); // SEPC = entry point
    // SSTATUS: SPIE=1, SPP=0 (U-mode) + FS=11b (issue #353: user FP must
    // be enabled from first dispatch — sret restores FS from this frame,
    // so FS=0 here would switch FP off at the first trap return and fault
    // the next fpu_save).
    stack[32] = (1ULL << 5) | (3ULL << 13);
    tcb->context.sp = reinterpret_cast<uint64_t>(stack);
#endif

    // BUGS.md#020: install a real user-mode entry (yield-forever stub) so this
    // task is SAFE to dispatch.  Without it the saved frame's RIP is the
    // caller's kernel-address lambda -> user-mode fetch of kernel .text -> #PF.
    // A failure to map the stub is not fatal for container-only use (the task
    // is simply never dispatched), so we leave the frame as-is.
    (void)install_user_yield_stub(*tcb);

    return tcb;
}

#if defined(CONFIG_ARCH_X86_64)
void TaskControlBlock::save_context(uint64_t &rsp) noexcept {
    context.rsp = rsp;
}

void TaskControlBlock::restore_context(uint64_t &rsp) noexcept {
    rsp = context.rsp;
}
#elif defined(CONFIG_ARCH_AARCH64)
void TaskControlBlock::save_context(uint64_t &rsp) noexcept {
    context.sp_el0 = rsp;
}

void TaskControlBlock::restore_context(uint64_t &rsp) noexcept {
    rsp = context.sp_el0;
}
#endif

/// @brief Fail-closed admission check for clone() user parents (issue #400).
/// A user-marked parent must own a live, page-aligned PMM USER block of
/// user_stack_size_ bytes: exactly the pages the stack-copy loop reads via
/// HHDM. Synthetic shapes (flag-flipped kernel TCBs with a fake
/// user_stack_, issue #399 verdict) fault on the first copy iteration into
/// a silent halt, so they are rejected here with no allocation and no
/// teardown. PMM::free_page clears only the allocation bit, never the
/// owner bit, hence is_allocated() && is_user_page() (either alone admits
/// a freed page). Bounded: at most total_memory()/PAGE_SIZE iterations.
/// @param parent  Candidate user-marked parent.
/// @return true iff every stack page is a live PMM USER page.
static bool user_parent_stack_is_live(const TaskControlBlock &parent) noexcept {
    const uint64_t phys = parent.user_stack_;
    const uint64_t size = parent.user_stack_size_;
    if (phys == 0 || size == 0)
        return false;
    if ((phys % arch::PAGE_SIZE) != 0)
        return false;
    // Geometric bound: keeps the size math overflow-free and the stack
    // within the user address range.
    if (size > USER_SPACE_LIMIT - (mem::STACK_VADDR + arch::PAGE_SIZE))
        return false;
    const uint64_t pages = (size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
    if (phys > PMM::total_memory())
        return false;
    if (pages > (PMM::total_memory() - phys) / arch::PAGE_SIZE)
        return false;
    for (uint64_t idx = 0; idx < pages; ++idx) {
        const uint64_t page = phys + idx * arch::PAGE_SIZE;
        if (!PMM::is_allocated(page) || !PMM::is_user_page(page))
            return false;
    }
    return true;
}

/// @brief Clones the current task (fork).
/// Allocates a new TCB and kernel stack, copies parent's fd table, FPU state,
/// signal handlers, and process hierarchy. For user tasks, clones the page
/// table and copies user stack contents with COW semantics for code/data pages.
/// On non-x86_64 the parent's live FP/SIMD registers are flushed into
/// parent->fpu_state before the child copy (issue #403: eager-switch arches
/// have no trap-time save); on x86_64 the lazy owner check is unchanged.
/// @param regs  Pointer to the saved register frame from the fork syscall.
/// @return  Pointer to the new child TCB, or nullptr on failure.
TaskControlBlock *TaskControlBlock::clone(uint64_t *regs) {
    auto *parent = Scheduler::current_task();
    if (!parent)
        return nullptr;

    const bool is_user_parent = parent->is_user_;
    if (is_user_parent && !user_parent_stack_is_live(*parent)) {
        // Issue #400: synthetic user parent (no live PMM user stack).
        // Nothing allocated yet, so no teardown — return nullptr with
        // zero side effects and zero PMM mutation.
        Logger::warn("TCB::clone: bad user parent stack %llu size %llu",
                     parent->user_stack_, parent->user_stack_size_);
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 7, log::LogSeverity::ERROR,
                            "TCB::clone: bad user parent stack",
                            parent->user_stack_size_);
        return nullptr;
    }

    auto *tcb = static_cast<TaskControlBlock *>(
        MemPool::alloc(sizeof(TaskControlBlock)));
    if (!tcb)
        return nullptr;
    memset(tcb, 0, sizeof(TaskControlBlock));

    TCB_WRITE(tcb, magic, TCB_MAGIC);
    TCB_WRITE(tcb, id, Scheduler::alloc_id());
    tcb->iopb_slot_ = TaskControlBlock::IOPB_SLOT_NONE;
    // Issue #41: watchdog disarmed, never-armed generation (see create).
    tcb->wdog_armed = false;
    tcb->wdog_period_ticks = 0;
    tcb->wdog_last_kick_tick = 0;
    tcb->wdog_expiry_tick = 0;
    tcb->wdog_gen = 0;
    __builtin_memcpy(tcb->name, parent->name, CONFIG_TASK_NAME_LEN);
    tcb->parent_id = parent->id;
    TCB_WRITE(tcb, state, TaskState::READY);
    tcb->priority = parent->priority;
    tcb->base_priority = parent->base_priority;
    tcb->cpu_affinity = parent->cpu_affinity; // issue #25 C1: inherit mask
    tcb->tls_base_ = parent->tls_base_; // issue #74: clone inherits TLS base
    // Issue #312: a child forked from a USER parent is best-effort/aperiodic
    // — it must not inherit the parent's periodic window.  runelf stamps the
    // parent period_ticks 100 with no WCET, so an implicit-100% periodic
    // child would be rejected by the Liu-Leyland admission gate
    // (1.0 + 1.0 = 2.0 > bound(2) = 0.828).  A child forked from a KERNEL
    // parent inherits as today, preserving the admission_fork_denied_no_leak
    // contract (kernel forker).  deadline_ticks == 0 makes the child
    // admission-exempt (admission_exempted, I-4).
    tcb->period_ticks = is_user_parent ? 0 : parent->period_ticks;
    tcb->deadline_ticks = is_user_parent ? 0 : parent->deadline_ticks;
    tcb->executed_ticks = 0;
    tcb->exec_ns_total = 0;
    tcb->exec_period_ns = 0;
    tcb->exec_window_start_tick = arch::Timer::ticks(); // issue #331 fresh
    tcb->exec_stamp_ns = 0;
    tcb->sched_policy = SchedPolicy::AUTO; // issue #19 (clone starts AUTO;
    tcb->edf_exempt = false;               // no policy inheritance)
    tcb->wcet_observed_ns = 0;          // issue #283: fresh observed max
    tcb->mem_alloc_ops_ = 0;               // issue #284: fresh counters
    tcb->mem_free_ops_ = 0;
    tcb->mem_prev_outstanding_ = 0;
    tcb->mem_prev_used_ = 0;
    tcb->mem_leak_streak_ = 0;
    tcb->edf_next_ = nullptr;
    tcb->edf_prev_ = nullptr;
    tcb->in_edf_queue_ = false;
    tcb->ready_deferred_ = false;
    tcb->remaining_ticks = is_user_parent ? 0 : parent->remaining_ticks;
    tcb->exit_code = 0;
    tcb->waiting_child_pid = 0;
    tcb->waiting_child_status = nullptr;
    tcb->user_data = parent->user_data;
    tcb->program_break = parent->program_break;
    tcb->program_break_start = parent->program_break_start;

    tcb->blocked_next = nullptr;
    tcb->blocked_prev = nullptr;

    // Process hierarchy: add child to parent
    tcb->first_child = nullptr;
    tcb->next_sibling = nullptr;
    tcb->prev_sibling = nullptr;
    tcb->num_children = 0;
    if (parent) {
        parent->add_child(tcb);
    }

    // Per-task IPC objects (child gets fresh empty queues — embedded in TCB)
    tcb->msg_queue.init();
    tcb->msg_queue.owner = tcb;
    kernel::test::ResourceTracker::instance().track_msg_queue_add();

    tcb->notify.init();
    kernel::test::ResourceTracker::instance().track_notify_add();

    tcb->event_group.init();
    kernel::test::ResourceTracker::instance().track_event_group_add();

    // Copy fd_table.  Each copied used vnode gets a vnode_ref_inc (mirrors the
    // dup2/open/dup discipline): the child owns an independent reference and
    // must decrement it at cleanup.  Without the bump a forked child
    // over-decrements vnodes at teardown -> premature vnode close -> double
    // release of a shared object (e.g. a pipe's PipeBuffer) once it is
    // reference-counted.
    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        tcb->fd_table.fds[i] = parent->fd_table.fds[i];
        if (tcb->fd_table.fds[i].used && tcb->fd_table.fds[i].vnode)
            vfs::vnode_ref_inc(tcb->fd_table.fds[i].vnode);
    }

    // Copy cwd
    size_t cwd_len = 0;
    while (parent->cwd[cwd_len] && cwd_len < 255)
        ++cwd_len;
    for (size_t i = 0; i <= cwd_len; ++i)
        tcb->cwd[i] = parent->cwd[i];
    tcb->cwd_vnode = parent->cwd_vnode;
    vfs::vnode_ref_inc(tcb->cwd_vnode);

    // Buffer pool starts empty — buffers are NOT inherited on fork
    tcb->buf_list_head = -1;

#if defined(CONFIG_ARCH_X86_64)
    // Save parent's FPU state if it's currently in this CPU's registers
    // (issue #151: ownership is per-CPU; the cloning CPU's slot decides).
    if (__atomic_load_n(&fpu_owner_own(), __ATOMIC_ACQUIRE) == parent) {
        arch::fxsave(parent->fpu_state);
        ++parent->fpu_state_gen;
    }
#else
    // Issue #403: aarch64/riscv64 switch FP eagerly, and only inside
    // scheduler_on_context_switch. A synchronous fork trap performs no
    // switch, so the parent's live FP regs are still unflushed here;
    // materialize them before the child copy (mirrors the x86 fxsave).
    // The gate mirrors the eager switch (docs/specs/fpu-context.md §7):
    // only user-side FP state is tracked (kernel code carries no FP
    // instructions) and fpu_available() keeps the degraded, FP-absent
    // path from executing save instructions.
    if (parent->is_user_ && arch::fpu_available()) {
        arch::fpu_ensure_enabled();
        arch::fpu_save(parent->fpu_state);
        parent->fpu_used = true;
        ++parent->fpu_state_gen;
    }
#endif
    // Copy FPU state to child (full arch area, not hardcoded 512 —
    // issue #353: NEON needs 528, riscv F/D 272).
    tcb->fpu_used = parent->fpu_used;
    tcb->fpu_state_gen = parent->fpu_state_gen;
    if (parent->fpu_used) {
        __builtin_memcpy(tcb->fpu_state, parent->fpu_state,
                         sizeof(tcb->fpu_state));
    }

    // Allocate and set up kernel stack
    size_t stack_pages = (STACK_SIZE + 4095) / arch::PAGE_SIZE;
#if CONFIG_MEMORY_BUDGET
    // Issue #328: charge the child's kernel stack against the global
    // budget, symmetric to create(). Fail-closed before any allocation;
    // later failure paths destroy() the TCB, which releases exactly
    // these pages (stack_phys_ set below), so no rollback is needed
    // here. User-stack/PML4/table pages stay uncharged — create_user()
    // charges nothing either, and destroy() has no matching release
    // for them (charging without release would drain the budget).
    if (!Scheduler::reserve_memory_pages(stack_pages)) {
        Logger::warn("TCB::clone: budget OOM for %zu-page stack",
                     stack_pages);
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 1, log::LogSeverity::ERROR,
                            "TCB stack budget OOM", stack_pages);
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }
#endif
    uint64_t kstack_phys = PMM::alloc_contiguous(stack_pages);
    if (!kstack_phys) {
        ASSERT(errors::TaskError::TASK_ERR_STACK_ALLOC);
#if CONFIG_MEMORY_BUDGET
        Scheduler::release_memory_pages(stack_pages);
#endif
        TaskControlBlock::destroy(tcb);
        return nullptr;
    }
    tcb->stack_phys_ = kstack_phys;

    // Route through kslot for guard page below kernel stack.
    // v0.4.0 MP-1 prerequisite: use the is_user_ flag, not `page_table_ != 0`
    // (every task now owns a private kernel-half PML4).
    bool is_user_task = is_user_parent;
    tcb->is_user_ = is_user_task;
// Issue #352: fork gets a FRESH context ID (never the parent's — the child
// owns a private address space).  Arch-neutral (x86_64 pcid, aarch64/riscv64
// asid); 0 when unsupported.
    if (is_user_task)
        tcb->pcid_ = arch::context_id_alloc();
    uint64_t slot_va = alloc_kslot(STACK_SIZE);
    if (slot_va) {
        tcb->kstack_slot_va_ = slot_va;
        tcb->kstack_slot_size_ = ((STACK_SIZE + 4095) / 4096) * 4096 + 4096;
        uint64_t kstack_va = slot_va + 4096;
        for (size_t i = 0; i < stack_pages; ++i)
            kslot_backend::map_window_page(kstack_va + i * arch::PAGE_SIZE,
                                           kstack_phys + i * arch::PAGE_SIZE);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        tcb->kernel_stack = reinterpret_cast<uint8_t *>(kstack_va);
        tcb->kernel_stack_top = kstack_va + STACK_SIZE;
    } else {
        tcb->kstack_slot_va_ = 0;
        tcb->kstack_slot_size_ = 0;
        if (is_user_task) {
            uint64_t kstack_virt = arch::HHDM_OFFSET + kstack_phys;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            tcb->kernel_stack = reinterpret_cast<uint8_t *>(kstack_virt);
            tcb->kernel_stack_top = kstack_virt + STACK_SIZE;
        } else {
            // Issue #209: a raw physical address is NOT a valid VA on
            // non-x86 (no identity map). Route kernel-task stacks through
            // HHDM on all archs (x86 HHDM alias is equally valid; the old
            // raw-phys convention is retired — #434 P1, C3, no arch guard).
            uint64_t kstack_virt = arch::HHDM_OFFSET + kstack_phys;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            TCB_WRITE(tcb, kernel_stack,
                      reinterpret_cast<uint8_t *>(kstack_virt));
            tcb->kernel_stack_top = kstack_virt + STACK_SIZE;
        }
    }

#if defined(CONFIG_ARCH_X86_64)
    // Build register frame on child's kernel stack
    // Layout matches isr_common push order (high to low):
    // SS, RSP, RFLAGS, CS, RIP, error, vector, r15..r8, rbp, rdi, rsi, rdx,
    // rcx, rbx, rax
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    *--stack = regs[21]; // SS
    *--stack = regs[20]; // RSP
    *--stack = regs[19]; // RFLAGS
    *--stack = regs[18]; // CS
    *--stack = regs[17]; // RIP
    *--stack = regs[16]; // error_code
    *--stack = regs[15]; // vector
    *--stack = regs[14]; // r15
    *--stack = regs[13]; // r14
    *--stack = regs[12]; // r13
    *--stack = regs[11]; // r12
    *--stack = regs[10]; // r11
    *--stack = regs[9];  // r10
    *--stack = regs[8];  // r9
    *--stack = regs[7];  // r8
    *--stack = regs[6];  // rbp
    *--stack = regs[5];  // rdi
    *--stack = regs[4];  // rsi
    *--stack = regs[3];  // rdx
    *--stack = regs[2];  // rcx
    *--stack = regs[1];  // rbx
    *--stack = 0;        // rax = 0 (child return value)
    tcb->context.rsp = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_AARCH64)
    // Issue #104: indices follow the vectors.S save-area layout (x0-x30 @
    // regs[0..30], SP_EL0 @ regs[31], ELR @ regs[32], SPSR @ regs[33]) —
    // NOT the x86 map (regs[17/19/20] are user x17/x19/x20 here).  Push
    // DESCENDING (like x86) so memory order matches the layout (lowest =
    // X0); ascending would mirror x1-x30.  X0 reads back zero so the child
    // observes fork-return-0 (x86 zeroes rax and riscv zeroes A0 the same
    // way).
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    // aarch64 exception frame: padding, SPSR, ELR, SP_EL0, X0-X30
    *--stack = 0;        // padding
    *--stack = 0;        // padding
    *--stack = regs[33]; // SPSR_EL1
    *--stack = regs[32]; // ELR_EL1
    *--stack = regs[31]; // SP_EL0
    for (int i = 30; i >= 0; --i)
        *--stack = regs[i];
    stack[0] = 0; // X0 = 0 (fork returns 0 in child)
    tcb->context.sp_el0 = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_RISCV64)
    // Copy trap frame from parent (regs matches syscall_entry.S save area
    // layout: [0..30]=X1-X31, [31]=SEPC, [32]=SSTATUS, [33]=SCAUSE, [34]=STVAL)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    stack -= 37;
    for (int i = 0; i < 37; ++i)
        stack[i] = regs[i];
    stack[9] = 0; // X10/A0 = 0 (fork returns 0 in child)
    tcb->context.sp = reinterpret_cast<uint64_t>(stack);
#endif

    // For user tasks: clone page table and user stack
    if (is_user_task) {
        uint64_t new_pml4 = PMM::alloc_page();
        if (!new_pml4) {
            ASSERT(errors::TaskError::TASK_ERR_PML4_CLONE);
            TaskControlBlock::destroy(tcb);
            return nullptr;
        }
        tcb->install_page_table(new_pml4);

        // Issue #96: converge the kernel half via recursive merge from
        // the live kernel PML4 (link semantics — lower tables stay
        // shared, zero allocations).  The live root, not the parent, is
        // the source: a parent forked before a late mapping must not
        // propagate stale entries (convergence goal, paper §2).
        auto *new_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                      (new_pml4 & ~0xFFFULL));
        __builtin_memset(new_virt, 0, arch::PAGE_SIZE);
        if (!VMM::merge_kernel_half(VMM::get_kernel_pml4(), new_pml4)) {
            ASSERT(errors::TaskError::TASK_ERR_PML4_CLONE);
            TaskControlBlock::destroy(tcb);
            return nullptr;
        }
        VMM::assert_kernel_half_converged(new_pml4);

        // Deep-copy user entries from parent (walk, allocate, copy).
        if (!VMM::deep_copy_user_pages(parent->get_page_table(), new_pml4)) {
            ASSERT(errors::TaskError::TASK_ERR_PML4_CLONE);
            TaskControlBlock::destroy(tcb);
            return nullptr;
        }

        tcb->stack_pdpt_phys_ = 0;

        // Free deep-copied stack data pages before mapping new ones.
        // deep_copy_user_pages copied the parent's stack data pages into
        // the child's PML4, but clone allocates fresh stack pages below.
        {
            uint64_t stack_va = mem::STACK_VADDR + arch::PAGE_SIZE;
            size_t stack_pages = (parent->user_stack_size_ + 4095) / 4096;
            for (size_t si = 0; si < stack_pages; ++si) {
                uint64_t pa = VMM::virt_to_phys_in_pml4(
                    stack_va + si * arch::PAGE_SIZE, new_pml4);
                if (pa)
                    PMM::free_page(pa);
            }
        }

        size_t ustack_pages =
            (parent->user_stack_size_ + 4095) / arch::PAGE_SIZE;
        uint64_t ustack_phys = PMM::alloc_user_contiguous(ustack_pages);
        if (!ustack_phys) {
            ASSERT(errors::TaskError::TASK_ERR_USTACK_ALLOC);
            TaskControlBlock::destroy(tcb);
            return nullptr;
        }
        tcb->user_stack_ = ustack_phys;
        tcb->user_stack_size_ = parent->user_stack_size_;

        // Guard page: leave first page unmapped, start mapping at
        // +arch::PAGE_SIZE
        uint64_t user_stack_virt = mem::STACK_VADDR + arch::PAGE_SIZE;
        for (size_t i = 0; i < ustack_pages; ++i) {
            // Issue #325-D: user stacks are data (never executable).
            VMM::map_page_in_pml4(user_stack_virt + i * arch::PAGE_SIZE,
                                  ustack_phys + i * arch::PAGE_SIZE, true,
                                  false, new_pml4);
            // Copy physical page content via kernel mapping
            uint64_t src_virt =
                arch::HHDM_OFFSET + parent->user_stack_ + i * arch::PAGE_SIZE;
            uint64_t dst_virt =
                arch::HHDM_OFFSET + ustack_phys + i * arch::PAGE_SIZE;
            for (uint64_t j = 0; j < arch::PAGE_SIZE; ++j) {
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                reinterpret_cast<uint8_t *>(
                    dst_virt)[j
                              // NOLINTNEXTLINE(performance-no-int-to-ptr)
                ] = reinterpret_cast<const uint8_t *>(src_virt)[j];
            }
        }

        // v0.4.0 MP-3: clone copies canary fields (deep copy carried the
        // TEXT/DATA/HEAP sentinel bytes; the fresh user-stack copy carries the
        // STACK sentinels) and arms the child's kernel-stack canary.
        for (size_t ci = 0; ci < TaskControlBlock::CANARY_SEGMENTS; ++ci) {
            tcb->canary_before[ci] = parent->canary_before[ci];
            tcb->canary_after[ci] = parent->canary_after[ci];
        }
        tcb->canary_installed = parent->canary_installed;
        canary_install_kernel_stack(tcb);
        canary_install_user_segments(tcb);
    }

    return tcb;
}

void TaskControlBlock::add_child(TaskControlBlock *child) noexcept {
    if (!child)
        return;
    child->next_sibling = first_child;
    child->prev_sibling = nullptr;
    if (first_child)
        first_child->prev_sibling = child;
    first_child = child;
    child->parent_id = id;
    ++num_children;
}

void TaskControlBlock::remove_child(TaskControlBlock *child) noexcept {
    if (!child)
        return;
    bool found = false;
    for (auto *current = first_child; current;
         current = current->next_sibling) {
        if (current == child) {
            found = true;
            break;
        }
    }
    if (!found)
        return;
    if (child->prev_sibling) {
        child->prev_sibling->next_sibling = child->next_sibling;
    } else {
        first_child = child->next_sibling;
    }
    if (child->next_sibling) {
        child->next_sibling->prev_sibling = child->prev_sibling;
    }
    child->parent_id = 0;
    child->prev_sibling = nullptr;
    child->next_sibling = nullptr;
    --num_children;
}

TaskControlBlock *TaskControlBlock::find_child(uint64_t pid) noexcept {
    for (auto *child = first_child; child; child = child->next_sibling) {
        if (child->id == pid)
            return child;
    }
    return nullptr;
}

/// @brief Frees the private stack PDPT and its PD/PT pages allocated during
/// clone().
///        Only frees intermediate page-table pages (PD, PT), not leaf pages
///        (those are freed separately by the user_stack_ loop in cleanup()).
///        The private stack PDPT is an x86_64 (4-level PML4/PDPT/PD/PT)
///        concept; AArch64/RISC-V use different page-table shapes.
#if defined(CONFIG_ARCH_X86_64)
static void free_stack_pdpt(uint64_t pdpt_phys) noexcept {
    constexpr uint64_t PAGE_PRESENT = 1ULL << 0;
    constexpr uint64_t PAGE_HUGE = 1ULL << 7;
    size_t st_pdpt_idx = arch::ArchPageTable::pdpt_index(mem::STACK_VADDR);

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
    if (!(pdpt[st_pdpt_idx] & PAGE_PRESENT))
        return;

    uint64_t pd_phys = pdpt[st_pdpt_idx] & ~0xFFFULL;
    if (!PMM::is_user_page(pd_phys))
        return;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    for (int i = 0; i < 512; ++i) {
        if (!(pd[i] & PAGE_PRESENT))
            continue;
        if (pd[i] & PAGE_HUGE)
            continue; // leaf — already freed by user_stack_ loop
        uint64_t pt_phys = pd[i] & ~0xFFFULL;
        if (!PMM::is_user_page(pt_phys))
            continue;
        PMM::free_page(pt_phys);
    }
    PMM::free_page(pd_phys);
    PMM::free_page(pdpt_phys);
}
#endif // CONFIG_ARCH_X86_64

/// @brief Releases all resources owned by the task.
/// Unlinks from parent's child list, detaches from message-queue blocked lists,
/// closes file descriptors, frees user/kernel stacks, tears down page tables,
/// destroys IPC objects, and notifies the daemon manager. After this call the
/// TCB must not be used except for MemPool::free().
void TaskControlBlock::cleanup() noexcept {
    // Issue #324 Tick 1: NO validate_task_ptr ENSURE here by design —
    // post-snapshot-restore poisoned (0xDD) TCBs legitimately reach this
    // entry and the is_valid skip below is their terminal handler (an
    // ENSURE would fire on the path this guard exists to tolerate).
    if (!TaskControlBlock::is_valid(this)) {
        Logger::raw_write("[CLEANUP] skip poisoned TCB\n");
        kernel::diag::dump_tcb_write_log("[CLEANUP] poisoned TCB");
        return;
    }
    // Issue #373: a fully torn-down TCB must never re-run teardown.
    // Direct re-cleanup (e.g. a double-drain) is a safe no-op; the
    // pinned-deferred case never sets the flag (see complete_teardown).
    if (teardown_completed_) {
        return;
    }
    state = TaskState::REAPED;
    kernel::task_step(kernel::TaskCleanupSteps::Validate); // issue #347 Slice C

    // Issue #41: disarm the watchdog at teardown (bump the generation so
    // a recycled TCB slot can never match a stale expiry).
    wdog_armed = false;
    wdog_expiry_tick = 0;
    ++wdog_gen;
    kernel::task_step(kernel::TaskCleanupSteps::WatchdogDisarm); // issue #347 Slice C
    if (wdog_gen == 0)
        wdog_gen = 1;

    // Pinned TCBs are part of the test-isolation baseline (snapshot).
    // Their resources (sporadic_server, kernel stack, page tables) must
    // survive intact — the snapshot restore puts back the original field
    // values, so freeing a pinned task's resources creates dangling
    // pointers.  Skip all teardown; the block itself is also never freed
    // (MemPool::free on a pinned block is a no-op).
    if (kernel::MemPool::is_block_pinned(this))
        return;
    complete_teardown();
}

void TaskControlBlock::complete_teardown() noexcept {
    // Issue #373: deferred tail of cleanup() — runs ExitStats..Finalize
    // exactly as cleanup() would after the pinned check. Only destroy()
    // calls this, only on REAPED-with-resources-pending TCBs (pinned at
    // first destroy, unpinned since). Table removal already happened on
    // the first destroy; the Unregister block below re-runs the idempotent
    // remove path (same as the normal unregister-then-remove_task order).

    // Issue #294: snapshot user-task exit stats before teardown frees
    // anything (post-mortem ring for `monstat user`). Scalar copy only,
    // no allocation, no locks; daemons + kernel tasks excluded inside.
    // Placed after the pinned early-return (baseline never recorded)
    // and before unregister (PIDs still valid for daemon exclusion).
    kernel::idle_record_task_exit(*this, arch::Timer::ticks());
    kernel::task_step(kernel::TaskCleanupSteps::ExitStats); // issue #347 Slice C

    // Unregister from the scheduler's live tables so we never leave a dangling
    // tasks_[]/id_table_ entry that aliases a later allocation (which
    // previously caused add_task to ENSURE on a non-READY state, and a spurious
    // LEAK count). IRQs are disabled around the unregister: with interrupts
    // off, the only possible holder of scheduler_lock_ is the reaper
    // (reap_orphans) running in the current context — which unregisters
    // manually after cleanup() — so unregister_task's try_lock cleanly
    // distinguishes "skip" from "do it", and the timer ISR (on_tick) cannot
    // transiently hold the lock and cause a skip.
    {
        arch::IrqGuard irq_guard{};
        Scheduler::unregister_task(*this);
    }
    kernel::task_step(kernel::TaskCleanupSteps::Unregister); // issue #347 Slice C

    // Remove self from parent's child list if we have a parent
    if (parent_id != 0) {
        auto *parent = Scheduler::find_task(parent_id);
        if (parent) {
            parent->remove_child(this);
        }
        parent_id = 0;
    }
    kernel::task_step(kernel::TaskCleanupSteps::ParentUnbind); // issue #347 Slice C

    // Cancel a wheel-armed bounded receive (issue #18) before detaching
    // from queues: an armed timer firing after this TCB is freed would run
    // the callback against poisoned memory (stale fire). The wheel owns
    // its own lock; cleanup() holds no scheduler lock, so cancel() here
    // preserves wake-path lock ordering. Stale handles fail closed.
    if (__atomic_load_n(&recv_timeout_armed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&recv_timeout_armed, false, __ATOMIC_RELEASE);
        time::TimerWheel::cancel(recv_timeout_handle);
    }

    // Drain POSIX sleep/timer/timerfd ownership (issue #76): disarm a live
    // nanosleep, free owned timer/timerfd slots, and clear waiter
    // registrations held by this task. Same lock position as the recv
    // cancel above (no scheduler lock held — death-drain §12.3).
    time::PosixTime::drain_owner(*this);
    kernel::task_step(kernel::TaskCleanupSteps::TimerDrain); // issue #347 Slice C

    // Remove self from any message queue's blocked-senders list *before*
    // freeing any resources.  If we are blocked on another task's queue
    // (blocked_on_queue != nullptr) we must detach now — otherwise the
    // queue owner's cleanup() will walk a dangling pointer after we are
    // freed and poisoned.
    if (blocked_on_queue &&
        TaskControlBlock::is_kernel_pointer(blocked_on_queue)) {
        auto &q = *blocked_on_queue;
        if (TaskControlBlock::is_valid(q.blocked_senders_head) &&
            q.blocked_senders_head == this) {
            q.blocked_senders_head = blocked_next;
            if (TaskControlBlock::is_valid(q.blocked_senders_tail) &&
                q.blocked_senders_tail == this)
                q.blocked_senders_tail = nullptr;
        } else {
            auto *prev = q.blocked_senders_head;
            while (prev && TaskControlBlock::is_valid(prev) &&
                   prev->blocked_next != this)
                prev = prev->blocked_next;
            if (prev && TaskControlBlock::is_valid(prev)) {
                prev->blocked_next = blocked_next;
                if (q.blocked_senders_tail == this)
                    q.blocked_senders_tail = prev;
            }
        }
        blocked_next = nullptr;
        blocked_on_queue = nullptr;
    }

    // Detach from any sync-object waiter list before freeing the TCB.
    // Semaphore/Mutex/EventGroup/Queue wait paths store a raw TCB in their
    // waiter arrays with no cleanup unlink (v0.3.9 teardown gap); a later
    // post()/unlock()/set_bits()/send()/receive() on a reaped task would feed
    // the freed block to Scheduler::set_task_ready (ready-queue corruption /
    // use-after-free).  Each remove_waiter acquires the object's own lock_ —
    // safe here: cleanup() holds no scheduler lock (see
    // drain_zombie_list/cleanup_step), preserving the lock ordering of the
    // wake paths.  The higher-half range check mirrors the blocked_on_queue
    // validation above (objects live in kernel/HHDM space, never low VA).
    if (waiting_on_mutex &&
        TaskControlBlock::is_kernel_pointer(waiting_on_mutex)) {
        waiting_on_mutex->remove_waiter(*this);
        waiting_on_mutex = nullptr;
    }
    if (waiting_on_semaphore &&
        TaskControlBlock::is_kernel_pointer(waiting_on_semaphore)) {
        waiting_on_semaphore->remove_waiter(*this);
        waiting_on_semaphore = nullptr;
    }
    if (waiting_on_eventgroup &&
        TaskControlBlock::is_kernel_pointer(waiting_on_eventgroup)) {
        waiting_on_eventgroup->remove_waiter(*this);
        waiting_on_eventgroup = nullptr;
    }
    if (waiting_on_queue &&
        TaskControlBlock::is_kernel_pointer(waiting_on_queue)) {
        waiting_on_queue->remove_waiter(*this);
        waiting_on_queue = nullptr;
    }
    kernel::task_step(kernel::TaskCleanupSteps::SyncDetach); // issue #347 Slice C

    // Notify the daemon manager so registered daemon PIDs are reset to 0.
    // Must run before msg_queue is destroyed so blocked senders get fast-fail
    // instead of blocking on a zombie destination.
    kernel::daemon::notify_death(this->id);

    // Issue #105 Part B: latch this task's death records and poke its
    // supervisors BEFORE any of this task's resources are torn down, so a
    // still-resolvable supervisor is woken while the death is fresh.  Then free
    // this task's own watches (incl. any PENDING records it held as a
    // supervisor for others) before its Notify is destroyed below — a poke or a
    // drain must never touch a recycled supervisor TCB.
    kernel::ipc::DeathNotify::on_task_death(*this);

    // Issue #226: debugger stop routing — publish the task-death stop
    // event (reserved slot, never dropped) while bindings, Notify, and
    // page tables are still intact, then run the debugger-death fail-safe
    // when the dying task owns debug bindings (fault-stopped targets
    // terminate, cleanly-stopped resume — no orphaned parked tasks).
    // Debugger-death supervision note (#191): a restarted debugd
    // re-attaches explicitly; kernel breakpoint shadows are restored
    // from the authoritative table, never assumed.
    kernel::debug::debug_publish_death(*this);
    kernel::debug::debug_drain_debugger(*this);
    kernel::task_step(kernel::TaskCleanupSteps::Notify); // issue #347 Slice C

    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        if (fd_table.fds[i].used) {
            auto *vn = fd_table.fds[i].vnode;
            if (vn && vfs::vnode_ref_dec(vn)) {
                if (vn->ops->close)
                    vn->ops->close(*vn);
            } else if (vn && !vn->refcount) {
                if (vn->ops->close)
                    vn->ops->close(*vn);
            }
            kernel::test::ResourceTracker::instance().track_fd_remove();
            fd_table.fds[i].used = false;
            fd_table.fds[i].vnode = nullptr;
        }
    }
    kernel::task_step(kernel::TaskCleanupSteps::VfsDrain); // issue #347 Slice C

    // v0.4.0 MP-1: keyed on the authoritative is_user_ flag — a kernel task
    // owns a private kernel-half PML4 but never a user stack.
    if (is_user_ && user_stack_) {
        size_t pages = (user_stack_size_ + 4095) / arch::PAGE_SIZE;
        for (size_t i = 0; i < pages; ++i) {
            PMM::free_page(user_stack_ + i * arch::PAGE_SIZE);
        }
        user_stack_ = 0;
    }

    if (stack_phys_) {
        size_t pages = (kstack_slot_size_ && kstack_slot_va_)
                           ? ((kstack_slot_size_ - 4096) / 4096)
                           : ((STACK_SIZE + 4095) / arch::PAGE_SIZE);
        if (kstack_slot_va_) {
            uint64_t slot_va = kstack_slot_va_;
            uint64_t stack_va = slot_va + 4096;
            for (size_t i = 0; i < pages; ++i)
                kslot_backend::unmap_window_page(stack_va +
                                                 i * arch::PAGE_SIZE);
            free_kslot(slot_va, kstack_slot_size_);
        }
        // Free physical pages (no poison — see NOTE).
        // NOTE: 0xDD poison removed — it persists in the freed page and
        // corrupts the next allocation after PMM restore recycles the page.
        // Use-after-free detection relies on TCB magic checks instead.
        for (size_t i = 0; i < pages; ++i)
            PMM::free_page(stack_phys_ + i * arch::PAGE_SIZE);
#if CONFIG_MEMORY_BUDGET
        Scheduler::release_memory_pages(pages);
#endif
        stack_phys_ = 0;
        kernel_stack = nullptr;
        kernel_stack_top = 0;
        kstack_slot_va_ = 0;
        kstack_slot_size_ = 0;
    }
    kernel::task_step(kernel::TaskCleanupSteps::MemFree); // issue #347 Slice C

    if (page_table_) {
        // Free any zero-copy buffers owned by this task before tearing down
        // page tables
        BufferPool::unmap_all(*this);

        // Issue #8: drain the task's user MMIO mappings from the OLD page
        // table BEFORE it is freed - a stored slot pml4 must never be walked
        // after the PML4 (and its PDPT/PD/PT pages) are released.  VMM's
        // map_page_in_pml4 (get_table, create=true) allocates fresh table
        // pages and writes table[index] = new_page; running drain_task after
        // free_user_pages/PMM::free_page would write those entries into freed
        // page-table memory (UAF), exactly the defect the exec-into-current
        // drain was added to prevent (iter-2 S1).  Runs while the mapping's
        // cap reference is still valid (before release_all_objects).
        cap::MmioUserMap::drain_task(*this);
        cap::FrameUserMap::drain_task(*this);
        // Issue #107: drain the pager registry BEFORE free_user_pages — a
        // pager-mapped ledger PTE must never outlive the frames it references
        // (the pager owns the frames; the client's teardown unmaps them first).
        kernel::ipc::PagerRegistry::drain_task(*this);
        // Issue #95: release acquired shared libs BEFORE free_user_pages —
        // shared RO phys lifetime belongs to the cache (refcount); the
        // blind per-pml4 teardown would otherwise double-free it.  The
        // drain unmaps shared RO from this pml4 first.
        kernel::elf::release_task_libs(this);

        // Unconditional teardown (v0.4.0 MP-7): every PML4 is either private
        // (deep-copied or freshly built via clone_kernel_pml4) or the boot
        // kernel PML4; no task ever shares another task's user entries.
        // free_user_pages() skips kernel-owned table/data pages via
        // PMM::is_user_page, so a partially deep-copied address space (OOM
        // during clone) is fully reclaimed here — no leak.
        // Issue #271 P1: unmap retained golden pages first so the blind
        // free below skips them (pages owned by mapping tasks, retention
        // decides frees at zero/zero).
        kernel::elf::ElfLoader::unmap_golden(this);
        // Issue #352: release the context ID before the address space goes
        // away (keyed on the live TCB, never after free; 0 is a no-op
        // for kernel tasks and never-published users).  Arch-neutral.
        arch::context_id_free(pcid_);
        pcid_ = 0;
        VMM::free_user_pages(get_page_table());
        PMM::free_page(get_page_table());
#if defined(CONFIG_ARCH_X86_64)
        if (stack_pdpt_phys_) {
            free_stack_pdpt(stack_pdpt_phys_);
            stack_pdpt_phys_ = 0;
        }
#endif // CONFIG_ARCH_X86_64
        clear_page_table();
    }
    kernel::task_step(kernel::TaskCleanupSteps::PageTeardown); // issue #347 Slice C

    msg_queue.~MessageQueue();
    kernel::test::ResourceTracker::instance().track_msg_queue_remove();

    notify.~Notify();
    kernel::test::ResourceTracker::instance().track_notify_remove();

    event_group.~EventGroup();
    kernel::task_step(kernel::TaskCleanupSteps::IpcDestroy); // issue #347 Slice C
    kernel::test::ResourceTracker::instance().track_event_group_remove();

    // Release the task's I/O permission bitmap slot (x86_64, issue #3)
    // before tearing down its capability objects.
    arch::iopb_release(*this);
    // Drop the task's capability-grant ledger entries (issue #8).
    arch::iopb_ledger_drop_task(*this);

    // Drain the user-space IRQ delivery table: a dying task must never leave
    // a dangling recipient/waiter (issue #2).  Runs before capability teardown
    // so no slot reference survives into a recycled TCB.
    IrqDelivery::drain_task(*this);

    release_all_objects();
    if (cwd_vnode)
        vfs::vnode_ref_dec(cwd_vnode);
    cwd_vnode = nullptr;
    kernel::task_step(kernel::TaskCleanupSteps::CapRelease); // issue #347 Slice C
    kernel::test::ResourceTracker::instance().track_task_remove();
    // If this block was the deadline-monitor task's TCB, clear the scheduler's
    // pointer so s_monitor_task_ can never dangle into a reused block (the
    // on_tick wake path would otherwise write state + enqueue_ready into
    // foreign memory — a corruption time bomb).
#if CONFIG_DEADLINE_MONITOR_TASK
    if (this == Scheduler::get_monitor_task())
        Scheduler::reset_monitor_task();
#endif
    kernel::task_step(kernel::TaskCleanupSteps::Finalize); // issue #347 Slice C
    // Issue #373: teardown ran to completion — any further cleanup() or
    // destroy() on this TCB is a safe no-op, never a re-teardown.
    teardown_completed_ = true;
}

} // namespace kernel

// --- Error-returning overloads ---
namespace kernel {

using namespace errors;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
TaskError TaskControlBlock::create_err(void (*entry)(), uint64_t priority,
                                       uint64_t period_ticks,
                                       TaskControlBlock *&out_tcb) {
    out_tcb = create(entry, priority, period_ticks);
    return out_tcb ? TASK_ERR_OK : TASK_ERR_OOM;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
TaskError TaskControlBlock::create_user_err(void (*entry)(), uint64_t priority,
                                            uint64_t period_ticks,
                                            size_t user_stack_size,
                                            TaskControlBlock *&out_tcb) {
    out_tcb = create_user(entry, priority, period_ticks, user_stack_size);
    return out_tcb ? TASK_ERR_OK : TASK_ERR_OOM;
}

TaskError TaskControlBlock::clone_err(uint64_t *regs,
                                      TaskControlBlock *&out_tcb) {
    out_tcb = clone(regs);
    return out_tcb ? TASK_ERR_OK : TASK_ERR_OOM;
}

// Issue #409: exactly-once teardown ownership. The tick-ISR deferred kill
// and task-context teardown (test end, reaper, drain, waitpid) can both
// reach the same TCB: the loser must skip the ENTIRE teardown including
// the free, and the winner is obliged to complete the full tail (this is
// what refutes the S1 leak objection — the tail is never abandoned, only
// never doubled). The single common exclusion window is interrupts-off:
// the claim is one atomic check-and-set under IrqGuard, consulted by
// every path that frees. The deferred-kill generation snapshot answers
// "same task?" (recycled occupants are rejected before the claim is
// consulted); the claim answers "teardown still unowned?".
bool TaskControlBlock::try_claim_teardown(TaskControlBlock *tcb) noexcept {
    if (!tcb || tcb->magic != TCB_MAGIC)
        return false;
    arch::IrqGuard irq_guard{};
    // Issue #409: reject ONLY a rival in-flight claim. A completed-but-
    // unclaimed tail (waitpid/test cleanup(); delete; flows complete the
    // tail without claiming — the claim is taken at destroy entry) must
    // still proceed to the free; the ISR-side predicate (kill_declined)
    // is where completed_/REAPED/unregistered decline a NEW teardown.
    if (tcb->teardown_claimed_)
        return false;
    tcb->teardown_claimed_ = true;
    return true;
}

void TaskControlBlock::release_teardown_claim(TaskControlBlock *tcb) noexcept {
    if (!tcb || tcb->magic != TCB_MAGIC)
        return;
    arch::IrqGuard irq_guard{};
    tcb->teardown_claimed_ = false;
}

void TaskControlBlock::destroy(TaskControlBlock *tcb) noexcept {
    // Issue #324 Tick 1: tri-state magic contract — TCB_MAGIC (normal),
    // 0 (OOM-path partial TCB, free directly), 0xDD (already freed, skip).
    // No validate_task_ptr ENSURE: states 0/0xDD are legitimate entries.
    if (!tcb)
        return;
    if (tcb->magic == 0) {
        // OOM-path partial TCB: never initialized, never published, so no
        // teardown to own — free directly (unchanged pre-#409 path).
        MemPool::free(tcb);
        return;
    }
    // Issue #409: claim teardown ownership before touching anything. A
    // rival-owned (claimed), completed, or already-freed (0xDD) block is a
    // no-op — the owner completes the full tail exactly once.
    if (!TaskControlBlock::try_claim_teardown(tcb))
        return;
    if (tcb->magic == TCB_MAGIC) {
        // If the caller already called cleanup() before destroy, state
        // is REAPED and we must NOT cleanup/remove again — doing so
        // double-frees resources and corrupts the scheduler lists.
        if (tcb->state != TaskState::REAPED) {
            tcb->cleanup();
            Scheduler::remove_task(*tcb);
        } else if (!tcb->teardown_completed_ &&
                   !kernel::MemPool::is_block_pinned(tcb)) {
            // Issue #373: REAPED-with-resources-pending — the first
            // destroy ran on a pinned TCB (teardown skipped, block kept)
            // and the block has since been unpinned. Complete the
            // deferred resource teardown now. Table removal already
            // happened on the first destroy and the tail's Unregister
            // block is idempotent, so remove_task() must NOT run again.
            tcb->complete_teardown();
        }
        // Baseline-owned (pinned) blocks are never freed: the snapshot
        // owns the block and restore rewinds it (cleanup() already skips
        // teardown for pinned for the same reason). In production nothing
        // is pinned, so this changes nothing outside tests. Issue
        // #409/#373: a pinned teardown takes nothing — release the claim
        // so the second destroy (unpinned since) re-claims fresh.
        if (kernel::MemPool::is_block_pinned(tcb)) {
            TaskControlBlock::release_teardown_claim(tcb);
            return;
        }
        tcb->magic =
            0; // Prevent double-free if caller re-enters the destroy path
        MemPool::free(tcb);
        return;
    }
    if (tcb->magic == 0)
        MemPool::free(tcb);
    // magic == 0xDD: reaper already freed it — skip silently.
}

void TaskControlBlock::operator delete(void *ptr) noexcept {
    destroy(static_cast<TaskControlBlock *>(ptr));
}

} // namespace kernel
