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

#ifndef MM_VF_H
#define MM_VF_H

#include <stdint.h>

#include "miner_ipc.h"

void     mm_vf_start(void);
void     mm_vf_stop_request(void);
void     mm_vf_stop(void);
void     mm_vf_set_ramp_target(uint16_t base, uint8_t at_target);
void     mm_vf_asic_spawned(void);
void     mm_vf_snapshot(mm_periph *p);
void     mm_vf_set_rail(uint16_t mv);
void     mm_vf_rail_unknown(void);
int      mm_vf_set_volt(uint16_t v);
void     mm_vf_set_volt_tune(uint8_t on);
int      mm_vf_hold_freq(uint16_t pll[ASIC_PLL_POINTS]);
int      mm_vf_take_aging_failed(void);
uint64_t mm_vf_beat_ms(void);
void     mm_vf_aging_rearm(void);

#endif /* MM_VF_H */
