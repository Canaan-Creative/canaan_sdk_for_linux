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

#ifndef HAL_TST_BOARD_EXPECT_H
#define HAL_TST_BOARD_EXPECT_H

/*
 * Per-board expectations for the HAL tests built once per product.
 * TEST_BOARD_X3 is set by the x3 test targets; the values are written by
 * hand off each board, not derived from its descriptor, so a descriptor
 * change shows up as a failure. Include after boardconf.h.
 */
#include "boardconf.h"

#ifdef TEST_BOARD_X3
#define TST_BOARD_X3    1
#define BOARD_NAME      "x3"
#define EXPECT_CHANNELS 2
#define WANT_SLOTS      3 /* expected tachometer counter count */
#elif defined(BOARD_NANO3S_H)
#define TST_BOARD_X3    0
#define BOARD_NAME      "nano3s"
#define EXPECT_CHANNELS 1
#define WANT_SLOTS      1 /* expected tachometer counter count */
#else
#error "no HAL test expectations for this board"
#endif

/* ASIC UART channels: board_hboards[] rows 0..HBOARD_COUNT-1. */
#define CHANNELS HBOARD_COUNT

#endif /* HAL_TST_BOARD_EXPECT_H */
