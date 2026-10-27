/* NexIOS RTOS — #171 P6 reference Ring-3 driver (draft).
 *
 * Exercises the userspace contract end to end through the P1 crt0
 * 6-step entry: SDK-version pin (compile-time), VFS (open/read/close
 * on the initrd image itself with an ELF-magic assert), and IPC
 * fail-closed behavior (async send to a nonexistent PID must return
 * negative via the lookup-miss early return, never block) plus a self-notify post (no peer required).
 *
 * Exit 0 = every check green; 1..5 name the failing check (probe
 * convention: the exit code IS the assertion channel, serial text is
 * diagnostic-only). No check blocks: the bogus-destination send misses
 * in `Scheduler::find_task` and returns false before any queue-full
 * blocking (the `IPC_NONBLOCK` flag is passed for forward-compatibility
 * only — currently dropped at the syscall boundary), and notify is
 * post-only, so the worst case is a
 * nonzero exit, never a hang under `runelf`.
 *
 * Post-merge E2E (P2+P3 headers land): the stub round-trip
 * (ABI_VERSION/CREATE+GETTIME+DELETE, cf. userspace/stub-roundtrip.c
 * on the P2 lane) and the cap-API lifecycle matrix (grant/copy/
 * revoke, cf. qe-171-p3-matrix) extend this driver; the responder
 * pattern (harness PID 1 replies, cf. userspace/ipcsync-probe.c)
 * provides the positive IPC round-trip. Until then this draft proves
 * the entry + VFS + fail-closed subset, which is all main carries.
 */

#include <sdk_version.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <ipc.h>

/* P0 staged-drift pin, consumer side: this driver was written against
 * SDK 0.1 / CRT0 contract 1. A lane bump breaks this build loudly
 * instead of silently changing behavior under it. */
_Static_assert(NEXIOS_SDK_MAJOR == 0, "ref-driver pins SDK major 0");
_Static_assert(NEXIOS_CRT0_CONTRACT == 1, "ref-driver pins CRT0 contract 1");

#define CHECK_VFS_OPEN 1
#define CHECK_VFS_READ 2
#define CHECK_VFS_MAGIC 3
#define CHECK_IPC_FAIL_CLOSED 4
#define CHECK_IPC_NOTIFY_SELF 5

/* A PID that can never exist: PIDs are small dense task indices, so
 * 9999 is out of range on every boot and always misses in
 * Scheduler::find_task (null/TERMINATED -> false before any queue
 * blocking). The kernel must fail the send
 * closed (negative return) without touching the caller. The
 * IPC_NONBLOCK flag is passed for forward-compatibility; it is
 * currently dropped at the syscall boundary and is not the
 * no-block mechanism. */
#define BOGUS_PID 9999u
#define PROBE_TYPE 0x55u

int main(void) {
    printf("ref-driver: SDK %d.%d cap-API %d.%d crt0-contract %d\n",
           NEXIOS_SDK_MAJOR, NEXIOS_SDK_MINOR, NEXIOS_CAP_API_MAJOR,
           NEXIOS_CAP_API_MINOR, NEXIOS_CRT0_CONTRACT);

    /* --- VFS: read our own image back through the VFS path. --- */
    int fd = open("/hey.c.elf", 0);
    if (fd < 0)
        return CHECK_VFS_OPEN;

    unsigned char magic[4];
    ssize_t n = read(fd, magic, sizeof(magic));
    close(fd);
    if (n != (ssize_t)sizeof(magic))
        return CHECK_VFS_READ;
    if (magic[0] != 0x7Fu || magic[1] != (unsigned char)'E' ||
        magic[2] != (unsigned char)'L' || magic[3] != (unsigned char)'F')
        return CHECK_VFS_MAGIC;
    printf("ref-driver: VFS OK (image has ELF magic)\n");

    /* --- IPC fail-closed: no such destination, must not block. --- */
    unsigned char req[4] = {0x10, 0x11, 0x12, 0x13};
    int r = ipc_send(BOGUS_PID, PROBE_TYPE, req, sizeof(req),
                     IPC_NONBLOCK);
    if (r >= 0)
        return CHECK_IPC_FAIL_CLOSED;
    printf("ref-driver: IPC fail-closed OK (bogus send -> %d)\n", r);

    /* --- IPC self-notify: async post to our own queue, no peer. --- */
    unsigned long me = (unsigned long)getpid();
    r = ipc_notify(me, 0xA5A5u);
    if (r < 0)
        return CHECK_IPC_NOTIFY_SELF;
    printf("ref-driver: IPC notify-self OK (pid %lu)\n", me);

    printf("ref-driver: ALL GREEN\n");
    return 0;
}
