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

#ifndef BOARD_DESC_H
#define BOARD_DESC_H

#include <stdint.h>

typedef struct board_desc {
    uint16_t hboard_count;
    uint16_t max_asic; /* chips per hash board, count */
    uint16_t fan_count;
    uint16_t freq_interval; /* spacing between adjacent PLL frequency points, MHz */
} board_desc_t;

extern board_desc_t g_board;

void board_desc_init(const board_desc_t *seed);

#endif /* BOARD_DESC_H */
