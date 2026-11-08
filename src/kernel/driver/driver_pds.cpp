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

} // namespace driver_pds
} // namespace kernel

#endif // any CONFIG_DRIVER_PD_*
