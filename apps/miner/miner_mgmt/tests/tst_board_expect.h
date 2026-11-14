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

#ifndef MGMT_TST_BOARD_EXPECT_H
#define MGMT_TST_BOARD_EXPECT_H

#include "boardconf.h"
#include "psu_ops.h"

#define TST_PSU_FLAGS /* the board PSU's flags */ \
    ((unsigned)(HAL_PSU_F_OFF_CONFIRM | HAL_PSU_F_LATCH_ON_FAIL))

static inline int tst_psu_vout_fault(int vout, int pout_w, int vout12)
{
    (void)pout_w;
    (void)vout12;
    return vout < POWER_VALID_VOLT_MIN;
}

#endif /* MGMT_TST_BOARD_EXPECT_H */
