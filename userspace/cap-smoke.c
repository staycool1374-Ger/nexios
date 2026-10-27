// Issue #171 P3: fail-closed probe for the Ring-3 cap API.
// Exercises nexios_cap_* (src/libc/cap/) with handles that cannot
// resolve: every call must fail closed (-1) without crashing, hanging,
// or allocating. Exit 0 = all fail-closed observed; otherwise the
// failing check number. Creates no residue: every probe call fails by
// design (bogus handles, count==0), so no cap is ever installed.

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
    if (nexios_cap_revoke(PROBE_BAD_HANDLE) != -1) {
        return 3;
    }
    if (nexios_cap_mint(PROBE_BAD_HANDLE, PROBE_BAD_HANDLE,
                        NEXIOS_CAP_READ, 0) != -1) {
        return 4;
    }
    // count == 0 is rejected before any allocation (fail closed).
    errno = 0;
    if (nexios_cap_frame_create(0) != -1) {
        return 5;
    }
    if (nexios_cap_frame_map(PROBE_BAD_HANDLE) != -1) {
        return 6;
    }
    if (nexios_cap_mmio_map(PROBE_BAD_HANDLE) != -1) {
        return 7;
    }
    if (nexios_cap_irq_register(PROBE_BAD_HANDLE,
                                NEXIOS_IRQ_WAIT) != -1) {
        return 8;
    }
    if (nexios_cap_iommu_map(PROBE_BAD_HANDLE,
                             PROBE_BAD_HANDLE) != -1) {
        return 9;
    }
    return 0;
}
