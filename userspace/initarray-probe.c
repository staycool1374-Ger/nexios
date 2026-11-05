/*
 * NexIOS RTOS — init_array verification probe (issue #455 item 3).
 * Two C constructors (constructor attribute => .init_array entries)
 * arm observable state before main; main pins that crt0's step-4b walk
 * ran them exactly once, in a single boot, with working errno (TLS_SET
 * precedes the walk). Marker lines anticipate the testbed probe class.
 */

#include <stdio.h>
#include <errno.h>

static int g_ctor_alpha_runs;
static int g_ctor_beta_runs;
static int g_ctor_errno_seen;

__attribute__((constructor)) static void ctor_alpha(void) {
    ++g_ctor_alpha_runs;
}

__attribute__((constructor)) static void ctor_beta(void) {
    ++g_ctor_beta_runs;
    errno = 0;
    if (errno == 0) {
        g_ctor_errno_seen = 1;
    }
}

int main(void) {
    int fails = 0;
    printf("INITARRAY_VERIFY: alpha %d\n", g_ctor_alpha_runs);
    printf("INITARRAY_VERIFY: beta %d\n", g_ctor_beta_runs);
    printf("INITARRAY_VERIFY: errno %d\n", g_ctor_errno_seen);
    if (g_ctor_alpha_runs != 1) {
        printf("INITARRAY_VERIFY: alpha FAIL\n");
        ++fails;
    }
    if (g_ctor_beta_runs != 1) {
        printf("INITARRAY_VERIFY: beta FAIL\n");
        ++fails;
    }
    if (g_ctor_errno_seen != 1) {
        printf("INITARRAY_VERIFY: errno FAIL\n");
        ++fails;
    }
    if (fails == 0) {
        printf("INITARRAY_VERIFY: PASS\n");
    } else {
        printf("INITARRAY_VERIFY: FAIL %d\n", fails);
    }
    return fails;
}
