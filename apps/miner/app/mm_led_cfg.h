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

#ifndef MM_LED_CFG_H
#define MM_LED_CFG_H

#include <stdint.h>

/* Light-strip settings kept in systemcfg.ini */
typedef struct {
    uint8_t  ledmode; /* [syscfg] ledmode: 0 day, 1 dark (strip off except the night lamp) */
    uint8_t  effect;  /* [led] effect: LED_FX_* shown in day mode, 0..LED_FX_USER_MAX */
    uint8_t  bright;  /* [led] bright: brightness 0..100, stored and reported only */
    uint32_t temper;  /* [led] temper: color temperature 0..0xFFFF, stored and reported only */
    uint32_t rgb;     /* [led] rgb: color 0xRRGGBB, 0..LED_RGB_MAX, stored in decimal */
} mm_led_cfg_t;

/* Used for each key absent from systemcfg.ini or not a valid number */
static const mm_led_cfg_t mm_led_cfg_default = {
    .ledmode = 0,
    .effect  = 1,
    .bright  = 100,
    .temper  = 50,
    .rgb     = 255,
};

#endif /* MM_LED_CFG_H */
