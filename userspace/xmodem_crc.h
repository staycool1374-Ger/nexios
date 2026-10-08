/* NexIOS RTOS — XMODEM CRC-16 (issue #276).
 *
 * Freestanding plain-C CRC-16/CCITT-FALSE shared by the userspace
 * receiver (userspace/xmodem.c) and the kernel lib tests
 * (src/kernel/test/test_lib.cpp). Zero includes and built-in types
 * only: neither src/libc nor src/lib provides stdint.h, so this header
 * spells `unsigned char` / `unsigned short` throughout and compiles as
 * both C and C++.
 *
 * Variant: poly 0x1021, init 0x0000, no xor-out, no reflection.
 * Check vector: "123456789" -> 0x31C3. Empty input -> 0x0000.
 * The accumulator is masked to 16 bits after every step so the vector
 * holds on hosts where `unsigned short` is wider than 16 bits.
 */

#ifndef XMODEM_CRC_H
#define XMODEM_CRC_H

#ifdef __cplusplus
extern "C" {
#endif

#define XMODEM_CRC_POLY 0x1021u
#define XMODEM_CRC_INIT 0x0000u
#define XMODEM_CRC_CHECK 0x31C3u

static unsigned short xmodem_crc16_table[256];
static int xmodem_crc16_table_ready = 0;

static void xmodem_crc16_init_table(void) {
    unsigned int i = 0;
    unsigned int j = 0;
    if (xmodem_crc16_table_ready)
        return;
    for (i = 0; i < 256; ++i) {
        unsigned int crc = i << 8;
        for (j = 0; j < 8; ++j) {
            if (crc & 0x8000u)
                crc = ((crc << 1) ^ XMODEM_CRC_POLY) & 0xFFFFu;
            else
                crc = (crc << 1) & 0xFFFFu;
        }
        xmodem_crc16_table[i] = (unsigned short)(crc & 0xFFFFu);
    }
    xmodem_crc16_table_ready = 1;
}

static unsigned short xmodem_crc16_update(unsigned short crc,
                                          const unsigned char *data,
                                          unsigned int len) {
    unsigned int i = 0;
    xmodem_crc16_init_table();
    crc &= 0xFFFFu;
    for (i = 0; i < len; ++i) {
        unsigned int idx =
            ((crc >> 8) ^ (unsigned int)data[i]) & 0xFFu;
        crc = (((crc << 8) ^ xmodem_crc16_table[idx])) & 0xFFFFu;
    }
    return (unsigned short)(crc & 0xFFFFu);
}

static unsigned short xmodem_crc16(const unsigned char *data,
                                   unsigned int len) {
    return xmodem_crc16_update(XMODEM_CRC_INIT, data, len);
}

#ifdef __cplusplus
}
#endif

#endif /* XMODEM_CRC_H */
