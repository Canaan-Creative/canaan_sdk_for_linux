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

#include "jread.h"
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

static void test_scalars(void)
{
    json_value *v;
    v = json_parse("true");
    CHECK(json_bool(v) == 1, "true");
    json_free(v);
    v = json_parse("false");
    CHECK(json_bool(v) == 0 && json_is(v, JSON_BOOL), "false");
    json_free(v);
    v = json_parse("null");
    CHECK(json_is(v, JSON_NULL), "null");
    json_free(v);
    v = json_parse("262144");
    CHECK(json_int(v) == 262144, "number int");
    json_free(v);
    v = json_parse("4294967300");
    CHECK(json_int(v) == 4294967300LL, "number beyond int");
    json_free(v);
    v = json_parse("-9007199254740992");
    CHECK(json_int(v) == -9007199254740992LL, "number neg large");
    json_free(v);
    v = json_parse("1e300");
    CHECK(json_int(v) == 0, "number beyond long long is 0");
    json_free(v);
    v = json_parse("-1e300");
    CHECK(json_int(v) == 0, "number below long long is 0");
    json_free(v);
    v = json_parse("9223372036854775808");
    CHECK(json_int(v) == 0, "2^63 is 0");
    json_free(v);
    v = json_parse("1e400");
    CHECK(json_int(v) == 0, "infinite number is 0");
    json_free(v);
    v = json_parse("-1.5");
    CHECK(json_number(v) == -1.5, "number neg dbl");
    json_free(v);
    v = json_parse("\"ab\\ncd\"");
    CHECK(!strcmp(json_string(v), "ab\ncd"), "string escape");
    json_free(v);
}

static void test_errors(void)
{
    CHECK(json_parse("") == NULL, "empty rejected");
    CHECK(json_parse("{\"a\":}") == NULL, "missing value rejected");
    CHECK(json_parse("[1,2") == NULL, "unterminated array rejected");
    CHECK(json_parse("truex") == NULL, "trailing garbage rejected");
    CHECK(json_parse("{\"a\":1,}") == NULL, "trailing comma rejected");
}

static void test_unicode_escapes(void)
{
    json_value *v;
    v = json_parse("\"\\u00e9\\u4e2d\"");
    CHECK(v && !strcmp(json_string(v), "\xc3\xa9\xe4\xb8\xad"), "BMP escapes");
    json_free(v);
    v = json_parse("\"\\ud83d\\ude00\"");
    CHECK(v && !strcmp(json_string(v), "\xf0\x9f\x98\x80"), "surrogate pair is 4-byte UTF-8");
    json_free(v);
    CHECK(json_parse("\"a\\u0000b\"") == NULL, "\\u0000 rejected");
    CHECK(json_parse("\"\\ud83d\"") == NULL, "lone high surrogate rejected");
    CHECK(json_parse("\"\\ud83dx\"") == NULL, "high surrogate without escape rejected");
    CHECK(json_parse("\"\\ud83d\\u0041\"") == NULL, "high surrogate with non-low rejected");
    CHECK(json_parse("\"\\ude00\"") == NULL, "lone low surrogate rejected");
    CHECK(json_parse("\"\\u12\"") == NULL, "short escape rejected");
}

static void test_configure_result(void)
{
    const char *line = "{\"id\":0,\"result\":{\"version-rolling\":true,"
                       "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}";
    json_value *v    = json_parse(line);
    CHECK(v != NULL, "configure result parses");
    const json_value *res = json_object_get(v, "result");
    CHECK(json_bool(json_object_get(res, "version-rolling")) == 1, "version-rolling true");
    CHECK(!strcmp(json_string(json_object_get(res, "version-rolling.mask")), "1fffe000"),
            "mask 1fffe000");
    CHECK(json_is(json_object_get(v, "error"), JSON_NULL), "error null");
    json_free(v);
}

static void test_subscribe_result(void)
{
    const char *line =
            "{\"id\":3,\"result\":[[[\"mining.notify\",\"mining.notify\"],"
            "[\"mining.set_difficulty\",\"mining.set_difficulty\"]],\"00\",8],\"error\":null}";
    json_value *v = json_parse(line);
    CHECK(v != NULL, "subscribe result parses");
    const json_value *res = json_object_get(v, "result");
    CHECK(json_array_size(res) == 3, "subscribe result has 3 elements");
    CHECK(!strcmp(json_string(json_array_get(res, 1)), "00"), "xnonce1 == 00");
    CHECK(json_int(json_array_get(res, 2)) == 8, "xnonce2_size == 8");
    CHECK(json_array_size(json_array_get(res, 0)) == 2, "two subscriptions");
    json_free(v);
}

static void test_notify(void)
{
    const char *line = "{\"params\":[\"JOB1\",\"PREV\",\"CB1\",\"CB2\","
                       "[\"aa\",\"bb\",\"cc\"],\"20000000\",\"17021a42\",\"6a4ae548\",true],"
                       "\"id\":null,\"method\":\"mining.notify\"}";
    json_value *v    = json_parse(line);
    CHECK(v != NULL, "notify parses");
    CHECK(!strcmp(json_string(json_object_get(v, "method")), "mining.notify"), "method");
    const json_value *p = json_object_get(v, "params");
    CHECK(json_array_size(p) == 9, "notify has 9 params");
    CHECK(!strcmp(json_string(json_array_get(p, 0)), "JOB1"), "job_id");
    const json_value *merkle = json_array_get(p, 4);
    CHECK(json_array_size(merkle) == 3, "merkle branch count");
    CHECK(!strcmp(json_string(json_array_get(merkle, 1)), "bb"), "merkle branch value");
    CHECK(json_bool(json_array_get(p, 8)) == 1, "clean_jobs true");
    json_free(v);
}

static void test_object_find(void)
{
    json_value *v = json_parse("{\"a\":{\"b\":[1,{\"c\":2}],\"d\":{\"c\":3}},\"e\":{\"f\":4},"
                               "\"f\":5}");
    CHECK(v != NULL, "nested object parses");
    CHECK(json_int(json_object_find(v, "f")) == 5, "top-level member found first");
    CHECK(json_int(json_object_find(v, "c")) == 2, "depth first, through an array");
    CHECK(json_int(json_object_find(json_object_get(v, "e"), "f")) == 4, "search from a member");
    CHECK(json_array_size(json_object_find(v, "b")) == 2, "array member returned whole");
    CHECK(json_object_find(v, "x") == NULL, "missing key -> NULL");
    CHECK(json_object_find(NULL, "a") == NULL, "NULL value -> NULL");
    CHECK(json_object_find(json_object_find(v, "f"), "a") == NULL, "scalar -> NULL");
    json_free(v);
}

int main(void)
{
    test_scalars();
    test_errors();
    test_unicode_escapes();
    test_configure_result();
    test_subscribe_result();
    test_notify();
    test_object_find();
    if (fails) {
        fprintf(stderr, "test_jread: %d FAIL\n", fails);
        return 1;
    }
    printf("test_jread: all pass\n");
    return 0;
}
