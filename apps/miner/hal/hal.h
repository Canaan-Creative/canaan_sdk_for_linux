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

#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

#include "psu_ops.h"
#include "temp_ops.h"
#include "hbmcu_ops.h"
#include "net_ops.h"
#include "board/board_tables.h"

#define HAL_OK  0
#define HAL_ERR (-1)

struct toast_work;
struct toast_nonce;

typedef struct hal_asic_ops {
    int (*send_work)(uint8_t channel, struct toast_work *w, uint32_t len);
    int (*get_nonce)(uint8_t channel, struct toast_nonce *n, uint8_t *count);
    int (*set_target)(uint8_t channel, uint32_t target_h, uint32_t target_l);
    void (*set_pll)(uint8_t miner_id, uint16_t asic_id, uint8_t pll0_update);
    void (*read_efuse)(uint8_t channel, uint32_t *data);
} hal_asic_ops_t;

const hal_asic_ops_t *hal_asic(void);

#endif /* HAL_H */
