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

#ifndef TST_UART_H
#define TST_UART_H

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "boardconf.h"
#include "board_tables.h"

_Static_assert(sizeof TST_UART_ROOT + sizeof "/uart0" - 1 <= 256,
        "TST_UART_ROOT too long for the node buffer");

static int tst_uart_open(const char *path, int flags)
{
    char p[256];

    for (size_t i = 0; path && i < HBOARD_COUNT; i++) {
        if (board_hboards[i].uart_dev && strcmp(path, board_hboards[i].uart_dev) == 0) {
            snprintf(p, sizeof p, "%s/uart%zu", TST_UART_ROOT, i);
            return open(p, flags);
        }
    }
    return open(path, flags);
}

#endif /* TST_UART_H */
