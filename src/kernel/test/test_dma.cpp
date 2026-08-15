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

/// @file test_dma.cpp
/// @brief DMA (Direct Memory Access) tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/driver/dma.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <string.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies DMA buffer allocation returns a valid phys_addr,
// and that the buffer is zero-filled and owned.
// Input: dma::alloc_buffer(512)
// Expect: phys_addr != 0, size >= 512, owned == true
// Depends: PMM, VMM
JARVIS_TEST(dma_alloc_buffer, "PRE: iocd | POST: none") {
    auto buf = dma::alloc_buffer(512);
    JARVIS_ASSERT(buf.phys_addr != 0);
    JARVIS_ASSERT(buf.size >= 512);
    JARVIS_ASSERT(buf.owned);
    JARVIS_ASSERT(buf.virt_addr != 0);
    // Verify zero-filled
    auto *ptr = reinterpret_cast<volatile uint8_t *>(buf.virt_addr);
    for (size_t i = 0; i < 64; ++i) {
        JARVIS_ASSERT_EQ(0, ptr[i]);
    }
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies DMA buffer can be written to and read from.
// Input: Allocate buffer, write pattern, read back.
// Expect: Data written matches data read.
// Depends: PMM, VMM
JARVIS_TEST(dma_buffer_write_read, "PRE: iocd | POST: none") {
    auto buf = dma::alloc_buffer(4096);
    JARVIS_ASSERT(buf.phys_addr != 0);
    auto *ptr = reinterpret_cast<uint8_t *>(buf.virt_addr);
    const char *test_str = "DMA buffer test pattern";
    memcpy(ptr, test_str, 22);
    JARVIS_ASSERT_EQ(0, memcmp(ptr, test_str, 22));
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies sg_from_buffer creates a single-entry SG list with
// correct phys_addr, virt_addr, and length.
// Input: DMA buffer + sg_from_buffer()
// Expect: SG list count == 1, entries match buffer
// Depends: dma::alloc_buffer
JARVIS_TEST(dma_sg_from_buffer, "PRE: iocd | POST: none") {
    auto buf = dma::alloc_buffer(2048);
    JARVIS_ASSERT(buf.phys_addr != 0);
    dma::SgList sg;
    dma::sg_reset(sg);
    bool ok = dma::sg_from_buffer(sg, buf);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ((size_t)1, sg.count);
    JARVIS_ASSERT_EQ(buf.phys_addr, sg.entries[0].phys_addr);
    JARVIS_ASSERT_EQ(buf.virt_addr, sg.entries[0].virt_addr);
    JARVIS_ASSERT_EQ(buf.size, sg.entries[0].length);
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies PRD table construction from SG list: byte_count and
// EOT flag are set correctly.
// Input: sg_from_buffer + prd_from_sg
// Expect: PRD count == 1, byte_count == size-1, EOT set
// Depends: dma::alloc_buffer
JARVIS_TEST(dma_prd_from_sg, "PRE: iocd | POST: none") {
    auto buf = dma::alloc_buffer(4096);
    JARVIS_ASSERT(buf.phys_addr != 0);
    dma::SgList sg;
    dma::sg_reset(sg);
    dma::sg_from_buffer(sg, buf);
    dma::PrdTable prd;
    size_t n = dma::prd_from_sg(prd, sg, true);
    JARVIS_ASSERT_EQ((size_t)1, n);
    JARVIS_ASSERT_EQ(buf.phys_addr, prd.entries[0].phys_addr);
    JARVIS_ASSERT_EQ((uint16_t)(buf.size - 1), prd.entries[0].byte_count);
    JARVIS_ASSERT((prd.entries[0].flags & 0x8000) != 0); // EOT set
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies pci_set_bus_master on the host bridge (harmless no-op).
// Input: arch::PciBdf for 00:00.0, enable then disable
// Expect: No crash, returns normally
// Depends: arch::pci_config_readw, arch::pci_config_writel
JARVIS_TEST(dma_bus_master_host_bridge, "PRE: iocd | POST: none") {
    arch::PciBdf bdf = {0, 0, 0};
    // Just verify the code doesn't crash
    dma::pci_set_bus_master(bdf, true);
    dma::pci_set_bus_master(bdf, false);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies sg_reset clears the SG list.
// Input: dma::sg_reset(sg)
// Expect: count == 0, total_length == 0
// Depends: none
JARVIS_TEST(dma_sg_reset, "PRE: iocd | POST: none") {
    dma::SgList sg;
    sg.count = 5;
    sg.total_length = 1234;
    dma::sg_reset(sg);
    JARVIS_ASSERT_EQ((size_t)0, sg.count);
    JARVIS_ASSERT_EQ((size_t)0, sg.total_length);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies prd_reset clears the PRD table.
// Input: dma::prd_reset(prd)
// Expect: count == 0
// Depends: none
JARVIS_TEST(dma_prd_reset, "PRE: iocd | POST: none") {
    dma::PrdTable prd;
    prd.count = 10;
    dma::prd_reset(prd);
    JARVIS_ASSERT_EQ((size_t)0, prd.count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies free_buffer with a zero-initialized buffer is a no-op.
// Input: dma::free_buffer({})
// Expect: No crash
// Depends: none
JARVIS_TEST(dma_free_empty_buffer, "PRE: iocd | POST: none") {
    dma::DmaBuffer buf = {};
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies zero-length buffer allocation returns gracefully.
// Input: dma::alloc_buffer(0)
// Expect: Returns DmaBuffer with phys_addr == 0 (failure, not a crash)
// Depends: dma::alloc_buffer
JARVIS_TEST(dma_zero_length_buffer, "PRE: iocd | POST: none") {
    auto buf = dma::alloc_buffer(0);
    JARVIS_ASSERT_EQ((uint64_t)0, buf.phys_addr);
    JARVIS_ASSERT(!buf.owned);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies PRD table construction from non-contiguous physical pages
// using two separate DMA buffers. Manually builds a 2-entry SG list from
// buffers that are not physically contiguous, then calls prd_from_sg.
// Input: Two dma::alloc_buffer calls, manual SG list setup, prd_from_sg
// Expect: PRD table has 2 entries with different phys_addr ranges
// Depends: dma::alloc_buffer, dma::sg_reset, dma::prd_from_sg
JARVIS_TEST(dma_sg_non_contiguous_prd, "PRE: iocd | POST: none") {
    auto buf1 = dma::alloc_buffer(4096);
    JARVIS_ASSERT(buf1.phys_addr != 0);
    auto buf2 = dma::alloc_buffer(4096);
    JARVIS_ASSERT(buf2.phys_addr != 0);
    // Buffers should not be contiguous (different allocations)
    JARVIS_ASSERT(buf1.phys_addr != buf2.phys_addr);
    dma::SgList sg;
    dma::sg_reset(sg);
    sg.entries[0].phys_addr = buf1.phys_addr;
    sg.entries[0].virt_addr = buf1.virt_addr;
    sg.entries[0].length = buf1.size;
    sg.entries[1].phys_addr = buf2.phys_addr;
    sg.entries[1].virt_addr = buf2.virt_addr;
    sg.entries[1].length = buf2.size;
    sg.count = 2;
    sg.total_length = buf1.size + buf2.size;
    dma::PrdTable prd;
    size_t n = dma::prd_from_sg(prd, sg, true);
    JARVIS_ASSERT_EQ((size_t)2, n);
    JARVIS_ASSERT(prd.entries[0].phys_addr != prd.entries[1].phys_addr);
    JARVIS_ASSERT((prd.entries[1].flags & 0x8000) != 0);
    dma::free_buffer(buf1);
    dma::free_buffer(buf2);
    JARVIS_TEST_PASS();
}

static volatile uint64_t g_dma_cb_ctx = 0;
static volatile bool g_dma_cb_fired = false;
static volatile bool g_pp_chain_fired = false;

static void dma_completion_cb(uint64_t ctx, bool success) {
    g_dma_cb_ctx = ctx;
    g_dma_cb_fired = true;
    (void)success;
}

static void pp_chain_cb(uint64_t ctx, dma::DmaBuffer *buf, bool success) {
    g_pp_chain_fired = true;
    // Fill the completed buffer with known pattern to simulate
    // the driver preparing it for the next cycle
    if (buf && buf->virt_addr) {
        __builtin_memset(reinterpret_cast<void *>(buf->virt_addr), 0xCC,
                         buf->size > 64 ? 64 : buf->size);
    }
    (void)ctx;
    (void)success;
}

// Runmode: kernel
// Testidea: Verifies DmaEngine + BmDmaChannel API: init, start_transfer,
// is_busy, handle_irq, callback invocation, and abort.
// Input: Create BmDmaChannel with mock I/O base, alloc buffer, build SG/PRD.
// Expect: start_transfer succeeds, is_busy reflects state, handle_irq
// acknowledges and invokes callback, abort stops the engine.
// Depends: dma::DmaEngine, dma::BmDmaChannel, dma::alloc_buffer, prd_from_sg
JARVIS_TEST(dma_completion_interrupt, "PRE: iocd | POST: none") {
    g_dma_cb_fired = false;
    g_dma_cb_ctx = 0;

    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);

    auto buf = dma::alloc_buffer(512);
    JARVIS_ASSERT(buf.phys_addr != 0);

    dma::SgList sg;
    dma::sg_reset(sg);
    JARVIS_ASSERT(dma::sg_from_buffer(sg, buf));

    dma::PrdTable prd;
    size_t n = dma::prd_from_sg(prd, sg, true);
    JARVIS_ASSERT_EQ((size_t)1, n);

    // Start transfer
    bool started = engine.start_transfer(prd, dma::Direction::READ,
                                         dma_completion_cb, 0x42);
    JARVIS_ASSERT(started);
    JARVIS_ASSERT(engine.is_busy());

    // Simulate completion via handle_irq
    bool irq_handled = engine.handle_irq();
    JARVIS_ASSERT(irq_handled);

    // Callback should have fired
    JARVIS_ASSERT(g_dma_cb_fired);
    JARVIS_ASSERT_EQ((uint64_t)0x42, g_dma_cb_ctx);
    JARVIS_ASSERT(!engine.is_busy());

    // Test abort
    started = engine.start_transfer(prd, dma::Direction::WRITE, nullptr, 0);
    JARVIS_ASSERT(started);
    engine.abort();
    JARVIS_ASSERT(!engine.is_busy());

    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Double-buffered DMA (ping-pong) with automatic chaining.
// Input: PingPongDma with two buffers, fill with alternating patterns,
// chain transfers via start_next, simulate completions via handle_irq.
// Expect: Back-to-back transfers complete without data corruption,
// buffer chaining swaps indices correctly.
// Depends: dma::PingPongDma, dma::DmaEngine, dma::BmDmaChannel
JARVIS_TEST(dma_double_buffered_transfer, "PRE: iocd | POST: none") {
    g_pp_chain_fired = false;

    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);

    dma::PingPongDma pp(engine, 512);
    JARVIS_ASSERT(pp.init());

    // Both buffers are zero-filled from alloc; fill with patterns
    dma::DmaBuffer *buf_a = pp.prepare_buf();
    JARVIS_ASSERT(buf_a != nullptr);
    JARVIS_ASSERT(buf_a->phys_addr != 0);
    __builtin_memset(reinterpret_cast<void *>(buf_a->virt_addr), 0xAA, 64);

    // Start first transfer (buffer A goes to DMA)
    bool started = pp.start_next(dma::Direction::READ, pp_chain_cb, 0x77);
    JARVIS_ASSERT(started);
    JARVIS_ASSERT(pp.busy());

    // Buffer B should now be the prepare buffer
    dma::DmaBuffer *buf_b = pp.prepare_buf();
    JARVIS_ASSERT(buf_b != nullptr);
    JARVIS_ASSERT(buf_b->phys_addr != buf_a->phys_addr);
    __builtin_memset(reinterpret_cast<void *>(buf_b->virt_addr), 0xBB, 64);

    // Verify buffer A data remains intact while B is being prepared
    uint8_t *a_data = reinterpret_cast<uint8_t *>(buf_a->virt_addr);
    for (int i = 0; i < 64; ++i) {
        JARVIS_ASSERT_EQ((uint8_t)0xAA, a_data[i]);
    }

    // Simulate completion of transfer A
    bool irq_handled = engine.handle_irq();
    JARVIS_ASSERT(irq_handled);
    JARVIS_ASSERT(!pp.busy());
    JARVIS_ASSERT_EQ((uint64_t)1, pp.completed_count());

    // Chain to buffer B transfer
    started = pp.start_next(dma::Direction::READ, pp_chain_cb, 0x88);
    JARVIS_ASSERT(started);
    JARVIS_ASSERT(pp.busy());

    // Buffer A is now the prepare buffer again — the chain callback on
    // completion writes 0xCC (simulating driver preparing for next cycle),
    // so verify the callback pattern, not the original fill.
    dma::DmaBuffer *buf_a_again = pp.prepare_buf();
    JARVIS_ASSERT(buf_a_again == buf_a);
    uint8_t *a_data2 = reinterpret_cast<uint8_t *>(buf_a_again->virt_addr);
    bool data_ok = true;
    for (int i = 0; i < 64; ++i) {
        if (a_data2[i] != (uint8_t)0xCC) {
            data_ok = false;
            break;
        }
    }

    // Simulate completion of transfer B
    irq_handled = engine.handle_irq();
    JARVIS_ASSERT(irq_handled);
    JARVIS_ASSERT_EQ((uint64_t)2, pp.completed_count());

    // Chain callback should have fired at least once
    JARVIS_ASSERT(g_pp_chain_fired);

    pp.shutdown();

    JARVIS_ASSERT(data_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-01 — a busy DmaEngine rejects a second start_transfer.
// Input: start once (OK), start again
// Expect: first true, second false; abort clears; restart OK
// Depends: dma::DmaEngine, dma::BmDmaChannel
JARVIS_TEST(dma_engine_reject_when_busy, "PRE: iocd | POST: none") {
    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);

    auto buf = dma::alloc_buffer(512);
    JARVIS_ASSERT(buf.phys_addr != 0);
    dma::SgList sg;
    dma::sg_reset(sg);
    JARVIS_ASSERT(dma::sg_from_buffer(sg, buf));
    dma::PrdTable prd;
    JARVIS_ASSERT_EQ((size_t)1, dma::prd_from_sg(prd, sg, true));

    JARVIS_ASSERT(engine.start_transfer(prd, dma::Direction::READ, nullptr, 0));
    JARVIS_ASSERT(
        !engine.start_transfer(prd, dma::Direction::READ, nullptr, 0));
    engine.abort();
    JARVIS_ASSERT(engine.start_transfer(prd, dma::Direction::READ, nullptr, 0));

    engine.abort();
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-01 — handle_irq fires the callback exactly once; a second
//           handle_irq (no active transfer) is a no-op.
// Input: start with cb; handle_irq; handle_irq again
// Expect: count == 1; second handle_irq returns false
// Depends: dma::DmaEngine, dma::BmDmaChannel
JARVIS_TEST(dma_engine_no_double_callback, "PRE: iocd | POST: none") {
    static uint64_t g_cb_count = 0;
    g_cb_count = 0;

    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);

    auto buf = dma::alloc_buffer(512);
    JARVIS_ASSERT(buf.phys_addr != 0);
    dma::SgList sg;
    dma::sg_reset(sg);
    JARVIS_ASSERT(dma::sg_from_buffer(sg, buf));
    dma::PrdTable prd;
    JARVIS_ASSERT_EQ((size_t)1, dma::prd_from_sg(prd, sg, true));

    struct Local {
        static void fired(uint64_t, bool) {
            ++g_cb_count;
        }
    };
    JARVIS_ASSERT(
        engine.start_transfer(prd, dma::Direction::READ, Local::fired, 0x99));
    JARVIS_ASSERT(engine.handle_irq());
    JARVIS_ASSERT_EQ((uint64_t)1, g_cb_count);
    JARVIS_ASSERT(!engine.handle_irq()); // no active transfer -> no-op
    JARVIS_ASSERT_EQ((uint64_t)1, g_cb_count);

    engine.abort();
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-01 — abort() before completion suppresses the callback.
// Input: start with cb; abort; handle_irq
// Expect: callback never fires; handle_irq returns false
// Depends: dma::DmaEngine, dma::BmDmaChannel
JARVIS_TEST(dma_engine_abort_suppresses_callback, "PRE: iocd | POST: none") {
    g_dma_cb_fired = false;
    g_dma_cb_ctx = 0;

    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);

    auto buf = dma::alloc_buffer(512);
    JARVIS_ASSERT(buf.phys_addr != 0);
    dma::SgList sg;
    dma::sg_reset(sg);
    JARVIS_ASSERT(dma::sg_from_buffer(sg, buf));
    dma::PrdTable prd;
    JARVIS_ASSERT_EQ((size_t)1, dma::prd_from_sg(prd, sg, true));

    JARVIS_ASSERT(engine.start_transfer(prd, dma::Direction::READ,
                                        dma_completion_cb, 0x42));
    engine.abort();
    JARVIS_ASSERT(!engine.handle_irq());
    JARVIS_ASSERT(!g_dma_cb_fired);
    JARVIS_ASSERT(!engine.is_busy());

    engine.abort();
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-01 — start_transfer inside an IrqGuard (IF=0) then
//           handle_irq outside restores IRQ state and fires the callback.
// Input: start under arch::IrqGuard; exit; handle_irq
// Expect: start OK; callback fires once; IRQ state restored (enabled)
// Depends: dma::DmaEngine, dma::BmDmaChannel, arch::IrqGuard
JARVIS_TEST(dma_engine_irqguard_roundtrip, "PRE: iocd | POST: none") {
    g_dma_cb_fired = false;
    g_dma_cb_ctx = 0;

    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);

    auto buf = dma::alloc_buffer(512);
    JARVIS_ASSERT(buf.phys_addr != 0);
    dma::SgList sg;
    dma::sg_reset(sg);
    JARVIS_ASSERT(dma::sg_from_buffer(sg, buf));
    dma::PrdTable prd;
    JARVIS_ASSERT_EQ((size_t)1, dma::prd_from_sg(prd, sg, true));

    {
        arch::IrqGuard outer;
        JARVIS_ASSERT(engine.start_transfer(prd, dma::Direction::READ,
                                            dma_completion_cb, 0x77));
        JARVIS_ASSERT(!arch::interrupts_enabled());
    }
    JARVIS_ASSERT(arch::interrupts_enabled()); // IrqGuard restored IF

    JARVIS_ASSERT(engine.handle_irq());
    JARVIS_ASSERT(g_dma_cb_fired);
    JARVIS_ASSERT_EQ((uint64_t)0x77, g_dma_cb_ctx);

    engine.abort();
    dma::free_buffer(buf);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-02 — while a transfer is active, xfer_buf() is the DMA
//           target and prepare_buf() is the other buffer; after completion
//           the indices swap.
// Input: init pingpong, start_next, inspect buffers, complete, inspect again
// Expect: while busy xfer != prepare; after completion they swap
// Depends: dma::PingPongDma, dma::DmaEngine, dma::BmDmaChannel
JARVIS_TEST(pingpong_xfer_buf_while_busy, "PRE: iocd | POST: none") {
    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);
    dma::PingPongDma pp(engine, 512);
    JARVIS_ASSERT(pp.init());

    dma::DmaBuffer *buf_a = pp.prepare_buf();
    JARVIS_ASSERT(buf_a != nullptr);
    JARVIS_ASSERT(pp.start_next(dma::Direction::READ, nullptr, 0));
    JARVIS_ASSERT(pp.busy());

    dma::DmaBuffer *xfer = pp.xfer_buf();
    dma::DmaBuffer *prep = pp.prepare_buf();
    JARVIS_ASSERT(xfer == buf_a); // buf_a is now the DMA target
    JARVIS_ASSERT(prep != buf_a); // other buffer is being prepared

    JARVIS_ASSERT(engine.handle_irq()); // complete transfer A
    JARVIS_ASSERT(!pp.busy());
    dma::DmaBuffer *xfer2 = pp.xfer_buf();
    JARVIS_ASSERT(xfer2 != buf_a); // indices swapped after completion

    pp.shutdown();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-02 — a busy PingPongDma rejects a second start_next.
// Input: start_next OK; start_next again while busy
// Expect: second returns false
// Depends: dma::PingPongDma, dma::DmaEngine, dma::BmDmaChannel
JARVIS_TEST(pingpong_reject_when_busy, "PRE: iocd | POST: none") {
    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);
    dma::PingPongDma pp(engine, 512);
    JARVIS_ASSERT(pp.init());

    JARVIS_ASSERT(pp.start_next(dma::Direction::READ, nullptr, 0));
    JARVIS_ASSERT(pp.busy());
    JARVIS_ASSERT(!pp.start_next(dma::Direction::READ, nullptr, 0));

    JARVIS_ASSERT(engine.handle_irq());
    JARVIS_ASSERT(!pp.busy());
    pp.shutdown();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-02 — shutdown clears state and frees both buffers.
// Input: init, start, shutdown
// Expect: !busy, completed_count == 0, prepare_buf returns bufs_[0]
// Depends: dma::PingPongDma, dma::DmaEngine, dma::BmDmaChannel
JARVIS_TEST(pingpong_shutdown_clears_state, "PRE: iocd | POST: none") {
    dma::BmDmaChannel channel(0xFF00);
    JARVIS_ASSERT(channel.init());
    dma::DmaEngine engine(channel);
    dma::PingPongDma pp(engine, 512);
    JARVIS_ASSERT(pp.init());

    JARVIS_ASSERT(pp.start_next(dma::Direction::READ, nullptr, 0));
    pp.shutdown();
    JARVIS_ASSERT(!pp.busy());
    JARVIS_ASSERT_EQ((uint64_t)0, pp.completed_count());
    dma::DmaBuffer *prep = pp.prepare_buf();
    JARVIS_ASSERT(prep != nullptr);
    JARVIS_TEST_PASS();
}

void register_dma_tests() {
    Logger::info("Registering DMA tests");
    JARVIS_REGISTER_TEST(dma_alloc_buffer);
    JARVIS_REGISTER_TEST(dma_buffer_write_read);
    JARVIS_REGISTER_TEST(dma_sg_from_buffer);
    JARVIS_REGISTER_TEST(dma_prd_from_sg);
    JARVIS_REGISTER_TEST(dma_bus_master_host_bridge);
    JARVIS_REGISTER_TEST(dma_sg_reset);
    JARVIS_REGISTER_TEST(dma_prd_reset);
    JARVIS_REGISTER_TEST(dma_free_empty_buffer);
    JARVIS_REGISTER_TEST(dma_zero_length_buffer);
    JARVIS_REGISTER_TEST(dma_sg_non_contiguous_prd);
    JARVIS_REGISTER_TEST(dma_completion_interrupt);
    JARVIS_REGISTER_TEST(dma_double_buffered_transfer);
    JARVIS_REGISTER_TEST(dma_engine_reject_when_busy);
    JARVIS_REGISTER_TEST(dma_engine_no_double_callback);
    JARVIS_REGISTER_TEST(dma_engine_abort_suppresses_callback);
    JARVIS_REGISTER_TEST(dma_engine_irqguard_roundtrip);
    JARVIS_REGISTER_TEST(pingpong_xfer_buf_while_busy);
    JARVIS_REGISTER_TEST(pingpong_reject_when_busy);
    JARVIS_REGISTER_TEST(pingpong_shutdown_clears_state);
}
