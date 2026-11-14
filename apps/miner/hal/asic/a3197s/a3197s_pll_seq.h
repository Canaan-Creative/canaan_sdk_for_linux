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

#ifndef A3197S_PLL_SEQ_H
#define A3197S_PLL_SEQ_H

#include <stdint.h>

#include "a3197s_reg.h"

#define A3197S_CORE_COUNT 152 /* big cores per chip, count */
/* ROLLTIME dividend; ROLLTIME = this / PLL3 frequency in MHz */
#define A3197S_TIMEOUT_CONST 335000000

typedef struct a3197s_pll_io {
    void (*chip_select)(uint8_t miner_id, uint16_t chip_addr);
    void (*set_reg)(uint8_t miner_id, uint32_t addr, uint32_t value);
    uint32_t (*get_reg)(uint8_t miner_id, uint32_t addr);
    uint16_t (*get_chip_select)(uint8_t miner_id);
    void (*delay_ticks)(uint32_t ticks); /* delay, ms */
} a3197s_pll_io_t;

typedef struct a3197s_pll_cfg {
    uint32_t freq[A3197S_PLL_POINTS]; /* PLL0..PLL3 target frequencies, MHz */
    uint32_t init_freq;               /* PLL0 park frequency, MHz */
    /* SS_CTRL ssdn_protect, spdlow, spdhigh, final spd_en and final glbset */
    uint8_t  ssdn_pro, spdlow, spdhigh, ss_enable, pll_select;
    uint8_t  ss_sel, ss_pri; /* SS_CTRL ss_sel and ss_pri */
    uint16_t ss_pd;          /* SS_CTRL ss_pd */
} a3197s_pll_cfg_t;

void    a3197s_set_pll_seq(const a3197s_pll_io_t *io, uint8_t miner_id, uint16_t asic_id,
           uint8_t pll0_update, const a3197s_pll_cfg_t *cfg);
uint8_t calc_mux_div_mode(uint32_t freq);

#endif /* A3197S_PLL_SEQ_H */
