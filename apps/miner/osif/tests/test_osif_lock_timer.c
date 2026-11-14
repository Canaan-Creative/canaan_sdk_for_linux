// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026, Canaan Creative Co., Ltd.
 *
 * For the licensing terms, see the LICENSE file (MIT License).
 *
 * NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability, fitness for a
 * particular purpose and non-infringement. In no event shall the authors or copyright
 * holders be liable for any claim, damages or other liability arising from, out of or in
 * connection with the Software or the use or other dealings in the Software.
 */

#include "osif.h"
#include <stdio.h>

#include "../../tests/include/tst_check.h"

#define LOCK_ROUNDS 20000

static osif_lock_t *g_lock;
static volatile int g_count;

static void *lock_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOCK_ROUNDS; i++) {
        osif_lock_acquire(g_lock);
        int v = g_count;
        if (!(i & 255))
            osif_sleep_ms(0);
        g_count = v + 1;
        osif_lock_release(g_lock);
    }
    return NULL;
}

int main(void)
{
    osif_lock_t *l = NULL;
    CHECK(osif_lock_create(&l) == OSIF_OK && l, "lock create");
    g_lock            = l;
    osif_thread_t *ta = NULL, *tb = NULL;
    int started = l && osif_thread_create(&ta, "lock-a", lock_worker, NULL, 0) == OSIF_OK &&
            osif_thread_create(&tb, "lock-b", lock_worker, NULL, 0) == OSIF_OK;
    if (ta)
        osif_thread_join(ta, NULL);
    if (tb)
        osif_thread_join(tb, NULL);
    CHECK(started && g_count == 2 * LOCK_ROUNDS,
            "two threads incrementing under the lock lose no update (%d)", g_count);

    uint64_t t0 = osif_now_ms();
    osif_sleep_ms(20);
    uint64_t t1 = osif_now_ms();
    CHECK(t1 >= t0, "clock is monotonic non-decreasing");
    CHECK(t1 - t0 >= 15, "sleep_ms(20) advanced the clock >= ~15 ms (%llu)",
            (unsigned long long)(t1 - t0));

    if (fails) {
        fprintf(stderr, "test_osif_lock_timer: %d FAIL\n", fails);
        return 1;
    }
    printf("test_osif_lock_timer: all pass\n");
    return 0;
}
