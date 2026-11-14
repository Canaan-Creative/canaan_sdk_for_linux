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

#include "mc_api_poolid.h"
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
    int id;

    id = -99;
    CHECK(mc_api_poolid_parse("enablepool|0", 0, &id) == MC_POOLID_OK && id == 0,
            "text enablepool|0 -> 0");
    id = -99;
    CHECK(mc_api_poolid_parse("disablepool|2", 0, &id) == MC_POOLID_OK && id == 2,
            "text disablepool|2 -> 2");
    id = -99;
    CHECK(mc_api_poolid_parse("enablepool| 1", 0, &id) == MC_POOLID_OK && id == 1,
            "leading space skipped -> 1");
    id = -99;
    CHECK(mc_api_poolid_parse("switchpool|1{", 0, &id) == MC_POOLID_OK && id == 1,
            "text switchpool|1{ -> 1");

    id = -99;
    CHECK(mc_api_poolid_parse("{\"command\":\"enablepool\",\"parameter\":\"1\"}", 1, &id) ==
                            MC_POOLID_OK &&
                    id == 1,
            "json parameter \"1\" -> 1");

    id = -99;
    CHECK(mc_api_poolid_parse("enablepool|9", 0, &id) == MC_POOLID_OK && id == 9,
            "oob id 9 parses (range is caller's job)");
    id = -99;
    CHECK(mc_api_poolid_parse("disablepool|-1", 0, &id) == MC_POOLID_OK && id == -1,
            "negative id parses (range is caller's job)");

    CHECK(mc_api_poolid_parse("enablepool", 0, &id) == MC_POOLID_MISSING,
            "no '|' and no json -> missing");
    CHECK(mc_api_poolid_parse("enablepool|", 0, &id) == MC_POOLID_MISSING,
            "empty after '|' -> missing");
    id = -99;
    CHECK(mc_api_poolid_parse("enablepool|abc", 0, &id) == MC_POOLID_OK && id == 0,
            "non-numeric after '|' -> atoi 0");
    id = -99;
    CHECK(mc_api_poolid_parse("enablepool| ", 0, &id) == MC_POOLID_OK && id == 0,
            "space-only after '|' -> atoi 0");
    id = -99;
    CHECK(mc_api_poolid_parse("enablepool|\n", 0, &id) == MC_POOLID_OK && id == 0,
            "newline-only after '|' -> atoi 0");
    id = -99;
    CHECK(mc_api_poolid_parse("enablepool|2abc", 0, &id) == MC_POOLID_OK && id == 2,
            "trailing garbage ignored -> 2");
    id = -99;
    CHECK(mc_api_poolid_parse("{\"command\":\"enablepool\",\"parameter\":2}", 1, &id) ==
                            MC_POOLID_OK &&
                    id == 2,
            "json integer parameter 2 -> 2");
    id = -99;
    CHECK(mc_api_poolid_parse("{\"command\":\"enablepool\",\"parameter\":\"x\"}", 1, &id) ==
                            MC_POOLID_OK &&
                    id == 0,
            "json non-numeric string -> atoi 0");
    id = -99;
    CHECK(mc_api_poolid_parse("{\"command\":\"enablepool\",\"parameter\":\"1|2\"}", 1, &id) ==
                            MC_POOLID_OK &&
                    id == 1,
            "json value containing '|' -> 1");
    CHECK(mc_api_poolid_parse("{\"command\":\"enablepool\",\"parameter\":\"\"}", 1, &id) ==
                    MC_POOLID_MISSING,
            "json empty string -> missing");
    CHECK(mc_api_poolid_parse("{\"command\":\"enablepool\",\"parameter\":null}", 1, &id) ==
                    MC_POOLID_MISSING,
            "json null -> missing");
    CHECK(mc_api_poolid_parse("{\"command\":\"enablepool\"}", 1, &id) == MC_POOLID_MISSING,
            "json without parameter -> missing");
    CHECK(mc_api_poolid_parse(NULL, 0, &id) == MC_POOLID_MISSING, "NULL req -> missing");
    CHECK(mc_api_poolid_parse("enablepool|0", 0, NULL) == MC_POOLID_MISSING, "NULL out -> missing");

    if (fails) {
        fprintf(stderr, "test_poolid: %d FAIL\n", fails);
        return 1;
    }
    printf("test_poolid: all pass\n");
    return 0;
}
