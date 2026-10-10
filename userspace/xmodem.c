
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <ipc.h>
#include <time.h>
#include "xmodem_crc.h"

#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define SUB 0x1A
#define CHR_C 0x43

#define PKT_128 128
#define PKT_1024 1024
#define PKT_MAX PKT_1024

#define RAMDISK_READ_BLOCK  400
#define RAMDISK_WRITE_BLOCK 401
#define RAMDISK_OK          0
#define RAMDISK_ERR_RANGE  -1
#define RAMDISK_ERR_NOGRANT -2
#define RAMDISK_ERR_NOMEM  -3

#define RAMDISK_BLOCKS           32768
#define RAMDISK_BLOCK_SIZE       512
#define RAMDISK_CHUNK_DATA       32
#define RAMDISK_CHUNKS_PER_BLOCK 16

#define MAX_RETRIES 10
#define C_TRIES 10
#define C_INTERVAL_S 3
#define PKT_TIMEOUT_S 10
#define OVERALL_CAP_S 3600

static unsigned char g_packet[PKT_MAX];
static unsigned char g_staging[RAMDISK_BLOCK_SIZE];
static unsigned long g_ramdisk_pid = 0;
static int g_mode = 0;
static int g_tty_fd = 0;

/// @brief Open the serial tty in RAW mode (issue #276): cooked mode
/// maps CR (0x0D) to LF (0x0A), which corrupts binary packet bytes
/// (proven: golden byte 38 0x0D arrived as 0x0A, CRC 0x2EE4 became
/// 0xC64A). Fail fast when raw open fails — silent corruption is
/// worse than no transfer.

static int x_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static unsigned long x_atoul(const char* s) {
    unsigned long v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 10) {
        v = v * 10 + (unsigned long)(*s - '0');
        ++s;
        ++digits;
    }
    return v;
}

/// @brief Bounded decimal parse of a YMODEM size field: digits must
/// terminate (NUL or end) within the packet; unbounded runs reject.
static unsigned long x_atoul_bounded(const char* s, unsigned int maxlen,
                                     int* ok) {
    unsigned long v = 0;
    unsigned int i = 0;
    *ok = 0;
    while (i < maxlen && s[i] >= '0' && s[i] <= '9') {
        if (i >= 10)
            return 0;
        v = v * 10 + (unsigned long)(s[i] - '0');
        ++i;
    }
    if (i == 0 || i >= maxlen)
        return 0;
    *ok = 1;
    return v;
}

static unsigned long x_now(void) {
    return (unsigned long)time(0);
}

/// @brief Blocking-with-deadline byte read: poll non-blocking tty_read
/// with 1 ms sleeps; -1 on timeout. time() is the only clock.
/// NOTE: tty_read returns -1 on empty (EAGAIN-style, never blocks), so
/// -1 here means "no data yet" and the loop retries until the deadline.
static int serial_getc(unsigned long deadline) {
    unsigned char c = 0;
    struct timespec nap;
    nap.tv_sec = 0;
    nap.tv_nsec = 1000000;
    for (;;) {
        long r = read(g_tty_fd, &c, 1);
        if (r == 1)
            return (int)c;
        if (x_now() >= deadline)
            return -1;
        nanosleep(&nap, 0);
    }
}

static void serial_putc(unsigned char c) {
    write(1, &c, 1);
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int b64_pending[3];
static int b64_n = 0;
static int b64_i = 0;

/// @brief Next stream byte through the selected framing (0=raw).
/// Decoded output is clamped: base64 yields at most 3 bytes per
/// quartet, hex exactly 1 per pair — never more than the caller
/// asked for. Returns -1 on timeout or framing error.
static int next_byte(unsigned long deadline) {
    if (g_mode == 0)
        return serial_getc(deadline);
    if (g_mode == 1) {
        if (b64_i < b64_n)
            return b64_pending[b64_i++];
        {
            int vals[4];
            int n = 0;
            int b0 = 0;
            int b1 = 0;
            int b2 = 0;
            int b3 = 0;
            while (n < 4) {
                int c = serial_getc(deadline);
                int v = 0;
                if (c < 0)
                    return -1;
                if (c == '\r' || c == '\n')
                    continue;
                if (c == '=') {
                    if (n < 2)
                        return -1;
                    vals[n++] = -2;
                    continue;
                }
                v = b64_val((char)c);
                if (v < 0)
                    return -1;
                vals[n++] = v;
            }
            b0 = vals[0] >= 0 ? vals[0] : 0;
            b1 = vals[1] >= 0 ? vals[1] : 0;
            b2 = vals[2] >= 0 ? vals[2] : 0;
            b3 = vals[3] >= 0 ? vals[3] : 0;
            b64_pending[0] = (b0 << 2) | (b1 >> 4);
            b64_pending[1] = ((b1 & 15) << 4) | (b2 >> 2);
            b64_pending[2] = ((b2 & 3) << 6) | b3;
            b64_n = 3;
            if (vals[3] == -2)
                b64_n = 2;
            if (vals[2] == -2)
                b64_n = 1;
            b64_i = 0;
            if (b64_i < b64_n)
                return b64_pending[b64_i++];
        }
        return -1;
    }
    {
        int hi = -1;
        for (;;) {
            int c = serial_getc(deadline);
            int v = 0;
            if (c < 0)
                return -1;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                continue;
            v = hex_val((char)c);
            if (v < 0)
                return -1;
            if (hi < 0) {
                hi = v;
                continue;
            }
            return (hi << 4) | v;
        }
    }
}

/// @brief Read exactly len stream bytes (len clamped to PKT_MAX).
/// Returns 0 on success, -1 on timeout/framing error/overrun.
static int read_bytes(unsigned char* out, unsigned int len,
                      unsigned long deadline) {
    unsigned int i = 0;
    if (len > PKT_MAX)
        return -1;
    for (i = 0; i < len; ++i) {
        int c = next_byte(deadline);
        if (c < 0)
            return -1;
        out[i] = (unsigned char)c;
    }
    return 0;
}

/// @brief Write one 512 B block to ramdisk start_block+block_idx.
/// NOTE (#315): the send_sync ABI has no reply-buffer slot — the kernel
/// writes the reply payload back over the REQUEST buffer and in-tree
/// libc drops its reply_buf arg (contract follow-up filed separately).
/// Callers MUST read result/payload from req[] after the call
/// (result req[0..8), payload req[8..40)) and req must stay >= 40 B.
/// Returns 0 on success, ramdisk error (<0) or -9 on IPC failure.
static int ramdisk_write_block(unsigned long start_block,
                               unsigned long block_idx,
                               const unsigned char* data) {
    unsigned long block_no = start_block + block_idx;
    unsigned long chunk = 0;
    unsigned long me = (unsigned long)getpid();
    char req[56];
    if (block_no >= RAMDISK_BLOCKS)
        return RAMDISK_ERR_RANGE;
    for (chunk = 0; chunk < RAMDISK_CHUNKS_PER_BLOCK; ++chunk) {
        unsigned int i = 0;
        int r = 0;
        long result = 0;
        unsigned int k = 0;
        for (i = 0; i < 8; ++i) {
            req[i] = (char)((me >> (i * 8)) & 0xFF);
            req[8 + i] = (char)((block_no >> (i * 8)) & 0xFF);
            req[16 + i] = (char)((chunk >> (i * 8)) & 0xFF);
        }
        for (i = 0; i < RAMDISK_CHUNK_DATA; ++i)
            req[24 + i] = (char)data[chunk * RAMDISK_CHUNK_DATA + i];
        r = ipc_send_sync(g_ramdisk_pid, RAMDISK_WRITE_BLOCK, req,
                          sizeof(req), req);
        if (r < 0)
            return -9;
        for (k = 0; k < 8; ++k)
            result |= ((long)(unsigned char)req[k]) << (k * 8);
        if (result != RAMDISK_OK)
            return (int)result;
    }
    return 0;
}

/// @brief Read one 512 B block from ramdisk into data. Same returns.
/// Reply-over-request contract: see ramdisk_write_block (#315). The
/// request length is sizeof(req) (not 24): the kernel copies up to 40
/// reply bytes back, so the checked range must cover them.
static int ramdisk_read_block(unsigned long start_block,
                              unsigned long block_idx,
                              unsigned char* data) {
    unsigned long block_no = start_block + block_idx;
    unsigned long chunk = 0;
    unsigned long me = (unsigned long)getpid();
    char req[56];
    if (block_no >= RAMDISK_BLOCKS)
        return RAMDISK_ERR_RANGE;
    for (chunk = 0; chunk < RAMDISK_CHUNKS_PER_BLOCK; ++chunk) {
        unsigned int i = 0;
        int r = 0;
        long result = 0;
        unsigned int k = 0;
        for (i = 0; i < 8; ++i) {
            req[i] = (char)((me >> (i * 8)) & 0xFF);
            req[8 + i] = (char)((block_no >> (i * 8)) & 0xFF);
            req[16 + i] = (char)((chunk >> (i * 8)) & 0xFF);
        }
        r = ipc_send_sync(g_ramdisk_pid, RAMDISK_READ_BLOCK, req,
                          sizeof(req), req);
        if (r < 0)
            return -9;
        for (k = 0; k < 8; ++k)
            result |= ((long)(unsigned char)req[k]) << (k * 8);
        if (result != RAMDISK_OK)
            return (int)result;
        for (k = 0; k < RAMDISK_CHUNK_DATA; ++k)
            data[chunk * RAMDISK_CHUNK_DATA + k] =
                (unsigned char)req[8 + k];
    }
    return 0;
}

/// @brief Receive one packet body for start byte; validates
/// seq/complement + CRC16. Returns data length (128/1024), or
/// -1 timeout, -2 CRC/seq error.
static int rx_packet_body(int start, unsigned long* seq_out,
                          unsigned long deadline) {
    unsigned char hdr[2];
    unsigned int data_len = (start == STX) ? PKT_1024 : PKT_128;
    unsigned char crcb[2];
    unsigned short want = 0;
    unsigned short got = 0;
    if (read_bytes(hdr, 2, deadline) < 0)
        return -1;
    if (((hdr[0] ^ hdr[1]) & 0xFF) != 0xFF)
        return -2;
    *seq_out = hdr[0];
    if (read_bytes(g_packet, data_len, deadline) < 0)
        return -1;
    if (read_bytes(crcb, 2, deadline) < 0)
        return -1;
    want = (unsigned short)(((unsigned short)crcb[0] << 8) | crcb[1]);
    got = xmodem_crc16(g_packet, data_len);
    if (got != want)
        return -2;
    return (int)data_len;
}

/// @brief Feed one stream byte into the staging block; flushes full
/// blocks to ramdisk. Returns 0 ok, ramdisk error (<0), -9 IPC fail.
static int stage_byte(unsigned long start_block, unsigned long* total,
                      unsigned char b) {
    unsigned long blk = *total / RAMDISK_BLOCK_SIZE;
    unsigned long off = *total % RAMDISK_BLOCK_SIZE;
    int w = 0;
    if (start_block + blk >= RAMDISK_BLOCKS)
        return RAMDISK_ERR_RANGE;
    if (off == 0) {
        unsigned int k = 0;
        for (k = 0; k < RAMDISK_BLOCK_SIZE; ++k)
            g_staging[k] = 0;
    }
    g_staging[off] = b;
    *total += 1;
    if (*total % RAMDISK_BLOCK_SIZE == 0) {
        w = ramdisk_write_block(start_block, blk, g_staging);
        if (w != 0)
            return w;
    }
    return 0;
}

/// @brief Flush a partial staging block (zero-padded) to ramdisk.
static int stage_flush(unsigned long start_block, unsigned long total) {
    unsigned long off = total % RAMDISK_BLOCK_SIZE;
    unsigned long blk = 0;
    unsigned int k = 0;
    int w = 0;
    if (off == 0)
        return 0;
    blk = total / RAMDISK_BLOCK_SIZE;
    for (k = (unsigned int)off; k < RAMDISK_BLOCK_SIZE; ++k)
        g_staging[k] = 0;
    w = ramdisk_write_block(start_block, blk, g_staging);
    return w;
}

/// @brief Run the receive session into ramdisk at start_block.
/// Returns bytes written, or negative -reason on abort.
static long rx_run(unsigned long start_block, unsigned long cap_time) {
    unsigned long expected = 1;
    unsigned long total = 0;
    unsigned long ysize = 0;
    int have_ysize = 0;
    unsigned long retries = 0;
    int c_tries = 0;
    int start = 0;
    printf("READY\n");
    while (c_tries < C_TRIES) {
        serial_putc(CHR_C);
        start = next_byte(x_now() + C_INTERVAL_S);
        if (start == SOH || start == STX)
            goto packet;
        if (start == CAN)
            return -2;
        ++c_tries;
    }
    return -2;
packet:
    for (;;) {
        unsigned long seq = 0;
        int rc = 0;
        unsigned int bi = 0;
        if (x_now() > cap_time)
            goto abort;
        if (start < 0) {
            start = next_byte(x_now() + PKT_TIMEOUT_S);
            if (start < 0) {
                if (++retries > MAX_RETRIES)
                    goto abort;
                serial_putc(NAK);
                continue;
            }
        }
        if (start == EOT) {
            serial_putc(ACK);
            break;
        }
        if (start == CAN)
            return -2;
        if (start != SOH && start != STX) {
            if (++retries > MAX_RETRIES)
                goto abort;
            serial_putc(NAK);
            start = -1;
            continue;
        }
        rc = rx_packet_body(start, &seq, x_now() + PKT_TIMEOUT_S);
        start = -1;
        if (rc < 0) {
            serial_putc(NAK);
            if (++retries > MAX_RETRIES)
                goto abort;
            continue;
        }
        if (seq == 0 && expected == 1 && !have_ysize) {
            unsigned int k = 0;
            int ok = 0;
            while (k < (unsigned int)rc && g_packet[k])
                ++k;
            ++k;
            if (k >= (unsigned int)rc) {
                serial_putc(NAK);
                if (++retries > MAX_RETRIES)
                    goto abort;
                continue;
            }
            ysize = x_atoul_bounded((const char*)(g_packet + k),
                                    (unsigned int)rc - k, &ok);
            if (!ok) {
                serial_putc(NAK);
                if (++retries > MAX_RETRIES)
                    goto abort;
                continue;
            }
            have_ysize = 1;
            serial_putc(ACK);
            continue;
        }
        if (seq != (expected & 0xFF)) {
            if (seq == ((expected - 1) & 0xFF)) {
                serial_putc(ACK);
                continue;
            }
            goto abort;
        }
        for (bi = 0; bi < (unsigned int)rc; ++bi) {
            int w = 0;
            if (have_ysize && total >= ysize)
                break;
            w = stage_byte(start_block, &total, g_packet[bi]);
            if (w != 0)
                goto abort;
        }
        if (have_ysize && total >= ysize) {
            int e = 0;
            serial_putc(ACK);
            ++expected;
            e = next_byte(x_now() + PKT_TIMEOUT_S);
            if (e == EOT)
                serial_putc(ACK);
            break;
        }
        serial_putc(ACK);
        ++expected;
        retries = 0;
    }
    {
        int f = stage_flush(start_block, total);
        if (f != 0)
            return f;
    }
    return (long)total;
abort:
    serial_putc(CAN);
    serial_putc(CAN);
    return -3;
}

/// @brief 64 KiB staging for rxfile (issue #314 file-ness bridge).
/// YMODEM-declared sizes above this are refused (fail-closed).
#define RXFILE_MAX_SIZE 65536
#define RXFILE_NAME_LEN 63
#define RXFILE_DIR "/mnt/ramdisk/"

static unsigned char g_rxfile[RXFILE_MAX_SIZE];

/// @brief Validate an upload filename (flat store: no separators).
/// @return 1 valid, 0 invalid.
static int rxfile_name_ok(const char* name) {
    int len = 0;
    if (!name || !name[0])
        return 0;
    while (name[len]) {
        if (name[len] == '/')
            return 0;
        ++len;
        if (len > RXFILE_NAME_LEN)
            return 0;
    }
    return 1;
}

/// @brief YMODEM-to-file receive (issue #314): block-0 carries name +
/// size; data packets stream into g_rxfile; on EOT the buffer is written
/// to /mnt/ramdisk/<name> (unlink-first), closed, re-read and verified.
/// Raw-mode only (SOH/STX packets); any failure unlinks the partial file
/// (fail-closed, no torn file left behind). The verified name is copied
/// to out_name (RXFILE_NAME_LEN+1). Returns staged bytes, or negative
/// -reason on abort.
static long rxfile_run(unsigned long cap_time, char *out_name) {
    unsigned long expected = 1;
    unsigned long total = 0;
    unsigned long ysize = 0;
    int retries = 0;
    int c_tries = 0;
    int start = 0;
    char name[RXFILE_NAME_LEN + 1];
    char path[13 + RXFILE_NAME_LEN + 1];
    unsigned long ni = 0;
    int fd = -1;
    printf("READY\n");
    while (c_tries < C_TRIES) {
        serial_putc(CHR_C);
        start = next_byte(x_now() + C_INTERVAL_S);
        if (start == SOH || start == STX)
            goto file_packet;
        if (start == CAN)
            return -2;
        ++c_tries;
    }
    return -2;
file_packet:
    for (;;) {
        unsigned long seq = 0;
        int rc = 0;
        unsigned int bi = 0;
        if (x_now() > cap_time) {
            printf("rxfile abort-cap total=%lu ysize=%lu now=%lu cap=%lu\n",
                   total, ysize, x_now(), cap_time);
            goto file_abort;
        }
        if (start < 0) {
            start = next_byte(x_now() + PKT_TIMEOUT_S);
            if (start < 0) {
                if (++retries > MAX_RETRIES)
                    goto file_abort;
                serial_putc(NAK);
                continue;
            }
        }
        if (start == EOT) {
            serial_putc(ACK);
            break;
        }
        if (start == CAN)
            return -2;
        if (start != SOH && start != STX) {
            if (++retries > MAX_RETRIES)
                goto file_abort;
            serial_putc(NAK);
            start = -1;
            continue;
        }
        rc = rx_packet_body(start, &seq, x_now() + PKT_TIMEOUT_S);
        start = -1;
        if (rc < 0) {
            serial_putc(NAK);
            if (++retries > MAX_RETRIES)
                goto file_abort;
            continue;
        }
        if (seq == 0 && expected == 1 && ysize == 0) {
            // YMODEM block-0: name NUL size NUL (raw bytes, bounded scan).
            unsigned int k = 0;
            int ok = 0;
            while (k < (unsigned int)rc && g_packet[k])
                ++k;
            if (k >= (unsigned int)rc || k > RXFILE_NAME_LEN) {
                serial_putc(NAK);
                if (++retries > MAX_RETRIES)
                    goto file_abort;
                continue;
            }
            for (ni = 0; ni <= k && ni < sizeof(name) - 1; ++ni)
                name[ni] = (char)g_packet[ni];
            name[ni] = '\0';
            if (out_name != 0) {
                unsigned int qi = 0;
                while (name[qi] && qi < RXFILE_NAME_LEN) {
                    out_name[qi] = name[qi];
                    ++qi;
                }
                out_name[qi] = '\0';
            }
            ++k;
            if (!rxfile_name_ok(name)) {
                serial_putc(NAK);
                if (++retries > MAX_RETRIES)
                    goto file_abort;
                continue;
            }
            ysize = x_atoul_bounded((const char*)(g_packet + k),
                                    (unsigned int)rc - k, &ok);
            if (!ok || ysize == 0 || ysize > RXFILE_MAX_SIZE) {
                serial_putc(NAK);
                if (++retries > MAX_RETRIES)
                    goto file_abort;
                continue;
            }
            serial_putc(ACK);
            continue;
        }
        if (ysize == 0) {
            // Raw (non-YMODEM) stream has no filename — refuse (use rx
            // for raw block staging, rxfile is YMODEM-files only).
            goto file_abort;
        }
        if (seq != (expected & 0xFF)) {
            if (seq == ((expected - 1) & 0xFF)) {
                serial_putc(ACK);
                continue;
            }
            printf("rxfile abort-seq seq=%lu expected=%lu total=%lu\n", seq,
                   expected, total);
            goto file_abort;
        }
        for (bi = 0; bi < (unsigned int)rc; ++bi) {
            if (total >= ysize)
                break;
            if (total >= RXFILE_MAX_SIZE)
                goto file_abort;
            g_rxfile[total++] = g_packet[bi];
        }
        serial_putc(ACK);
        ++expected;
        retries = 0;
        if (total >= ysize) {
            int e = next_byte(x_now() + PKT_TIMEOUT_S);
            if (e == EOT)
                serial_putc(ACK);
            break;
        }
    }
    if (total != ysize)
        goto file_abort;
    {
        unsigned int pi = 0;
        const char *prefix = RXFILE_DIR;
        while (prefix[pi] && pi < sizeof(path) - 1) {
            path[pi] = prefix[pi];
            ++pi;
        }
        for (ni = 0; name[ni] && pi < sizeof(path) - 1; ++ni)
            path[pi++] = name[ni];
        path[pi] = '\0';
        unlink(path);
        fd = open(path, O_CREAT | O_WRONLY);
        if (fd < 0)
            goto file_abort;
        {
            unsigned long off = 0;
            while (off < total) {
                unsigned long take = total - off;
                long w = 0;
                if (take > 512)
                    take = 512;
                w = write(fd, (const char*)(g_rxfile + off), take);
                if (w <= 0) {
                    close(fd);
                    unlink(path);
                    return -3;
                }
                off += (unsigned long)w;
            }
        }
        close(fd);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            unlink(path);
            return -3;
        }
        {
            unsigned long off = 0;
            char vbuf[512];
            while (off < total) {
                long r = read(fd, vbuf, 512);
                unsigned long k = 0;
                if (r <= 0) {
                    close(fd);
                    unlink(path);
                    return -3;
                }
                for (k = 0; k < (unsigned long)r; ++k) {
                    if (off + k >= total ||
                        (unsigned char)vbuf[k] != g_rxfile[off + k]) {
                        close(fd);
                        unlink(path);
                        return -3;
                    }
                }
                off += (unsigned long)r;
            }
        }
        close(fd);
    }
    return (long)total;
file_abort:
    serial_putc(CAN);
    serial_putc(CAN);
    return -3;
}

static int dump_run(unsigned long start_block, unsigned long nbytes) {
    unsigned long done = 0;
    static unsigned char blkbuf[RAMDISK_BLOCK_SIZE];
    while (done < nbytes) {
        unsigned long blk = done / RAMDISK_BLOCK_SIZE;
        unsigned long off = done % RAMDISK_BLOCK_SIZE;
        unsigned long take = nbytes - done;
        unsigned long room = RAMDISK_BLOCK_SIZE - off;
        unsigned long t = 0;
        if (take > room)
            take = room;
        if (off == 0 || done == 0) {
            int r = ramdisk_read_block(start_block, blk, blkbuf);
            if (r != 0) {
                printf("dump err %d\n", r);
                return 3;
            }
        }
        for (t = 0; t < take; ++t) {
            unsigned char c = blkbuf[off + t];
            if (g_mode == 2) {
                char hx[3];
                hx[0] = "0123456789ABCDEF"[(c >> 4) & 15];
                hx[1] = "0123456789ABCDEF"[c & 15];
                hx[2] = '\n';
                write(1, hx, 3);
            } else {
                write(1, (char*)&c, 1);
            }
        }
        done += take;
    }
    return 0;
}

/// @brief Print an unsigned long in decimal (issue #276: in-tree
/// printf %lu/%llu misrenders some 64-bit values in userspace, so
/// transfer counts use this direct path).
static void print_u64(unsigned long v) {
    char buf[24];
    int i = 0;
    int j = 0;
    if (v == 0) {
        write(1, "0", 1);
        return;
    }
    while (v > 0 && i < 23) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (j = i - 1; j >= 0; --j)
        write(1, &buf[j], 1);
}

static int read_line(char* buf, int cap) {    int i = 0;
    char c = 0;
    struct timespec nap;
    nap.tv_sec = 0;
    nap.tv_nsec = 1000000;
    // Interactive prompts have no deadline: an empty poll (-1) is
    // retried until a byte arrives (tty_read never blocks). EOF
    // (TCP close) surfaces as repeated -1 with no progress — the
    // session owner kills QEMU in that case.
    while (i < cap - 1) {
        long r = read(g_tty_fd, &c, 1);
        if (r != 1) {
            nanosleep(&nap, 0);
            continue;
        }
        if (c == '\n' || c == '\r')
            break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

int main(void) {
    char line[64];
    unsigned long start = 0;
    int is_rx = 0;
    int is_dump = 0;
    int is_rxfile = 0;
    g_tty_fd = open("/dev/tty", O_RAWTTY);
    if (g_tty_fd < 0) {
        printf("xmodem: raw tty open failed\n");
        return 1;
    }
    printf("xmodem receiver (uart|base64|hex), cmd (rx|rxfile|dump|quit)\n");
    for (;;) {
        printf("mode: ");
        if (read_line(line, sizeof(line)) < 0)
            return 1;
        if (x_strcmp(line, "uart") == 0)
            g_mode = 0;
        else if (x_strcmp(line, "base64") == 0)
            g_mode = 1;
        else if (x_strcmp(line, "hex") == 0)
            g_mode = 2;
        else {
            printf("usage: uart|base64|hex\n");
            continue;
        }
        printf("cmd: ");
        if (read_line(line, sizeof(line)) < 0)
            return 1;
        if (x_strcmp(line, "quit") == 0 || x_strcmp(line, "exit") == 0)
            return 0;
        is_rx = (x_strcmp(line, "rx") == 0);
        is_dump = (x_strcmp(line, "dump") == 0);
        is_rxfile = (x_strcmp(line, "rxfile") == 0);
        if (!is_rx && !is_dump && !is_rxfile) {
            printf("usage: rx|rxfile|dump|quit\n");
            continue;
        }
        if (is_rxfile) {
            // YMODEM-to-file (issue #314): filename + size come from
            // block-0; the file lands under /mnt/ramdisk/ (raw tty only
            // — base64/hex file framing is a follow-up).
            if (g_mode != 0) {
                printf("rxfile needs uart mode\n");
                continue;
            }
            {
                char rname[RXFILE_NAME_LEN + 1];
                long frc = 0;
                unsigned int wi = 0;
                rname[0] = '\0';
                frc = rxfile_run(x_now() + OVERALL_CAP_S, rname);
                if (frc < 0) {
                    printf("abort %ld\n", frc);
                    continue;
                }
                write(1, "done file=", 10);
                while (rname[wi])
                    write(1, &rname[wi++], 1);
                write(1, " bytes=", 7);
                print_u64((unsigned long)frc);
                write(1, " verified\n", 10);
            }
            continue;
        }
        printf("ramdisk pid: ");
        if (read_line(line, sizeof(line)) < 0)
            return 1;
        g_ramdisk_pid = x_atoul(line);
        if (g_ramdisk_pid == 0) {
            printf("bad pid\n");
            continue;
        }
        printf("start block: ");
        if (read_line(line, sizeof(line)) < 0)
            return 1;
        start = x_atoul(line);
        if (start >= RAMDISK_BLOCKS) {
            printf("bad block\n");
            continue;
        }
        if (is_rx) {
            long rc = rx_run(start, x_now() + OVERALL_CAP_S);
            if (rc < 0) {
                printf("abort %ld\n", rc);
                continue;
            }
            write(1, "done bytes=", 11);
            print_u64((unsigned long)rc);
            write(1, " blocks=", 8);
            print_u64(((unsigned long)rc + RAMDISK_BLOCK_SIZE - 1) /
                      RAMDISK_BLOCK_SIZE);
            write(1, "\n", 1);
            continue;
        }
        printf("bytes: ");
        if (read_line(line, sizeof(line)) < 0)
            return 1;
        {
            unsigned long n = x_atoul(line);
            int r = dump_run(start, n);
            printf("\ndump done\n");
            if (r != 0)
                return 3;
        }
    }
}
