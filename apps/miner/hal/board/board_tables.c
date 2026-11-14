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

#include "board_tables.h"
#include "boardconf.h"
#include "ctrl_cmds.h"

const board_fan_t board_fans[]    = { BOARD_FAN_ROWS };
const size_t      board_fan_count = sizeof board_fans / sizeof board_fans[0];
_Static_assert(sizeof board_fans / sizeof board_fans[0] == FAN_COUNT,
        "BOARD_FAN_ROWS row count != FAN_COUNT");

const board_hboard_t board_hboards[] = { BOARD_HBOARD_ROWS };
_Static_assert(sizeof board_hboards / sizeof board_hboards[0] >= HBOARD_COUNT,
        "BOARD_HBOARD_ROWS has fewer slots than HBOARD_COUNT populates");

#if CAP_HB_MCU
const board_hb_mcu_t board_hb_mcus[]    = { BOARD_HB_MCU_ROWS };
const size_t         board_hb_mcu_count = sizeof board_hb_mcus / sizeof board_hb_mcus[0];
_Static_assert(sizeof board_hb_mcus / sizeof board_hb_mcus[0] >= HBOARD_COUNT,
        "BOARD_HB_MCU_ROWS has fewer slots than HBOARD_COUNT populates");
_Static_assert(sizeof board_hb_mcus / sizeof board_hb_mcus[0] ==
                sizeof board_hboards / sizeof board_hboards[0],
        "BOARD_HB_MCU_ROWS and BOARD_HBOARD_ROWS describe the same carrier "
        "slots and row i is slot i in both, so they must have the same row "
        "count: a slot with an MCU but no hashboard row (or the reverse) "
        "makes the two tables disagree about which physical slot index i is");
#else  /* CAP_HB_MCU */
const board_hb_mcu_t board_hb_mcus[HBOARD_COUNT]; /* all-zero placeholder, no MCUs */
const size_t         board_hb_mcu_count = 0;
#endif /* CAP_HB_MCU */

const uint16_t board_ctrl_ops[]    = { BOARD_CTRL_OPS, CTRL_PLUGIN_BOARD_OPS };
const size_t   board_ctrl_op_count = sizeof board_ctrl_ops / sizeof board_ctrl_ops[0];
