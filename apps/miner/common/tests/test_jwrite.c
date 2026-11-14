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

#include "jwrite.h"
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
    char b[512];

    jwriter w;
    jw_init(&w, b, sizeof b);
    jw_obj_open(&w);
    jw_kv_str(&w, "a", "1");
    jw_kv_str(&w, "b", "2");
    jw_obj_close(&w);
    CHECK(!strcmp(b, "{\"a\":\"1\",\"b\":\"2\"}"), "flat object");
    CHECK(jw_ok(&w), "flat: jw_ok");

    jw_init(&w, b, sizeof b);
    jw_obj_open(&w);
    jw_kv_raw(&w, "power", "132");
    jw_key_obj(&w, "pool1");
    jw_kv_str(&w, "url", "stratum+tcp://x:3333");
    jw_obj_close(&w);
    jw_obj_close(&w);
    CHECK(!strcmp(b, "{\"power\":132,\"pool1\":{\"url\":\"stratum+tcp://x:3333\"}}"),
            "nested object + raw number");

    jw_init(&w, b, sizeof b);
    jw_obj_open(&w);
    jw_kv_str(&w, "q", "a\"b\\c");
    jw_obj_close(&w);
    CHECK(!strcmp(b, "{\"q\":\"a\\\"b\\\\c\"}"), "quote/backslash escaped");

    char small[8];
    jw_init(&w, small, sizeof small);
    jw_obj_open(&w);
    jw_kv_str(&w, "key", "value");
    jw_obj_close(&w);
    CHECK(!jw_ok(&w), "truncation detected (jw_ok false)");
    CHECK(jw_len(&w) >= sizeof small, "jw_len reports full length");
    CHECK(small[7] == '\0', "buffer stays NUL-terminated on truncation");

    if (fails) {
        fprintf(stderr, "test_jwrite: %d FAIL\n", fails);
        return 1;
    }
    printf("test_jwrite: all pass\n");
    return 0;
}
