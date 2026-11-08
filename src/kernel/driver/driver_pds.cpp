#include <kernel/driver/driver_pds.hpp>

#if CONFIG_DRIVER_PD_SERIAL || CONFIG_DRIVER_PD_KBD ||                        \
    CONFIG_DRIVER_PD_RTC || CONFIG_DRIVER_PD_FB ||                             \
    CONFIG_DRIVER_PD_BLOCK || CONFIG_DRIVER_PD_NET ||                          \
    CONFIG_SERIAL_TX_SHM

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/boot_sdf.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/memory/address.hpp>
#include <kernel/ipc/shm_ring.hpp>
#include <services/terminal/framebuffer.hpp>

namespace kernel {
namespace driver_pds {
namespace {

uint64_t g_role_pids[6] = {};
uint32_t g_epoch = 1;

struct Tombstone {
    uint32_t owner_id;
    uint32_t owner_gen;
};

Tombstone g_tombstones[16] = {};
uint64_t g_tombstone_count = 0;

uint64_t role_index(sdf::PdRole role) {
    return static_cast<uint64_t>(role);
}

} // namespace

void set_pd_pid(sdf::PdRole role, uint64_t pid) {
    const uint64_t idx = role_index(role);
    if (idx < 6) {
        __atomic_store_n(&g_role_pids[idx], pid, __ATOMIC_RELAXED);
    }
}

uint64_t get_pd_pid(sdf::PdRole role) {
    const uint64_t idx = role_index(role);
    if (idx >= 6) {
        return 0;
    }
    return __atomic_load_n(&g_role_pids[idx], __ATOMIC_RELAXED);
}

bool pd_alive(sdf::PdRole role) {
    const uint64_t pid = get_pd_pid(role);
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

uint32_t pd_epoch() {
    return __atomic_load_n(&g_epoch, __ATOMIC_RELAXED);
}

void pd_bump_epoch() {
    __atomic_fetch_add(&g_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void pd_on_restart(sdf::PdRole role, uint64_t pid) {
    set_pd_pid(role, pid);
    pd_bump_epoch();
}

void pd_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
    uint64_t count =
        __atomic_load_n(&g_tombstone_count, __ATOMIC_RELAXED);
    if (count >= 16) {
        return; // bounded residual drop (documented, ≤ table size)
    }
    g_tombstones[count].owner_id = owner_id;
    g_tombstones[count].owner_gen = owner_gen;
    __atomic_store_n(&g_tombstone_count, count + 1, __ATOMIC_RELAXED);
}

bool pd_rpc(sdf::PdRole role, const DriverRpc &req, DriverRpc &reply) {
    const uint64_t pid = get_pd_pid(role);
    if (pid == 0 || !pd_alive(role)) {
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
    if (raw.sender_id != pid || reply.epoch != pd_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

bool pd_bind(sdf::PdRole role) {
    const uint64_t pid = get_pd_pid(role);
    if (pid == 0 || !pd_alive(role)) {
        return false;
    }
    auto *srv = Scheduler::find_task(pid);
    if (srv == nullptr) {
        return false;
    }
    auto *space = srv->get_cspace();
    if (space == nullptr) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    // Convey the PD's composed caps: re-derive the row (pure function of
    // role+arch) for phys/size, pair with the persisted (slot, gen) for
    // the encoded handle. BIND conveys row index 0 (the UART cap:
    // arg0 = handle, arg1 = phys, arg2 = size, flags = row cap count);
    // the SHM ring handle (index 1) arrives via pd_bind_shm — never
    // silent partial conveyance (a half-bound PD would misbehave).
    sdf::CapSpec specs[sdf::kMaxCapsPerRole] = {};
    const uint64_t count = sdf::sdf_row(role, specs);
    if (count < 1) {
        return false;
    }
    int slot = -1;
    uint32_t cap_gen = 0;
    if (!sdf::sdf_role_cap(role, 0, slot, cap_gen)) {
        return false;
    }
    DriverRpc req{};
    req.op = static_cast<uint8_t>(DrvOp::BIND);
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = pd_epoch();
    req.flags = static_cast<uint8_t>(count);
    req.arg0 = cap::encode_user_handle(space->cspace_id, slot, cap_gen);
    req.arg1 = specs[0].addr;
    req.arg2 = specs[0].size;
    DriverRpc reply{};
    return pd_rpc(role, req, reply);
}

bool pd_bind_shm(sdf::PdRole role) {
    const uint64_t pid = get_pd_pid(role);
    if (pid == 0 || !pd_alive(role)) {
        return false;
    }
    auto *srv = Scheduler::find_task(pid);
    if (srv == nullptr) {
        return false;
    }
    auto *space = srv->get_cspace();
    if (space == nullptr) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    // The ring handle lives at row index 1 (index 0 is the UART cap).
    // Both the row shape and the composed record must agree — a knob
    // skew (row without SHM, or compose without SHM) fail-closes here,
    // never conveys a wrong cap.
    sdf::CapSpec specs[sdf::kMaxCapsPerRole] = {};
    const uint64_t count = sdf::sdf_row(role, specs);
    if (count < 2 || specs[1].kind != sdf::CapKind::SHM) {
        return false;
    }
    uint64_t pages = 0;
    uint64_t phys = 0;
    if (!sdf::sdf_shm_tx_phys(role, phys, pages) || phys == 0 ||
        pages == 0) {
        return false;
    }
    int slot = -1;
    uint32_t cap_gen = 0;
    if (!sdf::sdf_role_cap(role, 1, slot, cap_gen)) {
        return false;
    }
    DriverRpc req{};
    req.op = static_cast<uint8_t>(DrvOp::MAP_SHM);
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = pd_epoch();
    req.arg0 = cap::encode_user_handle(space->cspace_id, slot, cap_gen);
    req.arg1 = pages;
    req.arg2 = sdf::kSerialTxRingElemSize;
    DriverRpc reply{};
    if (!pd_rpc(role, req, reply)) {
        return false;
    }
    return reply.status == 0 && reply.arg0 == 1;
}

#if CONFIG_SERIAL_TX_SHM
namespace {

// TX-ring producer record (issue #51 step-c-prime): the SDF-composed
// ring backing store, re-resolved whenever the proxy epoch moves (fresh
// frames every rebind — never a stale ring). Written on the TX path
// only; the PD side owns head/tail progress via the shared header.
uint64_t g_tx_phys = 0;
uint64_t g_tx_pages = 0;
uint32_t g_tx_epoch = 0;
uint64_t g_tx_dropped = 0;

bool serial_tx_resolve() {
    if (g_tx_phys != 0 && g_tx_epoch == pd_epoch()) {
        return true;
    }
    uint64_t phys = 0;
    uint64_t pages = 0;
    if (!sdf::sdf_shm_tx_phys(sdf::PdRole::SERIAL, phys, pages) ||
        phys == 0 || pages == 0) {
        return false;
    }
    g_tx_phys = phys;
    g_tx_pages = pages;
    g_tx_epoch = pd_epoch();
    return true;
}

} // namespace

bool serial_tx_write(const char *data, uint64_t len) {
    if (!sdf::pd_owns_device(sdf::PdRole::SERIAL)) {
        return false;
    }
    if (len == 0) {
        return serial_tx_resolve();
    }
    if (data == nullptr) {
        return false;
    }
    if (!serial_tx_resolve()) {
        return false;
    }
    auto *hdr = reinterpret_cast<shm::SharedRingHeader *>(kernel::HHDM_OFFSET +
                                                           g_tx_phys);
    if (!shm::shm_ring_valid(hdr)) {
        return false;
    }
    uint8_t *data_base = reinterpret_cast<uint8_t *>(kernel::HHDM_OFFSET +
                                                     g_tx_phys +
                                                     arch::PAGE_SIZE);
    const bool was_empty = !shm::shm_ring_has_data(hdr);
    uint64_t off = 0;
    while (off < len) {
        uint8_t elem[sdf::kSerialTxRingElemSize] = {};
        uint64_t chunk = len - off;
        if (chunk > sdf::kSerialTxRingElemSize - 1) {
            chunk = sdf::kSerialTxRingElemSize - 1;
        }
        elem[0] = static_cast<uint8_t>(chunk);
        for (uint64_t i = 0; i < chunk; ++i) {
            elem[1 + i] = static_cast<uint8_t>(data[off + i]);
        }
        if (!shm::shm_ring_push(hdr, data_base, elem)) {
            __atomic_fetch_add(&g_tx_dropped, len - off, __ATOMIC_RELAXED);
            break;
        }
        off += chunk;
    }
    // Empty→nonempty NOTIFY is a latency optimization, not the liveness
    // mechanism: the PD polls with a 1-tick recv timeout, so a lost or
    // reordered notify costs one tick, never a stall. Skip the wakeup
    // when the ring was already draining (no thundering herd per char).
    if (off > 0 && was_empty) {
        const uint64_t tx_pid = get_pd_pid(sdf::PdRole::SERIAL);
        if (tx_pid != 0) {
            auto *srv = Scheduler::find_task(tx_pid);
            if (srv != nullptr) {
                srv->notify.notify(kSerialTxNotify);
            }
        }
    }
    // Live channel takes over the byte stream unconditionally: drops are
    // counted backpressure, while a raw-path fallback would duplicate
    // bytes and reintroduce the 51c garble.
    return true;
}

uint64_t serial_tx_dropped() {
    return __atomic_load_n(&g_tx_dropped, __ATOMIC_RELAXED);
}

bool serial_tx_test_hold(bool hold) {
    if (!serial_tx_resolve()) {
        return false;
    }
    auto *hdr = reinterpret_cast<shm::SharedRingHeader *>(kernel::HHDM_OFFSET +
                                                           g_tx_phys);
    if (!shm::shm_ring_valid(hdr)) {
        return false;
    }
    __atomic_store_n(&hdr->drain_hold, hold ? 1ULL : 0ULL, __ATOMIC_RELEASE);
    return true;
}

uint64_t serial_tx_test_phys() {
    if (!serial_tx_resolve()) {
        return 0;
    }
    auto *hdr = reinterpret_cast<shm::SharedRingHeader *>(kernel::HHDM_OFFSET +
                                                           g_tx_phys);
    if (!shm::shm_ring_valid(hdr)) {
        return 0;
    }
    return g_tx_phys;
}
#endif // CONFIG_SERIAL_TX_SHM

#if CONFIG_DRIVER_PD_FB
namespace {

// Console channel record (issue #51): the composed ring/status backing
// store re-resolved whenever the proxy epoch moves (fresh frames every
// rebind — never a stale ring or a stale counter page).
uint64_t g_con_ring_phys = 0;
uint64_t g_con_ring_pages = 0;
uint64_t g_con_status_phys = 0;
uint64_t g_con_status_pages = 0;
uint32_t g_con_epoch = 0;
bool g_con_bound = false;
uint64_t g_con_dropped = 0;

bool console_resolve() {
    if (g_con_bound && g_con_epoch == pd_epoch()) {
        return true;
    }
    g_con_bound = false;
    g_con_ring_phys = 0;
    g_con_status_phys = 0;
    uint64_t rphys = 0;
    uint64_t rpages = 0;
    if (!sdf::sdf_shm_tx_phys(sdf::PdRole::FB, rphys, rpages) ||
        rphys == 0 || rpages == 0) {
        return false;
    }
    g_con_ring_phys = rphys;
    g_con_ring_pages = rpages;
    auto *hdr = reinterpret_cast<shm::SharedRingHeader *>(kernel::HHDM_OFFSET +
                                                           rphys);
    if (!shm::shm_ring_valid(hdr)) {
        g_con_ring_phys = 0;
        return false;
    }
    g_con_epoch = pd_epoch();
    g_con_bound = true;
    return true;
}

} // namespace

bool pd_bind_console() {
    if (!sdf::pd_owns_device(sdf::PdRole::FB)) {
        return false;
    }
    const uint64_t pid = get_pd_pid(sdf::PdRole::FB);
    if (pid == 0 || !pd_alive(sdf::PdRole::FB)) {
        return false;
    }
    auto *srv = Scheduler::find_task(pid);
    if (srv == nullptr) {
        return false;
    }
    auto *space = srv->get_cspace();
    if (space == nullptr) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    sdf::CapSpec specs[sdf::kMaxCapsPerRole] = {};
    const uint64_t count = sdf::sdf_row(sdf::PdRole::FB, specs);
    // Fail-closed before ANY side effect: a console PD without a scanout
    // is a blind sink — never bind it (the kernel keeps its own path).
    // Scanout chunks are MMIO (true device memory) or FRAME (RAM-backed
    // scanout — SYS_MMIO_MAP refuses RAM, SYS_FRAME_MAP refuses kernel
    // pages, so the row kind must match the backend backing).
    if (count < 3 || specs[0].kind != sdf::CapKind::SHM ||
        specs[1].kind != sdf::CapKind::DMA_REGION) {
        return false; /* knob skew, degraded frame, or failed compose */
    }
    const bool ram = service::Framebuffer::scanout_ram_backed();
    const sdf::CapKind chunk_kind =
        ram ? sdf::CapKind::FRAME : sdf::CapKind::MMIO;
    if (!service::Framebuffer::available() ||
        service::Framebuffer::scanout_phys() == 0) {
        return false;
    }
    const uint32_t epoch = pd_epoch();
    {
        // BIND first: one RPC per scanout chunk (flags = chunk index,
        // handles/index-band unchanged on rebind = idempotent in the PD;
        // a changed handle fails closed, never a double grant).
        const uint64_t phys0 = service::Framebuffer::scanout_phys();
        const uint64_t chunk =
            ram ? sdf::kConsoleFrameChunk : sdf::kConsoleMmioChunk;
        for (uint64_t i = 2; i < count; ++i) {
            const bool shape_ok =
                specs[i].kind == chunk_kind &&
                specs[i].addr == phys0 + (i - 2) * chunk &&
                specs[i].size != 0 && specs[i].size <= chunk;
            if (!shape_ok) {
                return false; /* row shape skew: never bind a guess */
            }
        }
        for (uint64_t i = 2; i < count; ++i) {
            int slot = -1;
            uint32_t gen = 0;
            if (!sdf::sdf_role_cap(sdf::PdRole::FB, i, slot, gen)) {
                Logger::warn("console bind: role_cap(%lu) failed",
                             (unsigned long)i);
                return false;
            }
            DriverRpc req{};
            req.op = static_cast<uint8_t>(ram ? DrvOp::BIND_FRAME
                                              : DrvOp::BIND);
            req.flags = static_cast<uint8_t>(i - 2); /* chunk index */
            req.owner_id = static_cast<uint32_t>(cur->id);
            req.owner_gen = cur->generation;
            req.epoch = epoch;
            req.arg0 = cap::encode_user_handle(space->cspace_id, slot, gen);
            req.arg1 = specs[i].addr;
            req.arg2 = specs[i].size;
            DriverRpc reply{};
            if (!pd_rpc(sdf::PdRole::FB, req, reply) || reply.status != 0 ||
                reply.arg0 != 1) {
                Logger::warn("console bind: BIND[%lu] st=%d",
                             (unsigned long)i, (int)reply.status);
                return false;
            }
        }
        // Live geometry rides CONSOLE_MODE (pitch/width/height + bpp).
        service::FramebufferInfo info = service::Framebuffer::info();
        DriverRpc mode{};
        mode.op = static_cast<uint8_t>(DrvOp::CONSOLE_MODE);
        mode.flags = info.bpp;
        mode.owner_id = static_cast<uint32_t>(cur->id);
        mode.owner_gen = cur->generation;
        mode.epoch = epoch;
        mode.arg0 = info.pitch;
        mode.arg1 = info.width;
        mode.arg2 = info.height;
        DriverRpc mreply{};
        if (!pd_rpc(sdf::PdRole::FB, mode, mreply) || mreply.status != 0 ||
            mreply.arg0 != 1) {
            Logger::warn("console bind: MODE st=%d arg0=%lu",
                         (int)mreply.status,
                         (unsigned long)mreply.arg0);
            return false;
        }
    }
    // Text ring (row index 0).
    uint64_t rphys = 0;
    uint64_t rpages = 0;
    if (!sdf::sdf_shm_tx_phys(sdf::PdRole::FB, rphys, rpages) || rphys == 0 ||
        rpages < 2) {
        return false;
    }
    int slot0 = -1;
    uint32_t gen0 = 0;
    if (!sdf::sdf_role_cap(sdf::PdRole::FB, 0, slot0, gen0)) {
        return false;
    }
    DriverRpc shm{};
    shm.op = static_cast<uint8_t>(DrvOp::MAP_SHM);
    shm.owner_id = static_cast<uint32_t>(cur->id);
    shm.owner_gen = cur->generation;
    shm.epoch = epoch;
    shm.arg0 = cap::encode_user_handle(space->cspace_id, slot0, gen0);
    shm.arg1 = rpages;
    shm.arg2 = sdf::kSerialTxRingElemSize; /* same 8 B element wire */
    DriverRpc shm_reply{};
    if (!pd_rpc(sdf::PdRole::FB, shm, shm_reply) || shm_reply.status != 0 ||
        shm_reply.arg0 != 1) {
        Logger::warn("console bind: MAP_SHM st=%d",
                         (int)shm_reply.status,
                     (unsigned long)shm_reply.arg0);
        return false;
    }
    // Status page (row index 1, raw window).
    uint64_t sphys = 0;
    uint64_t spages = 0;
    if (!sdf::sdf_role_frame(sdf::PdRole::FB, 1, sphys, spages) ||
        sphys == 0 || spages == 0) {
        return false;
    }
    int slot1 = -1;
    uint32_t gen1 = 0;
    if (!sdf::sdf_role_cap(sdf::PdRole::FB, 1, slot1, gen1)) {
        return false;
    }
    DriverRpc st{};
    st.op = static_cast<uint8_t>(DrvOp::MAP_STATUS);
    st.owner_id = static_cast<uint32_t>(cur->id);
    st.owner_gen = cur->generation;
    st.epoch = epoch;
    st.arg0 = cap::encode_user_handle(space->cspace_id, slot1, gen1);
    st.arg1 = spages;
    DriverRpc st_reply{};
    if (!pd_rpc(sdf::PdRole::FB, st, st_reply) || st_reply.status != 0 ||
        st_reply.arg0 != 1) {
        Logger::warn("console bind: MAP_STATUS st=%d",
                             (int)st_reply.status);
        return false;
    }
    g_con_ring_phys = rphys;
    g_con_ring_pages = rpages;
    g_con_status_phys = sphys;
    g_con_status_pages = spages;
    g_con_epoch = epoch;
    g_con_bound = true;
    return true;
}

uint64_t console_write(const char *data, uint64_t len) {
    if (!console_resolve()) {
        return 0;
    }
    if (len == 0 || data == nullptr) {
        return 0;
    }
    auto *hdr = reinterpret_cast<shm::SharedRingHeader *>(kernel::HHDM_OFFSET +
                                                           g_con_ring_phys);
    if (!shm::shm_ring_valid(hdr)) {
        return 0;
    }
    uint8_t *data_base = reinterpret_cast<uint8_t *>(kernel::HHDM_OFFSET +
                                                     g_con_ring_phys +
                                                     arch::PAGE_SIZE);
    uint64_t off = 0;
    while (off < len) {
        uint8_t elem[sdf::kSerialTxRingElemSize] = {};
        uint64_t chunk = len - off;
        if (chunk > sdf::kSerialTxRingElemSize - 1) {
            chunk = sdf::kSerialTxRingElemSize - 1;
        }
        elem[0] = static_cast<uint8_t>(chunk);
        for (uint64_t i = 0; i < chunk; ++i) {
            elem[1 + i] = static_cast<uint8_t>(data[off + i]);
        }
        if (!shm::shm_ring_push(hdr, data_base, elem)) {
            __atomic_fetch_add(&g_con_dropped, len - off, __ATOMIC_RELAXED);
            break;
        }
        off += chunk;
    }
    return off;
}

bool console_status(ConsoleStatus &out) {
    if (!console_resolve() || g_con_status_phys == 0) {
        return false;
    }
    auto *st = reinterpret_cast<volatile ConsoleStatus *>(
        kernel::HHDM_OFFSET + g_con_status_phys);
    out.rendered_cells =
        __atomic_load_n(&st->rendered_cells, __ATOMIC_ACQUIRE);
    out.pass_max_cells =
        __atomic_load_n(&st->pass_max_cells, __ATOMIC_ACQUIRE);
    out.passes = __atomic_load_n(&st->passes, __ATOMIC_ACQUIRE);
    out.rejected = __atomic_load_n(&st->rejected, __ATOMIC_ACQUIRE);
    out.dropped_full = __atomic_load_n(&st->dropped_full, __ATOMIC_ACQUIRE);
    out.mode_w = __atomic_load_n(&st->mode_w, __ATOMIC_ACQUIRE);
    out.mode_h = __atomic_load_n(&st->mode_h, __ATOMIC_ACQUIRE);
    out.pitch = __atomic_load_n(&st->pitch, __ATOMIC_ACQUIRE);
    out.bpp = __atomic_load_n(&st->bpp, __ATOMIC_ACQUIRE);
    out.cols = __atomic_load_n(&st->cols, __ATOMIC_ACQUIRE);
    out.rows = __atomic_load_n(&st->rows, __ATOMIC_ACQUIRE);
    out.cursor_row = __atomic_load_n(&st->cursor_row, __ATOMIC_ACQUIRE);
    out.scanout_mapped =
        __atomic_load_n(&st->scanout_mapped, __ATOMIC_ACQUIRE);
    out.status_mapped =
        __atomic_load_n(&st->status_mapped, __ATOMIC_ACQUIRE);
    out.ring_mapped = __atomic_load_n(&st->ring_mapped, __ATOMIC_ACQUIRE);
    return true;
}

bool console_test_hold(bool hold) {
    if (!console_resolve()) {
        return false;
    }
    auto *hdr = reinterpret_cast<shm::SharedRingHeader *>(kernel::HHDM_OFFSET +
                                                           g_con_ring_phys);
    if (!shm::shm_ring_valid(hdr)) {
        return false;
    }
    __atomic_store_n(&hdr->drain_hold, hold ? 1ULL : 0ULL, __ATOMIC_RELEASE);
    return true;
}

uint64_t console_test_ring_phys() {
    if (!console_resolve()) {
        return 0;
    }
    return g_con_ring_phys;
}

bool console_test_pump() {
    if (!console_resolve()) {
        return false;
    }
    const uint64_t pid = get_pd_pid(sdf::PdRole::FB);
    if (pid == 0 || !pd_alive(sdf::PdRole::FB))
        return false;
    auto *cur = Scheduler::current_task();
    if (cur == nullptr)
        return false;
    DriverRpc req{};
    req.op = static_cast<uint8_t>(DrvOp::PRESENT);
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = pd_epoch();
    DriverRpc reply{};
    return pd_rpc(sdf::PdRole::FB, req, reply) && reply.status == 0 &&
           reply.arg0 == 1;
}
#endif // CONFIG_DRIVER_PD_FB

} // namespace driver_pds
} // namespace kernel

#endif // any CONFIG_DRIVER_PD_*
