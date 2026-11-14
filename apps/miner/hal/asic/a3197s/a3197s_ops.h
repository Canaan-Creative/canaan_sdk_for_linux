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

#ifndef A3197S_OPS_H
#define A3197S_OPS_H

#include <stdint.h>

#include "a3197s_reg.h"

void     a3197s_config_asic(uint8_t channel);
void     a3197s_config_coreregs(uint8_t channel);
void     a3197s_mx2_reset(uint8_t channel);
int      a3197s_read_reg(uint8_t channel, uint32_t addr, uint32_t *out, uint32_t n);
uint32_t a3197s_enumerate(uint8_t channel);
void     a3197s_set_freq(const uint16_t f[A3197S_PLL_POINTS]);
void     a3197s_enable_nonce_return(uint8_t channel);
void     a3197s_write_version(uint8_t channel, const uint32_t v[8]);
int      a3197s_pvt_select_mode(uint8_t channel, int temp_mode, uint16_t chip);
void     a3197s_fetch_pvt_temp(uint8_t channel, double *degc);
void     a3197s_fetch_pvt_volt(uint8_t channel, uint16_t *mv);
void     a3197s_enter_config_mode(uint8_t channel);
void     a3197s_enter_work_mode(uint8_t channel);

#endif /* A3197S_OPS_H */
