#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>
#include <ipc.h>

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

typedef enum {
    VFS_OP_BIND = 0,
    VFS_OP_INITRD = 1,
    VFS_OP_FAT32 = 2,
    VFS_OP_TMPFS = 3,
    VFS_OP_DEVFS = 4,
    VFS_OP_PIPE = 5,
    VFS_OP_RAMDISK = 6,
    VFS_OP_PROCFS_RENDER = 7,
    VFS_OP_INJECT_CORRUPT = 8,
} vfs_srv_op_t;

struct VfsRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t ino;
    uint64_t offset;
    uint64_t size;
    uint16_t status_op;
    uint16_t status;
    uint32_t rsvd2;
};

static void vfs_assert_shape(void) {
    char check[sizeof(struct VfsRpc) <= 48 ? 1 : -1];
    (void)check;
}

static int g_corrupt_once = 0;

int main(void) {
    vfs_assert_shape();
    (void)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue; // fail-closed: no reply on transport error
        struct VfsRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; // torn/stale payload: kernel type must match body op (audit S3)

        struct VfsRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;

        switch (req.op) {
        case VFS_OP_BIND:
            rep.status = 0;
            break;
        case VFS_OP_INJECT_CORRUPT:
            g_corrupt_once = 1;
            rep.status = 0;
            break;
        default:
            /* Step-a diagnostic shape: backends not served yet. */
            rep.status = (uint16_t)(uint16_t)-38;
            if (g_corrupt_once) {
                rep.status = (uint16_t)(uint16_t)-5;
                g_corrupt_once = 0;
            }
            break;
        }

        ipc_send(1, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
