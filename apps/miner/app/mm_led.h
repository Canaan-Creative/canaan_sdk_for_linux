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

#ifndef MM_LED_H
#define MM_LED_H

#include <stdint.h>

#include "mm_led_cfg.h"
#include "mc_set_result.h"

void          mm_led_start(void);
void          mm_led_reload(void);
mc_set_result mm_led_apply(uint16_t op, const char *args);
void          mm_led_state(uint8_t *ledmode, uint8_t *runeffect);
void          mm_led_snapshot(mm_led_cfg_t *cfg, uint8_t *runeffect);
int           mm_led_wake(void);
void          mm_led_stop(void);

#endif /* MM_LED_H */
