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

#include "cfg_u32.h"
#include <stdio.h>

#define UNTOUCHED 0xA5A5A5A5u /* sentinel out value, shows cfg_u32_parse left out unwritten */

static int fails;

static void expect(const char *s, uint32_t max, int want_rc, uint32_t want_out)
{
    uint32_t out = UNTOUCHED;
    int      rc  = cfg_u32_parse(s, max, &out);

    if (rc != want_rc || out != want_out) {
        fprintf(stderr, "  %-28s FAIL: rc %d out %u (want rc %d out %u)\n", s, rc, (unsigned)out,
                want_rc, (unsigned)want_out);
        fails++;
    } else {
        printf("  %-28s ok (rc %d out %u)\n", s, rc, (unsigned)out);
    }
}

int main(void)
{
    expect("0", 100, 0, 0);
    expect("42", 100, 0, 42);
    expect("100", 100, 0, 100);
    expect("007", 100, 0, 7);
    expect("4294967295", UINT32_MAX, 0, UINT32_MAX);
    expect("101", 100, 0, 100);
    expect("2", 1, 0, 1);

    expect("", 100, -1, UNTOUCHED);
    expect("abc", 100, -1, UNTOUCHED);
    expect("x1", 100, -1, UNTOUCHED);
    expect(" 5", 100, -1, UNTOUCHED);
    expect("+5", 100, -1, UNTOUCHED);
    expect("-1", 100, -1, UNTOUCHED);
    expect("-0", 100, -1, UNTOUCHED);
    expect("5x", 100, -1, UNTOUCHED);
    expect("5 ", 100, -1, UNTOUCHED);
    expect("1.5", 100, -1, UNTOUCHED);
    expect("0x10", 100, -1, UNTOUCHED);
    expect("99999999999999999999999", UINT32_MAX, -1, UNTOUCHED);

    if (fails) {
        fprintf(stderr, "test_cfg_u32: %d FAIL\n", fails);
        return 1;
    }
    printf("test_cfg_u32: all pass\n");
    return 0;
}
