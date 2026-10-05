/*
 * NexIOS RTOS — debugd Phase 5 (issue #295)
 *
 * Host conformance tests for the shared C protocol core
 * (userspace/debugd_proto.h): the same wire decisions the target loop
 * in userspace/debugd.c executes. Assert-based, zero dependencies,
 * return-code gate. Compiled with the host C compiler, NOT part of the
 * kernel build.
 */

#include <stdio.h>
#include <string.h>

#include "debugd_proto.h"

#include "debugd_proto.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
        } else {                                                               \
            ++g_fail;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

static void test_checksum(void) {
    /* "$?#3f": '?' = 0x3F. */
    char out[2] = {0, 0};

    dbg_rsp_checksum("?", 1, out);
    CHECK(out[0] == '3' && out[1] == 'f');
    /* "$g#67": 'g' = 0x67. */
    dbg_rsp_checksum("g", 1, out);
    CHECK(out[0] == '6' && out[1] == '7');
    /* Empty body checksums to 00. */
    dbg_rsp_checksum("", 0, out);
    CHECK(out[0] == '0' && out[1] == '0');
    /* Multi-byte: "m0,10" = 6D+30+2C+31+30 = 0x12A -> 2A. */
    dbg_rsp_checksum("m0,10", 5, out);
    CHECK(out[0] == '2' && out[1] == 'a');
}

static void test_hex_codec(void) {
    unsigned char buf[8] = {0};
    char text[32] = {0};

    CHECK(dbg_hex_val('0') == 0);
    CHECK(dbg_hex_val('9') == 9);
    CHECK(dbg_hex_val('a') == 10);
    CHECK(dbg_hex_val('F') == 15);
    CHECK(dbg_hex_val('g') == 16);
    CHECK(dbg_hex_val('#') == 16);

    buf[0] = 0xDE;
    buf[1] = 0xAD;
    dbg_hex_encode(buf, 2, text);
    CHECK(text[0] == 'd' && text[1] == 'e' && text[2] == 'a' &&
          text[3] == 'd' && text[4] == '\0');

    memset(buf, 0, sizeof(buf));
    CHECK(dbg_hex_decode("dead", 4, buf, sizeof(buf)) == 2);
    CHECK(buf[0] == 0xDE && buf[1] == 0xAD);
    /* Odd length rejected. */
    CHECK(dbg_hex_decode("abc", 3, buf, sizeof(buf)) ==
          (unsigned long)-1);
    /* Bad digits rejected. */
    CHECK(dbg_hex_decode("zz", 2, buf, sizeof(buf)) == (unsigned long)-1);
    /* Cap enforced. */
    CHECK(dbg_hex_decode("dead", 4, buf, 1) == (unsigned long)-1);
}

static void test_addr_len(void) {
    unsigned long addr = 0;
    unsigned long len = 0;

    CHECK(dbg_parse_addr_len("41000000,10", &addr, &len) == 0);
    CHECK(addr == 0x41000000UL && len == 0x10);
    CHECK(dbg_parse_addr_len("0,0", &addr, &len) == 0);
    CHECK(addr == 0 && len == 0);
    /* Missing comma rejected. */
    CHECK(dbg_parse_addr_len("41000000", &addr, &len) != 0);
    /* Bad hex rejected. */
    CHECK(dbg_parse_addr_len("zz,10", &addr, &len) != 0);
    CHECK(dbg_parse_addr_len("10,zz", &addr, &len) != 0);
}

static void test_stop_reply(void) {
    char out[32] = {0};
    unsigned long n = 0;

    n = dbg_stop_reply_for_kind(DBG_STOP_BREAKPOINT, out);
    CHECK(n == 12);
    CHECK(strncmp(out, "T05thread:1;", 12) == 0);
    n = dbg_stop_reply_for_kind(DBG_STOP_STEP, out);
    CHECK(strncmp(out, "T05thread:1;", 12) == 0);
    n = dbg_stop_reply_for_kind(DBG_STOP_FAULT, out);
    CHECK(strncmp(out, "T0bthread:1;", 12) == 0);
    n = dbg_stop_reply_for_kind(DBG_STOP_DEATH, out);
    CHECK(n == 3 && strncmp(out, "W00", 3) == 0);
    /* Unknown kinds fall back to TRAP (never empty, never garbage). */
    n = dbg_stop_reply_for_kind(99, out);
    CHECK(strncmp(out, "T05thread:1;", 12) == 0);
}

int main(void) {
    test_checksum();
    test_hex_codec();
    test_addr_len();
    test_stop_reply();
    printf("cproto: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
