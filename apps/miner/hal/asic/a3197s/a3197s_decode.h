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

#ifndef A3197S_DECODE_H
#define A3197S_DECODE_H

#include <stdint.h>

uint32_t get_cpm_pll(uint32_t freq);
uint32_t get_cpm_freq(uint32_t pll);
uint32_t correct_freq(uint32_t freq);
double   decode_pvt_temp(uint16_t pvt_code);
uint16_t decode_pvt_volt(uint16_t volt);

#endif /* A3197S_DECODE_H */
