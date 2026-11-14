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

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <linux/input.h>

#include "osif.h"
#include "btn_input.c"
#include "../../tests/include/tst_check.h"

/* Captured callbacks: key code, edge (input_event value), timestamp in ms */
static struct {
    int code, edge, t_ms;
} cap[64];
static int cap_n;

uint64_t osif_now_ms(void)
{
    return 123456u;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}
osif_status_t osif_thread_create(osif_thread_t **t, const char *name, osif_thread_fn fn, void *arg,
        size_t stack_bytes)
{
    (void)t;
    (void)name;
    (void)fn;
    (void)arg;
    (void)stack_bytes;
    return OSIF_OK;
}
osif_status_t osif_thread_join(osif_thread_t *t, void **ret)
{
    (void)t;
    (void)ret;
    return OSIF_OK;
}

static void cap_cb(int code, int edge, uint32_t t_ms, void *arg)
{
    (void)arg;
    if (cap_n < 64) {
        cap[cap_n].code = code;
        cap[cap_n].edge = edge;
        cap[cap_n].t_ms = (int)t_ms;
        cap_n++;
    }
}

static void put_ev(int wfd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ie;
    memset(&ie, 0, sizeof ie);
    ie.type  = type;
    ie.code  = code;
    ie.value = value;
    (void)!write(wfd, &ie, sizeof ie);
}

int main(void)
{
    CHECK(is_tracked(105) && is_tracked(103) && is_tracked(116), "tracked: TP1/TP2/POWER");
    CHECK(!is_tracked(42) && !is_tracked(0), "untracked codes rejected");

    CHECK(node_qualifies(-1, "nano3s gpio-keys") == 1, "name 'gpio-keys' qualifies");
    CHECK(node_qualifies(-1, "gpio_keys") == 1, "name 'gpio_keys' qualifies");
    CHECK(node_qualifies(-1, "usb-keyboard") == 0, "other name + bad fd -> no");
    CHECK(node_qualifies(-1, NULL) == 0, "no name + bad fd -> no");

    g.cb  = cap_cb;
    g.arg = NULL;
    cap_n = 0;
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe created");
    put_ev(fds[1], EV_KEY, 105, 1);
    put_ev(fds[1], EV_KEY, 105, 0);
    put_ev(fds[1], EV_KEY, 103, 1);
    put_ev(fds[1], EV_KEY, 116, 1);
    put_ev(fds[1], EV_KEY, 105, 2);
    put_ev(fds[1], EV_REL, 0, 1);
    put_ev(fds[1], EV_KEY, 42, 1);
    close(fds[1]);
    int rc = drain_fd(fds[0]);
    close(fds[0]);

    CHECK(rc == 0, "drain_fd returns 0 at EOF (device gone)");
    CHECK(cap_n == 4, "exactly 4 edges forwarded (dropped repeat/rel/untracked)");
    CHECK(cap[0].code == 105 && cap[0].edge == 1, "edge0: TP1 down");
    CHECK(cap[1].code == 105 && cap[1].edge == 0, "edge1: TP1 up");
    CHECK(cap[2].code == 103 && cap[2].edge == 1, "edge2: TP2 down");
    CHECK(cap[3].code == 116 && cap[3].edge == 1, "edge3: POWER down");
    CHECK(cap[0].t_ms == 123456, "edge stamped with osif_now_ms");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
