// Issue #171 P3: fail-closed probe for the Ring-3 cap API, plus the
// caller-root revoke contract (DECISION (a)).
// Exercises nexios_cap_* (src/libc/cap/): bogus handles must fail
// closed (-1) without crashing, hanging, or allocating; a frame
// created in the caller root must revoke clean (0), and re-revoke
// must fail closed (-1, idempotent). Revoke searches the CALLER ROOT
// CNode only — there is deliberately no id-to-CSpace registry (that
// would be cascade machinery by another name: revoke cascades are
// excluded from the approved plan, and a registry is a new security
// surface). Cross-space revoke therefore fails closed (-1), pinned
// below. A literal Ring-3 grant needs a dst CNode handle the probe
// cannot hold (no CNode constructors in the allow-list), so
// frame_create + revoke is the executable caller-root
// acquire-then-revoke.
// Exit 0 = all observed; otherwise the failing check number.
// Net-zero residue on the green path (the frame is created, then
// revoked; every other call fails by design).

#include <cap/nexios_cap.h>

#include <errno.h>

// EINVAL: kernel SYS_ERR_INVALID_ARGS. No EINVAL in libc errno.h,
// so pinned here with the stub-roundtrip EFAULT precedent.
#define PROBE_EINVAL 22
#define PROBE_BAD_HANDLE 0x0BADCAFEUL

int main(void) {
    if (nexios_cap_grant(PROBE_BAD_HANDLE, PROBE_BAD_HANDLE) != -1) {
        return 1;
    }
    if (nexios_cap_copy(PROBE_BAD_HANDLE, PROBE_BAD_HANDLE) != -1) {
        return 2;
    }
    // DECISION (a): caller-root acquire-then-revoke. frame_create
    // installs a FrameCap in OUR root CSpace; revoking its handle
    // must succeed (0); revoking again must fail closed (-1).
    long fr = nexios_cap_frame_create(1);
    if (fr < 0) {
        return 3;
    }
    if (nexios_cap_revoke((nexios_cap_t)fr) != 0) {
        return 4;
    }
    if (nexios_cap_revoke((nexios_cap_t)fr) != -1) {
        return 5;
    }
    // Cross-space pin: a handle rooted nowhere in our CSpace fails
    // closed (-1) — no registry lookup, no cascade, no crash.
    if (nexios_cap_revoke(PROBE_BAD_HANDLE) != -1) {
        return 6;
    }
    if (nexios_cap_mint(PROBE_BAD_HANDLE, PROBE_BAD_HANDLE,
                        NEXIOS_CAP_READ, 0) != -1) {
        return 7;
    }
    // count == 0 is rejected before any allocation (fail closed).
    errno = 0;
    if (nexios_cap_frame_create(0) != -1) {
        return 8;
    }
    if (nexios_cap_frame_map(PROBE_BAD_HANDLE) != -1) {
        return 9;
    }
    if (nexios_cap_mmio_map(PROBE_BAD_HANDLE) != -1) {
        return 10;
    }
    if (nexios_cap_irq_register(PROBE_BAD_HANDLE,
                                NEXIOS_IRQ_WAIT) != -1) {
        return 11;
    }
    if (nexios_cap_iommu_map(PROBE_BAD_HANDLE,
                             PROBE_BAD_HANDLE) != -1) {
        return 12;
    }
    return 0;
}
