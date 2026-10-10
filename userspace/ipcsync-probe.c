/*
 * NexIOS RTOS — ipc_send_sync reply-buffer contract probe (issue #317 option b)
 *
 * Sends one request to PID 1 (the test harness, which acts as the responder)
 * with a DISTINCT reply buffer and asserts:
 *   (a) the reply TYPE is returned by ipc_send_sync;
 *   (b) the reply PAYLOAD lands in the caller's reply_buf;
 *   (c) the REQUEST buffer is NOT overwritten by the reply.
 * Exit 0 = contract holds; 3/4/5 = the corresponding failure.
 */

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ipc.h>

#define REQ_TYPE 0x55u
#define REP_TYPE 0x77u
#define REQ_LEN 40u
#define REP_LEN 8u

int main(void) {
    unsigned char req[REQ_LEN];
    unsigned char reply[REQ_LEN];
    unsigned int i;

    for (i = 0; i < REQ_LEN; ++i)
        req[i] = (unsigned char)(0x10 + i);
    memset(reply, 0xEE, sizeof(reply));

    int r = ipc_send_sync(1, REQ_TYPE, req, REQ_LEN, reply);
    if (r != (int)REP_TYPE)
        _exit(3);

    /* (b) reply payload in reply_buf */
    for (i = 0; i < REP_LEN; ++i)
        if (reply[i] != (unsigned char)(0xA0 + i))
            _exit(4);

    /* (c) request buffer intact (reply must NOT overwrite it) */
    for (i = 0; i < REQ_LEN; ++i)
        if (req[i] != (unsigned char)(0x10 + i))
            _exit(5);

    _exit(0);
}
