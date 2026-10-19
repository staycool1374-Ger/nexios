/// @file test_tcb_write_log.cpp
/// @brief Demonstration that the TCB write-log tracer (docs/investigation
///        -cumulative-corruption.md Idea #4) fires when a stray write
///        corrupts a TCB and the poison-detection path runs.

#include <test.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/tcb_write_log.hpp>
#include <logger.hpp>

using namespace kernel;
using namespace kernel::test;

// Intent: prove the ring-buffer write tracker "catches" a stray write by
// dumping the last legitimate modifiers of a corrupted TCB.
//
// A real stray write bypasses the TCB_WRITE macro, so it is NOT recorded
// itself — that is precisely why the post-detection dump is valuable: it
// shows the last *legitimate* writers (callers) of the corrupted TCB,
// narrowing the suspect code path.
//
// Run with CONFIG_TCB_WRITE_LOG and grep the serial log for
// "[TCB-WRITE-LOG]" to confirm the dump was emitted.
JARVIS_TEST(test_tcb_write_log_catches_stray_write, "PRE: none | POST: none") {
    // Dedicated ORPHAN TCB: created via TaskControlBlock::create() but NEVER
    // registered with the scheduler (no register_task/add_task). It sits in
    // no scheduler table or queue, so no timer ISR / scheduler magic check
    // (scheduler.cpp:646, task.cpp:1253) can ever observe the corruption
    // window below. The live harness task's TCB is never touched.
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->magic == TaskControlBlock::TCB_MAGIC);

    // Simulate a STRAY WRITE that corrupts a TCB field, bypassing TCB_WRITE.
    // (A plain field write — exactly what a real stray write looks like to
    // the tracer: NOT recorded. That is the point of the post-hoc dump.)
    // Issue #377: page_table_ privatization made TaskControlBlock
    // non-standard-layout, so offsetof() is -Werror here — derive the
    // offset from live member addresses instead (same bytes, no layout
    // requirement).
    const uint64_t off = static_cast<uint64_t>(
        reinterpret_cast<uint8_t *>(&t->magic) -
        reinterpret_cast<uint8_t *>(t));
    *reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(t) + off) =
        0xDDDDDDDDDDDDDDDDULL;

    // Trigger the poison-detection path. cleanup() sees the invalid magic,
    // prints "[CLEANUP] skip poisoned TCB", calls
    // kernel::diag::dump_tcb_write_log("[CLEANUP] poisoned TCB") and
    // early-returns: no teardown, no tracker calls, state stays READY.
    t->cleanup();

    // Restore magic BEFORE any teardown/free. operator delete silently
    // SKIPS MemPool::free for blocks whose magic is neither TCB_MAGIC nor 0
    // (task.cpp:1498-1500) — deleting while poisoned would leak the block
    // and trip the ResourceTracker mempool check at snapshot_restore.
    t->magic = TaskControlBlock::TCB_MAGIC;

    // Confirm the tracer is wired: dump_tcb_write_log must be callable and,
    // under CONFIG_TCB_WRITE_LOG, shows the legitimate create-time writes
    // of this TCB (the suspects list a real investigation would use).
    kernel::diag::dump_tcb_write_log("[TEST] explicit dump after stray write");

    // Full teardown now that magic is valid: state=REAPED, frees the kernel
    // stack pages and destroys msg_queue/notify/event_group (balancing the
    // ResourceTracker adds from create/init_task_common). With state REAPED,
    // operator delete skips re-cleanup (task.cpp:1490) and only returns the
    // block to MemPool. unregister_task on this never-registered orphan is
    // a safe no-op (AllTasksRegistry::remove early-return, non-member).
    t->cleanup();
    delete t;

    JARVIS_TEST_PASS();
}

void register_tcb_write_log_tests() {
    JARVIS_REGISTER_TEST(test_tcb_write_log_catches_stray_write);
}
