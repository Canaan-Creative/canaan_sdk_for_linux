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

#include "a3197s_decode.h"
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
    CHECK(get_cpm_pll(100) == 0x90084303u, "get_cpm_pll(100) -> 0x90084303");
    CHECK(get_cpm_freq(0x90084303u) == 100, "get_cpm_freq(0x90084303) -> 100");

    uint32_t on_grid[] = { 100, 400, 460, 600 };
    int      rt_ok     = 1;
    for (unsigned i = 0; i < sizeof on_grid / sizeof on_grid[0]; i++) {
        uint32_t w = get_cpm_pll(on_grid[i]);
        if (w == 0 || get_cpm_freq(w) != on_grid[i]) {
            rt_ok = 0;
            fprintf(stderr, "    f=%u w=%08x back=%u\n", on_grid[i], w, get_cpm_freq(w));
        }
    }
    CHECK(rt_ok, "on-grid round-trip (100/400/460/600) is identity");

    CHECK(get_cpm_pll(401) == get_cpm_pll(402), "get_cpm_pll(401) -> snap up to 402");
    CHECK(get_cpm_pll(5000) == 0, "get_cpm_pll above max -> 0");
    CHECK(get_cpm_freq(get_cpm_pll(1)) == 15, "get_cpm_pll below min clamps to lowest entry (15)");

    CHECK(correct_freq(401) == 400, "correct_freq(401) -> 400 (snap down)");
    CHECK(correct_freq(100) == 99, "correct_freq(100) -> 99 (FPLL 200 snaps down to 198)");
    CHECK(get_cpm_pll(correct_freq(100)) == 0x90084203u,
            "correct_freq(100) programs 0x90084203 (FPLL 198)");
    CHECK(correct_freq(101) == 100, "correct_freq(101) -> 100 (FPLL 202 snaps down to 201)");
    CHECK(get_cpm_pll(correct_freq(101)) == 0x90084303u,
            "correct_freq(101) programs 0x90084303 (FPLL 201)");
    CHECK(correct_freq(400) == 399, "correct_freq(400) -> 399 (FPLL 800 snaps down to 798)");
    CHECK(correct_freq(50) == 99, "correct_freq(50) floors to init freq 100, then snaps to 99");
    CHECK(correct_freq(5000) == 600, "correct_freq above max clamps to 600");
    CHECK(get_cpm_freq(get_cpm_pll(correct_freq(401))) == correct_freq(401),
            "correct_freq(401) reads back as programmed");

    CHECK(get_cpm_freq(0x90084303u | 0x40000000u) == 100, "get_cpm_freq masks bit30 -> 100");

    CHECK(get_cpm_freq(0x12345678u) == 0, "unknown word -> 0");

    if (fails) {
        fprintf(stderr, "test_a3197s_pll: %d FAIL\n", fails);
        return 1;
    }
    printf("test_a3197s_pll: all pass\n");
    return 0;
}
