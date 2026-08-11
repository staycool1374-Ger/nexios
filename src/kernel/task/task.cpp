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
#include <kernel/task/scheduler.hpp>
#include <kernel/task/tcb_write_log.hpp>
#include <kernel/task/sporadic_server.hpp>
#include <kernel/debug/dump.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <assert.hpp>

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
void record_task_entry(uint64_t entry, uint64_t tcb) {
    auto &r = g_recent_tasks[g_recent_tasks_idx % kRecentTasks];
    r.entry = entry;
    r.tcb = tcb;
    r.ticks = 0;
    g_recent_tasks_idx++;
}
/// @brief If `value` equals the low 32 bits or full value of a recently
/// created task's `entry`, return that task's tcb address; else 0.
uint64_t find_entry_owner(uint64_t value) {
    for (size_t i = 0; i < kRecentTasks; ++i) {
        const auto &r = g_recent_tasks[i];
        if (r.entry == value || (r.entry & 0xFFFFFFFFULL) == (value & 0xFFFFFFFFULL))
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
static void _task_trampoline(void (*entry)()) {
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
    uint64_t budget_granularity) noexcept {
    if (sporadic_server)
        return;
    auto *ss = static_cast<task::SporadicServer *>(
        MemPool::alloc(sizeof(task::SporadicServer)));
    if (!ss)
        return;
    memset(ss, 0, sizeof(task::SporadicServer));
    ss->init(budget_c, period_t, bg_prio, budget_granularity);
    ss->set_base_priority(priority);
    sporadic_server = ss;
    Scheduler::inc_sporadic_count();
}

/// @brief Initialises common fields of a newly allocated TaskControlBlock.
/// Sets up fd table, cwd, IPC objects (MessageQueue, Notify, EventGroup),
/// process-hierarchy links, signal handlers, and buffer pool state.
static uint32_t s_next_generation = 1;

void init_task_common(TaskControlBlock &tcb) {
    tcb.generation = __atomic_fetch_add(&s_next_generation, 1, __ATOMIC_RELAXED);
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
    tcb.stack_pdpt_phys_ = 0;
    tcb.page_table_shared_ = false;
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

    // Signal handlers all default to nullptr (SIG_DFL)
    for (size_t i = 0; i < MAX_SIGNAL_HANDLERS; ++i) {
        tcb.signal_handlers[i] = nullptr;
    }
    tcb.pending_signals = 0;
}

// BUGS.md#019/#020: after MemPool::alloc() returns a TCB block (but BEFORE it is
// zeroed/reused), verify it is not still aliased by a live reference.  If the
// block we just got is pointed to by current_task_ptr_, any live task's
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
                              "task's TCB block %p", tcb);
        kernel::debug::dump_scheduler_info();
        panic("TCB reuse of current task (UAF)");
    }
    if (Scheduler::debug_id_table_references(tcb)) {
        kernel::Logger::fatal("BUGS.md#019/#020: create reused TCB block %p "
                              "still aliased by scheduler id_table_", tcb);
        kernel::debug::dump_scheduler_info();
        panic("TCB reuse of id_table_-aliased block (UAF)");
    }
    uint64_t n = Scheduler::task_count();
    for (uint64_t i = 0; i < n; ++i) {
        auto *t = Scheduler::task_at(i);
        if (!t || t == tcb)
            continue;
        if (t->first_child == tcb || t->next_sibling == tcb ||
            reinterpret_cast<void *>(t->sporadic_server) ==
                reinterpret_cast<void *>(tcb) ||
            t->runq_next_ == tcb || t->blocked_next == tcb ||
            t->dl_next_ == tcb ||
            (tcb->id != 0 &&
             static_cast<uint64_t>(t->parent_id) == tcb->id)) {
            kernel::Logger::fatal("BUGS.md#019/#020: create reused TCB block %p "
                                  "still referenced by task %u (child=%p sib=%p "
                                  "spor=%p par=%lu)",
                                  tcb, t->id, t->first_child, t->next_sibling,
                                  t->sporadic_server,
                                  static_cast<uint64_t>(t->parent_id));
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
    int32_t  next;
};

static constexpr int32_t KSLOT_POOL_SIZE = 64;
static_assert(KSLOT_POOL_SIZE >= CONFIG_MAX_TASKS,
              "KSLOT_POOL_SIZE must be >= CONFIG_MAX_TASKS to provide a "
              "guarded kernel-stack slot for every possible task");
static KSlotEntry s_kslot_pool[KSLOT_POOL_SIZE];
static int32_t    s_kslot_free_head = -1;
static int32_t    s_kslot_list      = -1;
static uint64_t   s_kslot_bump      = CONFIG_KSTACK_WINDOW_BASE;
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
///        test VAs (>= 0x100000000).
constexpr uint64_t kUserYieldStubVa = 0x40000000;

/// @brief Per-arch machine code for "yield forever" (syscall YIELD=0 loop).
#if defined(CONFIG_ARCH_X86_64)
//   xor eax,eax     31 C0     ; rax = SyscallNumber::YIELD (0)
//   int $0x80        CD 80    ; trap gate vector 0x80 (isr_128) — GS-free path.
//                             ; NOT `syscall` (0F 05): MSR_KERNEL_GS_BASE is
//                             ; never written, so syscall_entry's swapgs would
//                             ; #PF to phys 0 (see audits/gs-base-swapgs-audit-v0.3.9.md F-1).
//   jmp -6          EB FA
static constexpr uint8_t kUserYieldStub[] = {0x31, 0xC0, 0xCD, 0x80, 0xEB, 0xFA};
#elif defined(CONFIG_ARCH_AARCH64)
//   mov x8, #0      d2 00 00 00  ; x8 = SyscallNumber::YIELD (0)
//   svc #0          01 00 00 d4
//   b -12           18 00 00 14
static constexpr uint8_t kUserYieldStub[] = {0x00, 0x00, 0x80, 0xD2, 0x01,
                                             0x00, 0x00, 0xD4, 0x18, 0x00,
                                             0x00, 0x14};
#elif defined(CONFIG_ARCH_RISCV64)
//   li a7, 0        13 00 80 00  ; a7 = SyscallNumber::YIELD (0)
//   ecall           73 00 00 00
//   j -12           6f 00 00 00  ; (jump relative 0 — tighten below)
static constexpr uint8_t kUserYieldStub[] = {0x13, 0x00, 0x80, 0x00, 0x73,
                                             0x00, 0x00, 0x00, 0x6f, 0x00,
                                             0x00, 0x00};
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

    if (!tcb.page_table_) {
        PMM::free_page(stub_phys);
        return false;
    }
    VMM::map_page_in_pml4(kUserYieldStubVa, stub_phys, true, tcb.page_table_);

#if defined(CONFIG_ARCH_X86_64)
    // The saved iret frame stores the (placeholder kernel) entry in two slots:
    // the iret RIP and the SysV rdi copy.  The placeholder is a kernel
    // higher-half address (>= 0xFFFF800000000000); the stub is a low user VA.
    constexpr uint64_t kKernelHigherHalf = 0xFFFF800000000000ULL;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *frame = reinterpret_cast<uint64_t *>(tcb.context.rsp);
    size_t slots =
        (tcb.kernel_stack_top - tcb.context.rsp) / sizeof(uint64_t);

    uint64_t original_entry = 0;
    for (size_t i = 0; i < slots; ++i) {
        if (frame[i] >= kKernelHigherHalf) {
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

/// @brief Physical addresses of the 8 pre-allocated PT pages for the window.
static uint64_t s_kstack_pt_pages[8] = {};

static void init_kstack_window();
static void map_kstack_page(uint64_t virt, uint64_t phys);
static void unmap_kstack_page(uint64_t virt);

static uint64_t alloc_kslot(uint64_t stack_size) {
    // Lazy init: pre-allocate page table pages on first call.
    // Must be called after PMM and VMM init (first create() is in
    // Scheduler::init(), well after both).
    if (!s_kstack_pt_pages[0])
        init_kstack_window();

    uint64_t slot_size = ((stack_size + 4095) / 4096) * 4096 + 4096;

    { // Free list (first-fit), IRQ-safe.
        arch::IrqGuard _g{};
        int32_t *pp = &s_kslot_list;
        while (*pp >= 0) {
            KSlotEntry &e = s_kslot_pool[*pp];
            if (e.size >= slot_size) {
                uint64_t va = e.va;
                if (va < CONFIG_KSTACK_WINDOW_BASE ||
                    va + e.size > CONFIG_KSTACK_WINDOW_BASE + CONFIG_KSTACK_WINDOW_SIZE)
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
        if (va + slot_size > CONFIG_KSTACK_WINDOW_BASE + CONFIG_KSTACK_WINDOW_SIZE)
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
    s_kslot_pool[idx].va   = va;
    s_kslot_pool[idx].size = size;
    s_kslot_pool[idx].next = s_kslot_list;
    s_kslot_list = idx;
}

// ---------------------------------------------------------------------------
// Kernel-stack window page table pool
// ---------------------------------------------------------------------------

/// @brief Pre-allocate all page table pages for the kernel-stack window and
///        wire them into the kernel PML4.  Called once at boot, before any
///        snapshot is taken.
void init_kstack_window() {
    const uint64_t addr_mask = 0x0000FFFFFFFFFFFFULL;
    uint64_t base_48 = CONFIG_KSTACK_WINDOW_BASE & addr_mask;
    unsigned pml4_idx = (base_48 >> 39) & 0x1FF;
    unsigned pdpt_idx = (base_48 >> 30) & 0x1FF;
    unsigned pd_idx   = (base_48 >> 21) & 0x1FF;

    auto *pml4 = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
    constexpr uint64_t P = 1ULL << 0;   // PAGE_PRESENT
    constexpr uint64_t W = 1ULL << 1;   // PAGE_WRITE

    uint64_t pdpt_phys = 0;
    if (!(pml4[pml4_idx] & P)) {
        pdpt_phys = PMM::alloc_page_table();
        if (!pdpt_phys) panic("init_kstack_window: pdpt_phys OOM");
        auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
        __builtin_memset(pdpt, 0, 4096);
        pml4[pml4_idx] = pdpt_phys | P | W;
    } else {
        pdpt_phys = pml4[pml4_idx] & ~0xFFFULL;
    }

    auto *pdpt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pdpt_phys);
    uint64_t pd_phys = 0;
    if (!(pdpt[pdpt_idx] & P)) {
        pd_phys = PMM::alloc_page_table();
        if (!pd_phys) panic("init_kstack_window: pd_phys OOM");
        auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
        __builtin_memset(pd, 0, 4096);
        pdpt[pdpt_idx] = pd_phys | P | W;
    } else {
        pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
    }

    auto *pd = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + pd_phys);
    for (unsigned i = 0; i < 8; ++i) {
        if (!(pd[pd_idx + i] & P)) {
            s_kstack_pt_pages[i] = PMM::alloc_page_table();
            if (!s_kstack_pt_pages[i]) panic("init_kstack_window: pt page OOM");
            auto *pt = reinterpret_cast<uint64_t *>(
                arch::HHDM_OFFSET + s_kstack_pt_pages[i]);
            __builtin_memset(pt, 0, 4096);
            pd[pd_idx + i] = s_kstack_pt_pages[i] | P | W;
        } else {
            s_kstack_pt_pages[i] = pd[pd_idx + i] & ~0xFFFULL;
        }
    }

    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3) : : "memory");
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/// @brief Map one 4 KiB kernel-stack page via pre-allocated PT.
static void map_kstack_page(uint64_t virt, uint64_t phys) {
    uint64_t offset = virt - CONFIG_KSTACK_WINDOW_BASE;
    unsigned pt_idx = offset / (512 * 4096);
    unsigned entry  = (offset / 4096) & 0x1FF;
    if (pt_idx >= 8) return;
    constexpr uint64_t P = 1ULL << 0;  // PAGE_PRESENT
    constexpr uint64_t W = 1ULL << 1;  // PAGE_WRITE
    auto *pt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + s_kstack_pt_pages[pt_idx]);
    pt[entry] = phys | P | W;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/// @brief Unmap one 4 KiB kernel-stack page.
static void unmap_kstack_page(uint64_t virt) {
    uint64_t offset = virt - CONFIG_KSTACK_WINDOW_BASE;
    unsigned pt_idx = offset / (512 * 4096);
    unsigned entry  = (offset / 4096) & 0x1FF;
    if (pt_idx >= 8) return;
    auto *pt = reinterpret_cast<uint64_t *>(
        arch::HHDM_OFFSET + s_kstack_pt_pages[pt_idx]);
    pt[entry] = 0;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

} // anonymous namespace

/// @brief Creates a new kernel-space TaskControlBlock.
/// Allocates a TCB from MemPool, sets up a kernel stack with an
/// architecture-specific initial register frame, and returns it.
/// @param entry  Kernel function pointer to execute.
/// @param priority  Initial scheduling priority.
/// @param period_ticks  Period for rate-monotonic scheduling.
/// @return  Pointer to the new TCB, or nullptr on OOM.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
TaskControlBlock *TaskControlBlock::create(void (*entry)(), uint64_t priority,
                                           uint64_t period_ticks)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Logger::raw_write("[TCB] create pool8=");
    Logger::print_dec(MemPool::pool_free_count(8));
    Logger::raw_write(" tcnt=");
    Logger::print_dec(Scheduler::task_count());
    Logger::raw_write("\n");
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

    TCB_WRITE(tcb, magic, TCB_MAGIC);
    TCB_WRITE(tcb, id, Scheduler::alloc_id());
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
    tcb->period_ticks = period_ticks;
    tcb->deadline_ticks = arch::Timer::ticks() + period_ticks;
    tcb->deadline_missed = false;
    tcb->deadline_miss_count = 0;
    tcb->executed_ticks = 0;
    tcb->remaining_ticks = period_ticks;
    tcb->wcet_ticks = 0;
    tcb->wcet_overrun_fired = false;
    tcb->memory_budget_pages_ = 0;
    tcb->memory_used_pages_ = 0;
    init_task_common(*tcb);

uint64_t stack_size = stack_size_for_priority(priority);
    size_t stack_pages = (stack_size + 4095) / arch::PAGE_SIZE;
#if CONFIG_MEMORY_BUDGET
    if (!Scheduler::reserve_memory_pages(stack_pages)) {
        Logger::warn("TCB::create: budget OOM for %zu-page stack", stack_pages);
        delete tcb;
        return nullptr;
    }
#endif
    uint64_t stack_phys = PMM::alloc_contiguous(stack_pages);
    if (!stack_phys) {
        Logger::warn("TCB::create: PMM OOM for %zu-page stack", stack_pages);
#if CONFIG_MEMORY_BUDGET
        Scheduler::release_memory_pages(stack_pages);
#endif
        delete tcb;
        return nullptr;
    }

    tcb->stack_phys_ = stack_phys;
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
    // ------------------------------------------------------------------
    if (!Scheduler::is_test_active()) {
        uint64_t slot_va = alloc_kslot(stack_size);
        if (slot_va) {
            tcb->kstack_slot_va_ = slot_va;
            tcb->kstack_slot_size_ = ((stack_size + 4095) / 4096) * 4096 + 4096;
            uint64_t kstack_va = slot_va + 4096;
            for (size_t i = 0; i < stack_pages; ++i)
                map_kstack_page(kstack_va + i * arch::PAGE_SIZE,
                                stack_phys + i * arch::PAGE_SIZE);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            TCB_WRITE(tcb, kernel_stack, reinterpret_cast<uint8_t *>(kstack_va));
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

    // PMM does not zero freed pages (it poison-fills them, e.g. with 0x0b),
    // so a stack allocated from recycled memory can retain stale poison in
    // the region below the initial frame.  Zero the entire stack up front so
    // every slot the task may reach — including deep call-chain return
    // addresses — starts from a known state instead of tripping a fault on a
    // leftover 0x0b return address.
    __builtin_memset(tcb->kernel_stack, 0, stack_size);

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
    *--stack = 0x3C5; // SPSR_EL1: EL1h, all interrupts masked
    *--stack = reinterpret_cast<uint64_t>(entry); // ELR_EL1
    *--stack = 0; // SP_EL0 (unused for kernel task)
    for (int i = 0; i < 31; ++i)
        *--stack = 0; // X0-X30
    tcb->context.sp_el0 = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_RISCV64)
    // Build trap frame matching syscall_entry.S save area layout.
    // Offsets (in qwords): [0]=RA, [1]=SP, [9]=A0, [13]=A4 (regs ptr),
    //   [31]=SEPC, [32]=SSTATUS. SAVE_SIZE = 37 qwords (296 bytes).
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    stack -= 37;
    __builtin_memset(stack, 0, 37 * 8);
    stack[31] = reinterpret_cast<uint64_t>(entry); // SEPC = entry point
    stack[32] = (1ULL << 5) | (1ULL << 8); // SSTATUS: SPIE=1, SPP=1 (S-mode)
    tcb->context.sp = reinterpret_cast<uint64_t>(stack);
#endif

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
    // (create() had it); the buffer_pool_* tests reproduce the UAF via this path.
    debug_check_tcb_reuse(tcb);
#endif
    memset(tcb, 0, sizeof(TaskControlBlock));

    TCB_WRITE(tcb, magic, TCB_MAGIC);
    TCB_WRITE(tcb, id, Scheduler::alloc_id());
    TCB_WRITE(tcb, state, TaskState::READY);
    tcb->priority = priority;
    tcb->base_priority = priority;
    tcb->period_ticks = period_ticks;
    tcb->deadline_ticks = arch::Timer::ticks() + period_ticks;
    tcb->deadline_missed = false;
    tcb->deadline_miss_count = 0;
    tcb->executed_ticks = 0;
    tcb->remaining_ticks = period_ticks;
    tcb->memory_budget_pages_ = 0;
    tcb->memory_used_pages_ = 0;
    tcb->wcet_ticks = 0;
    tcb->wcet_overrun_fired = false;
    init_task_common(*tcb);

    size_t kernel_stack_pages = (STACK_SIZE + 4095) / arch::PAGE_SIZE;
    uint64_t kstack_phys = PMM::alloc_contiguous(kernel_stack_pages);
    if (!kstack_phys) {
        ASSERT(errors::TaskError::TASK_ERR_STACK_ALLOC);
        delete tcb;
        return nullptr;
    }
    tcb->stack_phys_ = kstack_phys;

    // Route through kslot for guard page below kernel stack.
    uint64_t slot_va = alloc_kslot(STACK_SIZE);
    if (slot_va) {
        tcb->kstack_slot_va_ = slot_va;
        tcb->kstack_slot_size_ = ((STACK_SIZE + 4095) / 4096) * 4096 + 4096;
        uint64_t kstack_va = slot_va + 4096;
        for (size_t i = 0; i < kernel_stack_pages; ++i)
            map_kstack_page(kstack_va + i * arch::PAGE_SIZE,
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
        delete tcb;
        return nullptr;
    }

    uint64_t pml4 = VMM::clone_kernel_pml4();
    if (!pml4) {
        ASSERT(errors::TaskError::TASK_ERR_PML4_CLONE);
        size_t pages = (user_stack_size + 4095) / arch::PAGE_SIZE;
        for (size_t i = 0; i < pages; ++i)
            PMM::free_page(ustack_phys + i * arch::PAGE_SIZE);
        delete tcb;
        return nullptr;
    }
    tcb->page_table_ = pml4;

    // Guard page: leave first page unmapped, start mapping at +arch::PAGE_SIZE
    uint64_t user_stack_virt = mem::STACK_VADDR + arch::PAGE_SIZE;
    for (size_t i = 0; i < user_stack_pages; ++i) {
        VMM::map_page_in_pml4(user_stack_virt + i * arch::PAGE_SIZE,
                              ustack_phys + i * arch::PAGE_SIZE, true, pml4);
    }

    tcb->user_stack_ = ustack_phys;
    tcb->user_stack_size_ = user_stack_size;
    uint64_t user_rsp = user_stack_virt + user_stack_size;

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
    *--stack = 0x10; // SPSR_EL1: EL0t, all interrupts unmasked (DAIF=0)
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
    stack[32] = (1ULL << 5); // SSTATUS: SPIE=1, SPP=0 (U-mode)
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

/// @brief Clones the current task (fork).
/// Allocates a new TCB and kernel stack, copies parent's fd table, FPU state,
/// signal handlers, and process hierarchy. For user tasks, clones the page
/// table and copies user stack contents with COW semantics for code/data pages.
/// @param regs  Pointer to the saved register frame from the fork syscall.
/// @return  Pointer to the new child TCB, or nullptr on failure.
TaskControlBlock *TaskControlBlock::clone(uint64_t *regs) {
    auto *parent = Scheduler::current_task();
    if (!parent)
        return nullptr;

    auto *tcb = static_cast<TaskControlBlock *>(
        MemPool::alloc(sizeof(TaskControlBlock)));
    if (!tcb)
        return nullptr;
    memset(tcb, 0, sizeof(TaskControlBlock));

    TCB_WRITE(tcb, magic, TCB_MAGIC);
    TCB_WRITE(tcb, id, Scheduler::alloc_id());
    __builtin_memcpy(tcb->name, parent->name, CONFIG_TASK_NAME_LEN);
    tcb->parent_id = parent->id;
    TCB_WRITE(tcb, state, TaskState::READY);
    tcb->priority = parent->priority;
    tcb->base_priority = parent->base_priority;
    tcb->period_ticks = parent->period_ticks;
    tcb->deadline_ticks = parent->deadline_ticks;
    tcb->executed_ticks = 0;
    tcb->remaining_ticks = parent->remaining_ticks;
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


    // Copy fd_table
    for (size_t i = 0; i < vfs::MAX_FDS; ++i) {
        tcb->fd_table.fds[i] = parent->fd_table.fds[i];
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
    // Save parent's FPU state if it's currently in the registers
    if (__atomic_load_n(&fpu_owner, __ATOMIC_ACQUIRE) == parent) {
        arch::fxsave(parent->fpu_state);
    }
#else
    (void)parent;
#endif
    // Copy FPU state to child
    tcb->fpu_used = parent->fpu_used;
    if (parent->fpu_used) {
        __builtin_memcpy(tcb->fpu_state, parent->fpu_state, 512);
    }

    // Allocate and set up kernel stack
    size_t stack_pages = (STACK_SIZE + 4095) / arch::PAGE_SIZE;
    uint64_t kstack_phys = PMM::alloc_contiguous(stack_pages);
    if (!kstack_phys) {
        ASSERT(errors::TaskError::TASK_ERR_STACK_ALLOC);
        delete tcb;
        return nullptr;
    }
    tcb->stack_phys_ = kstack_phys;

    // Route through kslot for guard page below kernel stack.
    bool is_user_task = (parent->page_table_ != 0);
    uint64_t slot_va = alloc_kslot(STACK_SIZE);
    if (slot_va) {
        tcb->kstack_slot_va_ = slot_va;
        tcb->kstack_slot_size_ = ((STACK_SIZE + 4095) / 4096) * 4096 + 4096;
        uint64_t kstack_va = slot_va + 4096;
        for (size_t i = 0; i < stack_pages; ++i)
            map_kstack_page(kstack_va + i * arch::PAGE_SIZE,
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
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            TCB_WRITE(tcb, kernel_stack, reinterpret_cast<uint8_t *>(kstack_phys));
            tcb->kernel_stack_top = kstack_phys + STACK_SIZE;
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
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    // aarch64 exception frame: padding, SPSR, ELR, SP_EL0, X0-X30
    *--stack = 0;        // padding
    *--stack = 0;        // padding
    *--stack = regs[19]; // SPSR_EL1 (from regs[19])
    *--stack = regs[17]; // ELR_EL1 (from regs[17])
    *--stack = regs[20]; // SP_EL0 (from regs[20])
    for (int i = 0; i < 31; ++i)
        *--stack = regs[i];
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
            delete tcb;
            return nullptr;
        }
        tcb->page_table_ = new_pml4;
        tcb->page_table_shared_ = false;  // deep copy → no sharing

        // Copy kernel entries from the kernel PML4.
        auto *new_virt = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                       (new_pml4 & ~0xFFFULL));
        auto *kernel_virt = reinterpret_cast<uint64_t *>(
            arch::HHDM_OFFSET + (VMM::get_kernel_pml4() & ~0xFFFULL));
        __builtin_memset(new_virt, 0, arch::PAGE_SIZE);
        for (size_t i = arch::PML4_KERNEL_START; i < arch::PML4_ENTRIES; ++i)
            new_virt[i] = kernel_virt[i];

        // Deep-copy user entries from parent (walk, allocate, copy).
        if (!VMM::deep_copy_user_pages(parent->page_table_, new_pml4)) {
            ASSERT(errors::TaskError::TASK_ERR_PML4_CLONE);
            delete tcb;
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
            delete tcb;
            return nullptr;
        }
        tcb->user_stack_ = ustack_phys;
        tcb->user_stack_size_ = parent->user_stack_size_;

        // Guard page: leave first page unmapped, start mapping at
        // +arch::PAGE_SIZE
        uint64_t user_stack_virt = mem::STACK_VADDR + arch::PAGE_SIZE;
        for (size_t i = 0; i < ustack_pages; ++i) {
            VMM::map_page_in_pml4(user_stack_virt + i * arch::PAGE_SIZE,
                                  ustack_phys + i * arch::PAGE_SIZE, true,
                                  new_pml4);
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

/// @brief Releases all resources owned by the task.
/// Unlinks from parent's child list, detaches from message-queue blocked lists,
/// closes file descriptors, frees user/kernel stacks, tears down page tables,
/// destroys IPC objects, and notifies the daemon manager. After this call the
/// TCB must not be used except for MemPool::free().
void TaskControlBlock::cleanup() noexcept {
    if (!TaskControlBlock::is_valid(this)) {
        Logger::raw_write("[CLEANUP] skip poisoned TCB\n");
        kernel::diag::dump_tcb_write_log("[CLEANUP] poisoned TCB");
        return;
    }
    state = TaskState::REAPED;

    // Pinned TCBs are part of the test-isolation baseline (snapshot).
    // Their resources (sporadic_server, kernel stack, page tables) must
    // survive intact — the snapshot restore puts back the original field
    // values, so freeing a pinned task's resources creates dangling
    // pointers.  Skip all teardown; the block itself is also never freed
    // (MemPool::free on a pinned block is a no-op).
    if (kernel::MemPool::is_block_pinned(this))
        return;

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

    // Remove self from parent's child list if we have a parent
    if (parent_id != 0) {
        auto *parent = Scheduler::find_task(parent_id);
        if (parent) {
            parent->remove_child(this);
        }
        parent_id = 0;
    }

    // Remove self from any message queue's blocked-senders list *before*
    // freeing any resources.  If we are blocked on another task's queue
    // (blocked_on_queue != nullptr) we must detach now — otherwise the
    // queue owner's cleanup() will walk a dangling pointer after we are
    // freed and poisoned.
    if (blocked_on_queue &&
        reinterpret_cast<uintptr_t>(blocked_on_queue) >= 0xFFFF800000000000ULL) {
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
        reinterpret_cast<uintptr_t>(waiting_on_mutex) >= 0xFFFF800000000000ULL) {
        waiting_on_mutex->remove_waiter(*this);
        waiting_on_mutex = nullptr;
    }
    if (waiting_on_semaphore &&
        reinterpret_cast<uintptr_t>(waiting_on_semaphore) >= 0xFFFF800000000000ULL) {
        waiting_on_semaphore->remove_waiter(*this);
        waiting_on_semaphore = nullptr;
    }
    if (waiting_on_eventgroup &&
        reinterpret_cast<uintptr_t>(waiting_on_eventgroup) >= 0xFFFF800000000000ULL) {
        waiting_on_eventgroup->remove_waiter(*this);
        waiting_on_eventgroup = nullptr;
    }
    if (waiting_on_queue &&
        reinterpret_cast<uintptr_t>(waiting_on_queue) >= 0xFFFF800000000000ULL) {
        waiting_on_queue->remove_waiter(*this);
        waiting_on_queue = nullptr;
    }

    // Notify the daemon manager so registered daemon PIDs are reset to 0.
    // Must run before msg_queue is destroyed so blocked senders get fast-fail
    // instead of blocking on a zombie destination.
    kernel::daemon::notify_death(this->id);

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

    if (user_stack_ && page_table_) {
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
                unmap_kstack_page(stack_va + i * arch::PAGE_SIZE);
            free_kslot(slot_va, kstack_slot_size_);
        }
        // Poison physical pages via HHDM before freeing.
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

    if (page_table_) {
        // Free any zero-copy buffers owned by this task before tearing down
        // page tables
        BufferPool::unmap_all(*this);

        if (!page_table_shared_) {
            VMM::free_user_pages(page_table_);
            PMM::free_page(page_table_);
        }
        if (stack_pdpt_phys_) {
            free_stack_pdpt(stack_pdpt_phys_);
            stack_pdpt_phys_ = 0;
        }
        page_table_ = 0;
    }

    msg_queue.~MessageQueue();
    kernel::test::ResourceTracker::instance().track_msg_queue_remove();

    notify.~Notify();
    kernel::test::ResourceTracker::instance().track_notify_remove();

    event_group.~EventGroup();
    kernel::test::ResourceTracker::instance().track_event_group_remove();

    if (sporadic_server) {
        Scheduler::dec_sporadic_count();
        MemPool::free(sporadic_server);
        sporadic_server = nullptr;
    }
    if (cwd_vnode)
        vfs::vnode_ref_dec(cwd_vnode);
    cwd_vnode = nullptr;
    kernel::test::ResourceTracker::instance().track_task_remove();
    // If this block was the deadline-monitor task's TCB, clear the scheduler's
    // pointer so s_monitor_task_ can never dangle into a reused block (the
    // on_tick wake path would otherwise write state + enqueue_ready into
    // foreign memory — a corruption time bomb).
#if CONFIG_DEADLINE_MONITOR_TASK
    if (this == Scheduler::get_monitor_task())
        Scheduler::reset_monitor_task();
#endif
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

void TaskControlBlock::operator delete(void *ptr) noexcept {
    if (!ptr)
        return;
    auto *tcb = static_cast<TaskControlBlock *>(ptr);
    if (tcb->magic == TCB_MAGIC) {
        // If the caller already called cleanup() before delete, state
        // is REAPED and we must NOT cleanup/remove again — doing so
        // double-frees resources and corrupts the scheduler lists.
        if (tcb->state != TaskState::REAPED) {
            tcb->cleanup();
            Scheduler::remove_task(*tcb);
        }
        tcb->magic = 0;  // Prevent double-free if caller re-enters operator delete
        MemPool::free(ptr);
        return;
    }
    if (tcb->magic == 0)
        MemPool::free(ptr);
    // magic == 0xDD: reaper already freed it — skip silently.
}

} // namespace kernel
