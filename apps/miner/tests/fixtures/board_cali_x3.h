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

#ifndef BOARD_CALI_X3_H
#define BOARD_CALI_X3_H

/*
 * default calibration rows: { ASIC count, { max_pout W, temp degC, volt,
 * pll_start MHz, pll_interval MHz } per work mode }
 */
/* clang-format off */
#define BOARD_CALI_ROWS \
    { \
        44, \
        { \
            { 180, 75, 2500, 250, 25 }, \
            { 240, 80, 2700, 300, 25 }, \
            { 300, 85, 2900, 350, 25 }, \
        }, \
    },
/* clang-format on */

#endif /* BOARD_CALI_X3_H */
