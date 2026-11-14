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

#include "mc_api_poollist.h"
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

int main(void)
{
    mc_poollist_req r;

    CHECK(mc_api_poollist_parse("poolpriority|2,0,1", 0, &r) == MC_POOLLIST_OK && r.n == 3 &&
                    r.ids[0] == 2 && r.ids[1] == 0 && r.ids[2] == 1,
            "text 2,0,1 -> [2,0,1]");

    CHECK(mc_api_poollist_parse("poolpriority|1", 0, &r) == MC_POOLLIST_OK && r.n == 1 &&
                    r.ids[0] == 1,
            "single id -> [1]");

    CHECK(mc_api_poollist_parse("poolpriority| 0 , 1 ", 0, &r) == MC_POOLLIST_OK && r.n == 2 &&
                    r.ids[0] == 0 && r.ids[1] == 1,
            "spaces skipped -> [0,1]");
    CHECK(mc_api_poollist_parse("poolpriority|1{", 0, &r) == MC_POOLLIST_OK && r.n == 1 &&
                    r.ids[0] == 1,
            "text poolpriority|1{ -> [1]");

    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\":\"1,0\"}", 1, &r) ==
                            MC_POOLLIST_OK &&
                    r.n == 2 && r.ids[0] == 1 && r.ids[1] == 0,
            "json 1,0 -> [1,0]");

    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\":\"2\"}", 1, &r) ==
                            MC_POOLLIST_OK &&
                    r.n == 1 && r.ids[0] == 2,
            "json string \"2\" -> [2]");
    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\":1}", 1, &r) ==
                            MC_POOLLIST_OK &&
                    r.n == 1 && r.ids[0] == 1,
            "json integer 1 -> [1]");
    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\": -1}", 1, &r) ==
                            MC_POOLLIST_OK &&
                    r.n == 1 && r.ids[0] == -1,
            "json integer -1 -> [-1]");
    CHECK(mc_api_poollist_parse("{\"parameter\":0, \"command\":\"poolpriority\"}", 1, &r) ==
                            MC_POOLLIST_OK &&
                    r.n == 1 && r.ids[0] == 0,
            "json integer before command -> [0]");
    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\":1.5}", 1, &r) ==
                            MC_POOLLIST_OK &&
                    r.n == 1 && r.ids[0] == 1,
            "json real 1.5 -> [1]");
    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\":null}", 1, &r) ==
                    MC_POOLLIST_MISSING,
            "json null -> missing");
    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\":true}", 1, &r) ==
                    MC_POOLLIST_MISSING,
            "json bool -> missing");
    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\",\"parameter\":\"\"}", 1, &r) ==
                    MC_POOLLIST_MISSING,
            "json empty string -> missing");
    CHECK(mc_api_poollist_parse("{\"command\":\"poolpriority\"}", 1, &r) == MC_POOLLIST_MISSING,
            "json no parameter -> missing");

    CHECK(mc_api_poollist_parse("poolpriority|0,1,0", 0, &r) == MC_POOLLIST_OK && r.n == 3 &&
                    r.ids[2] == 0,
            "dup id kept for the handler -> [0,1,0]");

    CHECK(mc_api_poollist_parse("poolpriority|0,1,2,3,4", 0, &r) == MC_POOLLIST_OK &&
                    r.n == MC_POOLLIST_MAX && r.ids[MC_POOLLIST_MAX - 1] == 3,
            "over-long list truncated after one id past the pool limit");

    CHECK(mc_api_poollist_parse("poolpriority|abc", 0, &r) == MC_POOLLIST_OK && r.n == 1 &&
                    r.ids[0] == 0,
            "non-numeric -> atoi 0");
    CHECK(mc_api_poollist_parse("poolpriority|0,x", 0, &r) == MC_POOLLIST_OK && r.n == 2 &&
                    r.ids[1] == 0,
            "2nd token non-numeric -> atoi 0");
    CHECK(mc_api_poollist_parse("poolpriority|1,", 0, &r) == MC_POOLLIST_OK && r.n == 1 &&
                    r.ids[0] == 1,
            "trailing comma ignored -> [1]");
    CHECK(mc_api_poollist_parse("poolpriority|-1,7", 0, &r) == MC_POOLLIST_OK && r.n == 2 &&
                    r.ids[0] == -1 && r.ids[1] == 7,
            "out-of-range ids kept for the handler's range check");

    CHECK(mc_api_poollist_parse("poolpriority", 0, &r) == MC_POOLLIST_MISSING, "no '|' -> missing");
    CHECK(mc_api_poollist_parse("poolpriority|", 0, &r) == MC_POOLLIST_MISSING, "empty -> missing");
    CHECK(mc_api_poollist_parse(NULL, 0, &r) == MC_POOLLIST_MISSING, "NULL -> missing");

    if (fails) {
        fprintf(stderr, "test_poollist: %d FAIL\n", fails);
        return 1;
    }
    printf("test_poollist: all pass\n");
    return 0;
}
