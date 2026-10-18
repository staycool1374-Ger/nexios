#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <ipc.h>

#define IOCD_REGISTER_DEVICE 200
#define IOCD_IRQ_EVENT       201
#define IOCD_MMIO_MAP        202
#define IOCD_MMIO_UNMAP      203
#define IOCD_KEYBOARD_READ   204
#define IOCD_SERIAL_READ     205
#define IOCD_SERIAL_WRITE    206

#define MAX_DEVICES 16

struct IocdMsg {
    uint64_t type;
    uint64_t args[7];
};

struct IocdReply {
    int64_t result;
    uint64_t data[4];
};

struct DeviceEntry {
    uint64_t pid;
    char     name[32];
    uint32_t irq_line;
};

static uint64_t g_my_id = 0;
static struct DeviceEntry g_devices[MAX_DEVICES];
static size_t g_device_count = 0;

/* Issue #340 P0: set-1 scancode tables (values copied from
 * src/kernel/arch/x86_64/keyboard.cpp scancode_lower/scancode_upper —
 * the daemon cannot include kernel headers; hal_keyboard_decode pins
 * the kernel side so divergence shows in gates). */
static const char kbd_lower[128] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9',  '0', '-', '=',  0,
    0,   'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',  '[', ']', 0,    0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z',
    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*',  0,   ' ', 0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

static const char kbd_upper[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
    0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,   0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z',
    'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

/* Keyboard decode state + byte queue. Single-consumer/single-producer
 * under the daemon's serial dispatch loop (no threads): plain statics,
 * dispatch-loop-only access. 256 B bound, drop-newest on overflow
 * (oldest order preserved). */
#define KBD_QUEUE_SIZE 256
static char kbd_queue[KBD_QUEUE_SIZE];
static size_t kbd_head = 0;
static size_t kbd_count = 0;
static int kbd_shift = 0;
static int kbd_caps = 0;

static void kbd_enqueue(char c) {
    if (kbd_count >= KBD_QUEUE_SIZE)
        return; /* drop-newest: oldest order intact */
    kbd_queue[(kbd_head + kbd_count) % KBD_QUEUE_SIZE] = c;
    ++kbd_count;
}

static void kbd_decode_and_enqueue(uint8_t scancode) {
    int pressed = !(scancode & 0x80);
    uint8_t code = scancode & 0x7F;
    if (code == 0x2A || code == 0x36) {
        kbd_shift = pressed;
        return;
    }
    if (code == 0x3A && pressed) {
        kbd_caps = !kbd_caps;
        return;
    }
    if (!pressed)
        return;
    if (code == 0x1C) {
        kbd_enqueue('\n');
        return;
    }
    if (code == 0x0E) {
        kbd_enqueue('\b');
        return;
    }
    if (code == 0x0F) {
        kbd_enqueue('\t');
        return;
    }
    if (code >= 128)
        return;
    int use_shift = (!!kbd_shift) != (!!kbd_caps);
    char c = use_shift ? kbd_upper[code] : kbd_lower[code];
    if (c)
        kbd_enqueue(c);
}

static int iocd_handle_register_device(struct IocdMsg* msg, struct IocdReply* reply) {
    if (g_device_count >= MAX_DEVICES) {
        reply->result = -1;
        return 0;
    }
    struct DeviceEntry* dev = &g_devices[g_device_count++];
    dev->pid = msg->args[0];
    dev->irq_line = (uint32_t)msg->args[1];
    size_t i = 0;
    const char* src = (const char*)&msg->args[2];
    while (src[i] && i < sizeof(dev->name) - 1) {
        dev->name[i] = src[i];
        ++i;
    }
    dev->name[i] = '\0';
    reply->result = 0;
    return 0;
}

static int iocd_handle_irq_event(struct IocdMsg* msg, struct IocdReply* reply) {
    uint32_t irq = (uint32_t)msg->args[0];
    /* Issue #340 P0: IRQ line 1 carries keyboard scancodes in args[1] —
     * decode into the byte queue regardless of device registration
     * (the queue is the keyboard consumer; forwarding below is the
     * device-consumer path — both may fire, neither blocks). */
    if (irq == 1)
        kbd_decode_and_enqueue((uint8_t)msg->args[1]);
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].irq_line == irq) {
            /* Never forward to self: a self-registered line (P1
             * keyboard ownership) would loop the dispatch forever. */
            if (g_devices[i].pid == g_my_id)
                continue;
            struct IocdMsg fwd;
            fwd.type = IOCD_IRQ_EVENT;
            fwd.args[0] = irq;
            fwd.args[1] = msg->args[1];
            ipc_send(g_devices[i].pid, IOCD_IRQ_EVENT, &fwd, sizeof(fwd), 0);
            reply->result = 0;
            return 0;
        }
    }
    reply->result = -1;
    return 0;
}

static int iocd_handle_mmio_map(struct IocdMsg* msg, struct IocdReply* reply) {
    (void)msg;
    reply->result = -1;
    return 0;
}

static int iocd_handle_mmio_unmap(struct IocdMsg* msg, struct IocdReply* reply) {
    (void)msg;
    reply->result = -1;
    return 0;
}

static int iocd_handle_keyboard_read(struct IocdMsg* msg, struct IocdReply* reply) {
    (void)msg;
    /* Issue #340 P0: non-blocking serve — one byte or empty. Never
     * blocks; the consumer polls (shell helper / test harness). */
    if (kbd_count == 0) {
        reply->result = 0;
        reply->data[0] = 0;
        return 0;
    }
    reply->result = 1;
    reply->data[0] = (uint64_t)(unsigned char)kbd_queue[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_QUEUE_SIZE;
    --kbd_count;
    return 0;
}

static int iocd_handle_serial_read(struct IocdMsg* msg, struct IocdReply* reply) {
    (void)msg;
    reply->result = -1;
    return 0;
}

static int iocd_handle_serial_write(struct IocdMsg* msg, struct IocdReply* reply) {
    (void)msg;
    reply->result = -1;
    return 0;
}

static int iocd_dispatch(struct IocdMsg* msg, struct IocdReply* reply) {
    switch (msg->type) {
        case IOCD_REGISTER_DEVICE: return iocd_handle_register_device(msg, reply);
        case IOCD_IRQ_EVENT:       return iocd_handle_irq_event(msg, reply);
        case IOCD_MMIO_MAP:        return iocd_handle_mmio_map(msg, reply);
        case IOCD_MMIO_UNMAP:      return iocd_handle_mmio_unmap(msg, reply);
        case IOCD_KEYBOARD_READ:   return iocd_handle_keyboard_read(msg, reply);
        case IOCD_SERIAL_READ:     return iocd_handle_serial_read(msg, reply);
        case IOCD_SERIAL_WRITE:    return iocd_handle_serial_write(msg, reply);
        default:
            reply->result = -1;
            return -1;
    }
}

int main(void) {
    g_my_id = (uint64_t)getpid();
    printf("[iocd] I/O Controller Daemon started (PID=%llu)\n", g_my_id);

    // Signal init (PID 1) that this daemon is ready
#define MSG_DAEMON_READY 0xF0000001
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    while (1) {
        struct IocdMsg msg;
        int r = ipc_recv(&msg, sizeof(msg));
        if (r < 0) continue;
        if ((unsigned long)r != msg.type) continue;

        struct IocdReply reply = {0};
        iocd_dispatch(&msg, &reply);

        ipc_send(msg.args[0], 0, &reply, sizeof(reply), 0);
    }

    return 0;
}
