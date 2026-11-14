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

#include "mc_api_addpool.h"
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

int main(void)
{
    mc_addpool_req r;

    CHECK(mc_api_addpool_parse("addpool|stratum+tcp://h:3333,worker,x", &r) == MC_ADDPOOL_OK &&
                    !strcmp(r.url, "stratum+tcp://h:3333") && !strcmp(r.user, "worker") &&
                    !strcmp(r.pass, "x"),
            "text url,user,pass split");

    CHECK(mc_api_addpool_parse("addpool|url,,", &r) == MC_ADDPOOL_OK && !strcmp(r.url, "url") &&
                    r.user[0] == '\0' && r.pass[0] == '\0',
            "empty user+pass ok");

    CHECK(mc_api_addpool_parse("addpool| u,us,pw", &r) == MC_ADDPOOL_OK && !strcmp(r.url, "u"),
            "leading space skipped");

    CHECK(mc_api_addpool_parse("{\"command\":\"addpool\",\"parameter\":\"u2,us2,pw2\"}", &r) ==
                            MC_ADDPOOL_OK &&
                    !strcmp(r.url, "u2") && !strcmp(r.user, "us2") && !strcmp(r.pass, "pw2"),
            "json parameter split");

    CHECK(mc_api_addpool_parse("addpool|u,us,a,b,c", &r) == MC_ADDPOOL_OK &&
                    !strcmp(r.pass, "a,b,c"),
            "pass keeps embedded commas");

    CHECK(mc_api_addpool_parse("addpool", &r) == MC_ADDPOOL_MISSING, "no '|' -> missing");
    CHECK(mc_api_addpool_parse("addpool|", &r) == MC_ADDPOOL_MISSING, "empty payload -> missing");
    CHECK(mc_api_addpool_parse("addpool|   ", &r) == MC_ADDPOOL_MISSING, "spaces-only -> missing");

    CHECK(mc_api_addpool_parse("addpool|justurl", &r) == MC_ADDPOOL_INVALID,
            "no commas -> invalid");
    CHECK(mc_api_addpool_parse("addpool|url,user", &r) == MC_ADDPOOL_INVALID,
            "one comma -> invalid");
    CHECK(mc_api_addpool_parse("addpool|,user,pass", &r) == MC_ADDPOOL_INVALID,
            "empty url -> invalid");

    char big[64 + POOL_URL_MAX];
    int  k = sprintf(big, "addpool|");
    for (int i = 0; i < POOL_URL_MAX + 5; i++)
        big[k++] = 'a';
    strcpy(big + k, ",u,p");
    CHECK(mc_api_addpool_parse(big, &r) == MC_ADDPOOL_INVALID, "oversized url -> invalid");

    CHECK(mc_api_addpool_parse(NULL, &r) == MC_ADDPOOL_MISSING, "NULL req -> missing");

    if (fails) {
        fprintf(stderr, "test_addpool: %d FAIL\n", fails);
        return 1;
    }
    printf("test_addpool: all pass\n");
    return 0;
}
