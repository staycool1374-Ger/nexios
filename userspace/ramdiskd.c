
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <ipc.h>

#define RAMDISK_READ_BLOCK  400
#define RAMDISK_WRITE_BLOCK 401
#define RAMDISK_STAT        402
#define RAMDISK_GRANT       403

#define RAMDISK_BLOCK_SIZE       512
#define RAMDISK_CHUNK_DATA       32
#define RAMDISK_CHUNKS_PER_BLOCK 16
#define RAMDISK_BLOCKS           32768
#define RAMDISK_SEGMENTS         8
#define RAMDISK_SEGMENT_BLOCKS   4096

#define RAMDISK_OK          0
#define RAMDISK_ERR_RANGE  -1
#define RAMDISK_ERR_NOGRANT -2
#define RAMDISK_ERR_NOMEM  -3
#define RAMDISK_ERR_UNKNOWN_OP -4

#define SYS_FRAME_MAP   64
#define SYS_FRAME_UNMAP 65

#define MSG_DAEMON_READY 0xF0000001

struct RamdiskdMsg {
    uint64_t sender_pid;
    uint64_t block_no;
    uint64_t chunk_idx;
    char     data[RAMDISK_CHUNK_DATA];
};

struct RamdiskdReply {
    int64_t  result;
    char     data[RAMDISK_CHUNK_DATA];
};

static uint64_t g_my_id = 0;
static uint64_t g_seg_handle[RAMDISK_SEGMENTS] = {0};
static uint64_t g_seg_va[RAMDISK_SEGMENTS] = {0};
static int g_has_grant = 0;

static int ramdiskd_map_segment(int seg) {
    if (g_seg_va[seg] != 0)
        return 0;
    if (g_seg_handle[seg] == 0)
        return -1;
    long va = __syscall5(SYS_FRAME_MAP, (long)g_seg_handle[seg], 0, 0, 0);
    if (va < 0)
        return -1;
    g_seg_va[seg] = (uint64_t)va;
    return 0;
}

static void ramdiskd_handle_grant(struct RamdiskdMsg* msg,
                                  const char* payload) {
    (void)msg;
    // Store handles only; segments map lazily on first use so the
    // grant path stays fast (one 2 MiB mapping costs seconds under
    // TCG — mapping all eight up front stalls serving).
    for (int i = 0; i < RAMDISK_SEGMENTS; ++i) {
        uint64_t handle = 0;
        memcpy(&handle, payload + i * 8, 8);
        if (handle == 0 || handle == (uint64_t)-1) {
            g_has_grant = 0;
            return;
        }
        g_seg_handle[i] = handle;
    }
    g_has_grant = 1;
}

static int ramdiskd_check_range(uint64_t block_no, uint64_t chunk_idx) {
    if (block_no >= RAMDISK_BLOCKS)
        return -1;
    if (chunk_idx >= RAMDISK_CHUNKS_PER_BLOCK)
        return -1;
    return 0;
}

static char* ramdiskd_chunk_ptr(uint64_t block_no, uint64_t chunk_idx) {
    uint64_t seg = block_no / RAMDISK_SEGMENT_BLOCKS;
    uint64_t seg_block = block_no % RAMDISK_SEGMENT_BLOCKS;
    uint64_t off = seg_block * RAMDISK_BLOCK_SIZE +
                   chunk_idx * RAMDISK_CHUNK_DATA;
    return (char*)(g_seg_va[seg] + off);
}

static int ramdiskd_serve_range(uint64_t block_no, uint64_t chunk_idx) {
    if (!g_has_grant)
        return RAMDISK_ERR_NOGRANT;
    if (ramdiskd_check_range(block_no, chunk_idx) < 0)
        return RAMDISK_ERR_RANGE;
    uint64_t seg = block_no / RAMDISK_SEGMENT_BLOCKS;
    if (ramdiskd_map_segment((int)seg) < 0)
        return RAMDISK_ERR_NOMEM;
    return RAMDISK_OK;
}

static int ramdiskd_handle_read(struct RamdiskdMsg* msg,
                                struct RamdiskdReply* reply) {
    int rc = ramdiskd_serve_range(msg->block_no, msg->chunk_idx);
    if (rc != RAMDISK_OK) {
        reply->result = rc;
        return -1;
    }
    memcpy(reply->data, ramdiskd_chunk_ptr(msg->block_no, msg->chunk_idx),
           RAMDISK_CHUNK_DATA);
    reply->result = RAMDISK_OK;
    return 0;
}

static int ramdiskd_handle_write(struct RamdiskdMsg* msg,
                                 struct RamdiskdReply* reply) {
    int rc = ramdiskd_serve_range(msg->block_no, msg->chunk_idx);
    if (rc != RAMDISK_OK) {
        reply->result = rc;
        return -1;
    }
    memcpy(ramdiskd_chunk_ptr(msg->block_no, msg->chunk_idx), msg->data,
           RAMDISK_CHUNK_DATA);
    reply->result = RAMDISK_OK;
    return 0;
}

static int ramdiskd_handle_stat(struct RamdiskdMsg* msg,
                                struct RamdiskdReply* reply) {
    (void)msg;
    if (!g_has_grant) {
        reply->result = RAMDISK_ERR_NOGRANT;
        return -1;
    }
    reply->result = RAMDISK_OK;
    return 0;
}

static int ramdiskd_dispatch(struct RamdiskdMsg* msg, long rtype,
                             struct RamdiskdReply* reply) {
    switch (rtype) {
        case RAMDISK_READ_BLOCK:
            return ramdiskd_handle_read(msg, reply);
        case RAMDISK_WRITE_BLOCK:
            return ramdiskd_handle_write(msg, reply);
        case RAMDISK_STAT:
            return ramdiskd_handle_stat(msg, reply);
        default:
            reply->result = RAMDISK_ERR_UNKNOWN_OP;
            return -1;
    }
}

int main(void) {
    g_my_id = (uint64_t)getpid();
    printf("[ramdiskd] Ramdisk Daemon started (PID=%llu)\n", g_my_id);

    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    while (1) {
        char raw[64] = {0};
        long r = ipc_recv(raw, sizeof(raw));
        if (r < 0) continue;
        if (r == RAMDISK_GRANT) {
            ramdiskd_handle_grant((struct RamdiskdMsg*)raw,
                                  (const char*)raw);
            continue;
        }
        struct RamdiskdMsg* msg = (struct RamdiskdMsg*)raw;
        struct RamdiskdReply reply;
        reply.result = RAMDISK_ERR_UNKNOWN_OP;
        ramdiskd_dispatch(msg, r, &reply);

        ipc_send(msg->sender_pid, 0, &reply, sizeof(reply), 0);
    }

    return 0;
}
