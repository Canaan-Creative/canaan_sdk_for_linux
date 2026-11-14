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

#include <assert.h>
#include <stdio.h>
#include "boardconf.h"
#include "uart_pro.h"
#include "board_tables.h"
#include "tst_board_expect.h"

int main(void)
{
    assert(CHANNELS == EXPECT_CHANNELS);
    for (int i = 0; i < CHANNELS; i++)
        assert(board_hboards[i].uart_dev != NULL);

    uart_chip_select(0, 0x123);
    assert(uart_get_chip_select(0) == 0x123);
    uart_chip_select(0, 0);
    assert(uart_get_chip_select(0) == 0);

    if (CHANNELS > 1) {
        uart_chip_select(0, 0x011);
        uart_chip_select(1, 0x022);
        assert(uart_get_chip_select(0) == 0x011);
        assert(uart_get_chip_select(1) == 0x022);

        uart_chip_select(1, 0x0aa);
        uart_chip_select(0, 0x0bb);
        assert(uart_get_chip_select(0) == 0x0bb);
        assert(uart_get_chip_select(1) == 0x0aa);

        uart_chip_select(0, 0x3ff);
        assert(uart_get_chip_select(1) == 0x0aa);
        assert(uart_get_chip_select(0) == 0x3ff);
    } else {
        uart_chip_select(1, 0x077);
        assert(uart_get_chip_select(0) == 0x077);
        assert(uart_get_chip_select(1) == 0x077);
    }

    uart_chip_select(0, 0x155);
    assert(uart_get_chip_select(CHANNELS) == 0x155);
    uart_chip_select(CHANNELS, 0x166);
    assert(uart_get_chip_select(0) == 0x166);
    assert(uart_get_chip_select(2 * CHANNELS) == 0x166);
    assert(uart_get_chip_select(255) == uart_get_chip_select(255 % CHANNELS));

    printf("uart chip select per channel (%s, %d ch) OK\n", BOARD_NAME, CHANNELS);
    return 0;
}
