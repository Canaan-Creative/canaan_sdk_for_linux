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

#include "share.h"
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

static void test_dedup(void)
{
    mc_dedup d;
    mc_dedup_init(&d);
    CHECK(mc_dedup_check(&d, "JOB", "0400", "6a4a", "0f2c", NULL) == 0, "first share is fresh");
    CHECK(mc_dedup_check(&d, "JOB", "0400", "6a4a", "0f2c", NULL) == 1,
            "exact repeat is a duplicate");
    CHECK(mc_dedup_check(&d, "JOB", "0400", "6a4a", "0f2d", NULL) == 0, "different nonce is fresh");
    CHECK(mc_dedup_check(&d, "JOB2", "0400", "6a4a", "0f2c", NULL) == 0, "different job is fresh");
    CHECK(mc_dedup_check(&d, "JOB", "0401", "6a4a", "0f2c", NULL) == 0,
            "different xnonce2 is fresh");
    CHECK(mc_dedup_check(&d, "JOB", "0400", "6a4b", "0f2c", NULL) == 0, "different ntime is fresh");
    CHECK(mc_dedup_check(&d, "JOB2", "0400", "6a4a", "0f2c", NULL) == 1,
            "re-submit of JOB2 is a duplicate");
    CHECK(mc_dedup_check(&d, "JOB", "0400", "6a4a", "0f2c", "20000000") == 0,
            "same nonce, different version_bits is a distinct share");
    CHECK(mc_dedup_check(&d, "JOB", "0400", "6a4a", "0f2c", "20000000") == 1,
            "repeat with the same version_bits is a duplicate");

    mc_dedup e;
    mc_dedup_init(&e);
    char n[16];
    for (int i = 0; i < MC_DEDUP_MAX + 50; i++) {
        snprintf(n, sizeof n, "%x", i);
        mc_dedup_check(&e, "J", "x", "t", n, NULL);
    }
    snprintf(n, sizeof n, "%x", MC_DEDUP_MAX + 49);
    CHECK(mc_dedup_check(&e, "J", "x", "t", n, NULL) == 1,
            "recent key still deduped after overflow");
}

static void test_tally(void)
{
    mc_tally t;
    mc_tally_init(&t);
    CHECK(t.accepted == 0 && t.rejected == 0, "tally starts at 0/0");
    mc_tally_record(&t, 1);
    mc_tally_record(&t, 1);
    mc_tally_record(&t, 0);
    CHECK(t.accepted == 2 && t.rejected == 1, "a/r tally counts 2 accepted, 1 rejected");
}

int main(void)
{
    test_dedup();
    test_tally();
    if (fails) {
        fprintf(stderr, "test_share: %d FAIL\n", fails);
        return 1;
    }
    printf("test_share: all pass\n");
    return 0;
}
