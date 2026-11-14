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

#include "harness_todo.h"
#include "harness_fixture.h"

int main(int argc, char **argv)
{
    const char *id = argc > 1 ? argv[1] : "";

    if (!strcmp(id, "config_pools_conf_json")) {
        hf_fails = 0;
        char *c  = hf_read("config/ref_pools.conf");
        HF_CHECK(c != NULL, "pool config fixture present");
        if (c) {
            HF_CHECK(c[0] == '{', "top level is a JSON object");
            HF_CHECK(hf_count(c, "{") == hf_count(c, "}"), "braces balanced");
            HF_CHECK(strstr(c, "\"pools\"") != NULL, "has a pools object");
            HF_CHECK(strstr(c, "\"standard\"") != NULL, "has a standard args string");
            HF_CHECK(strstr(c, "--lowmem") != NULL && strstr(c, "--real-quiet") != NULL,
                    "standard carries the expected flags");
        }
        free(c);
        HF_DONE("config_pools_conf_json");
    }

    if (!strcmp(id, "config_pool_list")) {
        hf_fails = 0;
        char *c  = hf_read("config/ref_pools.conf");
        HF_CHECK(c != NULL, "pool config fixture present");
        if (c) {
            int np = hf_count(c, "\"url\":");
            HF_CHECK(strstr(c, "\"pools\"") != NULL, "has pools container");
            HF_CHECK(np >= 1 && np <= 3, "poolcfg lists %d pool(s), <=3 (POOL_VALID_NUM)", np);
            HF_CHECK(strstr(c, "\"pool0\"") != NULL, "pools are 0-indexed (pool0 present)");
            HF_CHECK(strstr(c, "\"pool3\"") == NULL, "no pool3 (max 3, indices 0..2)");
            HF_CHECK(hf_count(c, "\"user\":") == np, "every pool has a user");
            HF_CHECK(hf_count(c, "\"pass\":") == np, "every pool has a pass");
            HF_CHECK(hf_count(c, "stratum+tcp://") >= np, "pool urls use the stratum+tcp scheme");
        }
        free(c);
        HF_DONE("config_pool_list");
    }

    HARNESS_UNKNOWN(id);
}
