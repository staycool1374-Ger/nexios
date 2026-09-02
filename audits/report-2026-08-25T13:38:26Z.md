# AUDIT REPORT 2026-08-25T13:38:26Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/drivers.md, src/kernel/driver/ahci.cpp, src/kernel/driver/ahci.hpp, src/kernel/driver/virtio_blk.cpp, src/kernel/driver/virtio_blk.hpp, src/kernel/driver/virtio_net.cpp, src/kernel/net/net.cpp

## FINDINGS
- [S3] src/kernel/driver/ahci.cpp:98-102 — Redundant `active_port_ >= AHCI_MAX_PORTS` re-check after locking cmd_lock_[active_port_]. The check is duplicated (once before slot allocation, once after), but is harmless since active_port_ does not change between checks.
- [S3] src/kernel/driver/ahci.cpp:69 — us→ticks conversion uses `timeout_us / 1000` (assuming ~1 kHz tick rate). For timeout_us=5_000_000, this yields 5000 ticks ≈ 5s. The +1 provides a small buffer. Assumes Timer::ticks() is monotonic, so wraparound is not a concern for practical timeouts.
- [S3] src/kernel/driver/virtio_blk.cpp:229 — Snapshot used_idx BEFORE avail push + virtio_notify follows the FLAW-03 pattern correctly. Reading after notify would race fast completions.
- [S3] src/kernel/driver/virtio_net.cpp:313-317 — Bounds-check `desc_idx < queue_size` before indexing rx_bufs[] prevents OOB read/write from device-controlled index. The invalid-id path advances rx_last_seen_used and returns false without refilling; this is safe because an out-of-bounds index cannot be reliably used for refill anyway.
- [S3] src/kernel/net/net.cpp:391-396 — `net_icmp_last_reply()` returns a pointer to g_icmp_reply after releasing the lock. This is acceptable under the documented single-core cooperative polling model: the caller reads the record immediately in the same task context, and the lock orders the preceding write before this read. No other core can modify it concurrently.
- [S3] src/kernel/net/net.cpp:433-434,451-452 — `g_ip_ident` increment converted from post-increment (`g_ip_ident++`) to `__atomic_fetch_add(&g_ip_ident, 1U, __ATOMIC_RELAXED)`. Cooperative polling on single core makes the atomic change defensive but harmless; it eliminates any theoretical duplicate-value risk through interleaved poll-driven paths.
- [S3] src/kernel/net/net.cpp:377,384,404,414,442 — `g_net_lock` SpinLock correctly guards the ICMP reply record, ARP-cache update, and net_init cache clear. The lock is never held across a context switch; all operations are task-context poll-driven on single core.

## PATCH
audits/rejected_patch.diff was NOT written; the patch applies cleanly with no violations requiring correction.

## DECISION: APPROVED
