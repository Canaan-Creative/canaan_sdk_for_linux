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

#ifndef LED_FX_H
#define LED_FX_H

#include <stdint.h>

#if defined(__has_include)
#if __has_include("boardconf.h")
#include "boardconf.h"
#endif
#endif

#define LED_PIXELS      LED_PIXEL_COUNT
#define LED_TICK_MS     20                /* effect tick period, ms */
#define LED_FX_USER_MAX LED_FX_COLORCYCLE /* highest effect a user may select */
#define LED_RGB_BLACK   0x000000u
#define LED_RGB_RED     0xFF0000u
#define LED_RGB_GREEN   0x00FF00u
#define LED_RGB_BLUE    0x0000FFu
#define LED_RGB_YELLOW  0xFFFF00u
#define LED_RGB_WHITE   0xFFFFFFu
#define LED_RGB_MAX     0xFFFFFFu

enum {
    LED_FX_DARK = 0,   /* strip off */
    LED_FX_CONTINUE,   /* steady colour (systemcfg default) */
    LED_FX_BLINK,      /* colour <-> off, twice a second */
    LED_FX_BREATH,     /* colour ramped up and down */
    LED_FX_COLORCYCLE, /* 7-colour rainbow, one step every 2 s */
    LED_FX_SHRINK,     /* white collapses inward (night-lamp off) */
    LED_FX_EXTEND,     /* white grows outward (night-lamp on) */
    LED_FX_MAX
};

typedef struct {
    uint32_t step; /* frame count within the current effect */
    uint32_t loop;
    uint32_t pixel[LED_PIXELS]; /* extend/shrink frame, 0xRRGGBB each */
    uint32_t prev_rgb;          /* colour of the last tick, 0xRRGGBB */
    uint8_t  prev_fx;           /* effect of the last tick */
    uint8_t  breath_mult;       /* breath brightness, percent */
    uint8_t  cycle_ofst;        /* next rainbow colour index */
} led_state_t;

int led_fx_tick(led_state_t *st, uint8_t fx, uint32_t rgb, uint32_t out[LED_PIXELS]);

#endif /* LED_FX_H */
