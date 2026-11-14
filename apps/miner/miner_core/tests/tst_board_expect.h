/* SPDX-License-Identifier: MIT */
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

#ifndef CORE_TST_BOARD_EXPECT_H
#define CORE_TST_BOARD_EXPECT_H

/*
 * Per-board expected values for the miner_core host tests. Strings that
 * follow from a board key are built from it at run time, so one source
 * holds for every descriptor the tests are built against.
 */

#include "boardconf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static __attribute__((format(printf, 1, 2), unused)) const char *tst_fmt(const char *fmt, ...)
{
    static char     ring[4][1024];
    static unsigned next;
    char           *b = ring[next++ % 4];
    va_list         ap;

    va_start(ap, fmt);
    vsnprintf(b, sizeof ring[0], fmt, ap);
    va_end(ap);
    return b;
}

static __attribute__((unused)) const char *tst_board_list(char *out, size_t cap, const char *label,
        int first)
{
    char tail[2 * HBOARD_COUNT + 1] = "";

    for (int b = 1; b < HBOARD_COUNT; b++)
        strcat(tail, " 0");
    snprintf(out, cap, "%s[%d%s]", label, first, tail);
    return out;
}

static __attribute__((unused)) const char *tst_fan_span(char *out, size_t cap)
{
    out[0] = '\0';
    for (int f = 0; f < FAN_COUNT; f++)
        snprintf(out + strlen(out), cap - strlen(out), " Fan%d[%d]", f + 1, 1460 + 10 * f);
    return out;
}

#endif /* CORE_TST_BOARD_EXPECT_H */
