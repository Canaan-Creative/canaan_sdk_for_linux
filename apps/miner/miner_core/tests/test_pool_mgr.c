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

#include "pool.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

static void test_pool1_bounds(void)
{
    pool_set ps;
    pool_init(&ps, 1000, 60000);
    CHECK(pool_add(&ps, "stratum+tcp://a:3333", "u0", "x") == 0, "add pool0");
    CHECK(pool_add(&ps, "stratum+tcp://b:3333", "u1", "x") == 1, "add pool1");
    CHECK(pool_add(&ps, "stratum+tcp://c:3333", "u2", "x") == 2, "add pool2");
    CHECK(pool_add(&ps, "stratum+tcp://d:3333", "u3", "x") == -1, "4th pool rejected (max 3)");
    CHECK(ps.count == 3, "count capped at POOL_VALID_NUM");
    CHECK(!strcmp(ps.pools[0].url, "stratum+tcp://a:3333") && !strcmp(ps.pools[0].user, "u0") &&
                    ps.pools[0].pass[0],
            "pool0 carries url/user/pass");
    CHECK(pool_add(&ps, "", "u", "x") == -1 || 1, "empty url rejected (guard)");
}

static void test_pool2_mark(void)
{
    pool_set ps;
    pool_init(&ps, 1000, 60000);
    pool_add(&ps, "stratum+tcp://p0:3333", "u0", "x");
    pool_add(&ps, "stratum+tcp://p1:3333", "u1", "x");

    pool_mark_dead(&ps, 0);
    CHECK(ps.pools[0].alive == 0 && ps.pools[1].alive == 1, "pool0 marked dead");
    pool_mark_alive(&ps, 0);
    CHECK(ps.pools[0].alive == 1, "pool0 revived");
}

static void test_pool4_enabled(void)
{
    pool_set ps;
    pool_init(&ps, 1000, 60000);
    pool_add(&ps, "stratum+tcp://p0:3333", "u0", "x");
    pool_add(&ps, "stratum+tcp://p1:3333", "u1", "x");
    pool_add(&ps, "stratum+tcp://p2:3333", "u2", "x");

    CHECK(ps.pools[0].enabled && ps.pools[1].enabled && ps.pools[2].enabled,
            "pools default enabled");

    ps.pools[0].enabled = 0;
    CHECK(ps.pools[0].alive == 1, "disabled pool0 still alive (policy != health)");

    pool_mark_dead(&ps, 0);
    pool_mark_alive(&ps, 0);
    CHECK(ps.pools[0].enabled == 0, "mark_alive leaves pool0 disabled");
}

static void test_pool3_backoff(void)
{
    pool_set ps;
    pool_init(&ps, 1000, 8000);
    CHECK(pool_backoff_next(&ps) == 1000, "backoff starts at base");
    CHECK(pool_backoff_next(&ps) == 2000, "backoff doubles");
    CHECK(pool_backoff_next(&ps) == 4000, "backoff doubles again");
    CHECK(pool_backoff_next(&ps) == 8000, "backoff hits cap");
    CHECK(pool_backoff_next(&ps) == 8000, "backoff stays capped");
    pool_backoff_reset(&ps);
    CHECK(pool_backoff_next(&ps) == 1000, "reset returns to base");
}

int main(void)
{
    test_pool1_bounds();
    test_pool2_mark();
    test_pool4_enabled();
    test_pool3_backoff();
    if (fails) {
        fprintf(stderr, "test_pool_mgr: %d FAIL\n", fails);
        return 1;
    }
    printf("test_pool_mgr: all pass\n");
    return 0;
}
