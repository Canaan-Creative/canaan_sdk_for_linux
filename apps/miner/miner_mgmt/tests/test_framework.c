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

#include "framework.h"
#include <stdio.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

static int a_runs, b_runs; /* eval_a() and eval_b() call counts */
/* Run order, one letter per evaluation ('a' or 'b'); used entries */
static char order[64];
static int  order_n;
static void eval_a(void *ctx, uint64_t now)
{
    (void)ctx;
    (void)now;
    a_runs++;
    order[order_n++] = 'a';
}
static void eval_b(void *ctx, uint64_t now)
{
    (void)ctx;
    (void)now;
    b_runs++;
    order[order_n++] = 'b';
}

int main(void)
{
    ctl_reset();
    a_runs = b_runs = order_n = 0;

    ctl_rule_t ra = { .name = "a", .cadence_ms = 1000, .evaluate = eval_a };
    ctl_rule_t rb = { .name = "b", .cadence_ms = 50, .evaluate = eval_b };
    CHECK(ctl_register(&ra) == 0 && ctl_register(&rb) == 0, "register two rules");
    CHECK(ra.next_due_ms == 0, "registered rule due now");

    ctl_rule_t bad = { .name = NULL, .cadence_ms = 0, .evaluate = eval_a };
    CHECK(ctl_register(&bad) == -1, "rule without name rejected");
    ctl_rule_t noeval = { .name = "x", .cadence_ms = 0, .evaluate = NULL };
    CHECK(ctl_register(&noeval) == -1, "rule without evaluate rejected");

    int ran = ctl_tick(0);
    CHECK(ran == 2, "first tick runs both");
    CHECK(order_n == 2 && order[0] == 'a' && order[1] == 'b', "registration order preserved");

    a_runs = b_runs = 0;
    ran             = ctl_tick(50);
    CHECK(ran == 1 && b_runs == 1 && a_runs == 0, "only the due-cadence rule runs (b@50)");

    a_runs = b_runs = 0;
    ctl_tick(1000);
    CHECK(a_runs == 1 && b_runs == 1, "both due at t=1000");

    ctl_reset();
    CHECK(ctl_tick(9999) == 0, "no rules after reset");

    if (fails) {
        fprintf(stderr, "test_framework: %d FAIL\n", fails);
        return 1;
    }
    printf("test_framework: all pass\n");
    return 0;
}
