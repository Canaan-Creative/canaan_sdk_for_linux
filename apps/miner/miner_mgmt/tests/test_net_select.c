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

#include "net_select.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c, ...) \
    do { \
        if (c) { \
            printf("  ok: "); \
        } else { \
            fails++; \
            printf("  FAIL: "); \
        } \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } while (0)

static int fails;

int main(void)
{
    net_select_t st;

    memset(&st, 0, sizeof st);
    CHECK(net_select_eval(&st, 0, 0) == NET_SEL_NONE,
            "no wired link and no client is the wifi-dongle case and asks for nothing");
    CHECK(!net_select_wired_up(&st),
            "and the device type stays whatever the descriptor's primary says");

    CHECK(net_select_eval(&st, 1, 0) == NET_SEL_DHCP_START,
            "a wired link appearing with no client starts one");
    CHECK(net_select_wired_up(&st),
            "the device type reports wired on the LINK, before any lease exists");

    CHECK(net_select_eval(&st, 1, 1) == NET_SEL_NONE,
            "link with the client up is steady: edge-triggered, so the caller forks "
            "once per transition and not once per 2s tick");
    CHECK(net_select_eval(&st, 1, 1) == NET_SEL_NONE, "and stays steady on the tick after that");

    CHECK(net_select_eval(&st, 1, 0) == NET_SEL_DHCP_START,
            "a client that died under a live link is a transition, so it is "
            "restarted rather than left dead until the cable moves");

    CHECK(net_select_eval(&st, 0, 1) == NET_SEL_DHCP_STOP,
            "the link going with the client up stops it — a dongle pulled at "
            "runtime, which this hardware allows at any moment");
    CHECK(!net_select_wired_up(&st), "and the device type stops claiming wired");

    CHECK(net_select_eval(&st, 0, 0) == NET_SEL_NONE, "gone and stopped is steady");

    memset(&st, 0, sizeof st);
    CHECK(net_select_eval(&st, 1, 1) == NET_SEL_NONE,
            "a client already running at the first evaluation is adopted, not "
            "restarted: mm_miner restarting must not disturb a live lease");
    CHECK(net_select_wired_up(&st), "and the adopted state still reports wired");

    CHECK(net_select_eval(NULL, 1, 0) == NET_SEL_NONE, "NULL state asks for nothing");
    CHECK(!net_select_wired_up(NULL), "NULL state does not report wired");

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
