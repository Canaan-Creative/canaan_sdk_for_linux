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

#ifndef TST_TACH_H
#define TST_TACH_H

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "board_tables.h"

_Static_assert(sizeof TST_TACH_ROOT + sizeof "/tach0" - 1 <= 256,
        "TST_TACH_ROOT too long for the node buffer");

static int tst_tach_open(const char *path, int flags)
{
    char p[256];

    for (size_t i = 0; path && i < board_fan_count; i++) {
        if (board_fans[i].tach_node && strcmp(path, board_fans[i].tach_node) == 0) {
            snprintf(p, sizeof p, "%s/tach%zu", TST_TACH_ROOT, i);
            return open(p, flags);
        }
    }
    return open(path, flags);
}

#endif /* TST_TACH_H */
