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

#ifndef CORE_BOARD_H
#define CORE_BOARD_H

#include "boardconf.h"

#define CORE_ASC_COUNT 1 /* ASC devices reported by the API */
#define CORE_PUB_LCD   CAP_LCD
#define CORE_PUB_LED   CAP_LED_STRIP
#define CORE_PUB_AUDIO CAP_BEEP /* 1 when the API reports the volume field */

/* CORE_SEL(p, ...) expands to its tail when p is the literal 1 and to nothing when 0. */
#define CORE_SEL_0(...)
#define CORE_SEL_1(...)   __VA_ARGS__
#define CORE_SEL_(p, ...) CORE_SEL_##p(__VA_ARGS__)
#define CORE_SEL(p, ...)  CORE_SEL_(p, __VA_ARGS__)
/* estats format fragment, LCD on/off and switch */
#define CORE_FMT_LCD_ONOFF CORE_SEL(CORE_PUB_LCD, "LcdOnoff[%d] LcdSwitch[%d] ") ""
#define CORE_ARG_LCD_ONOFF CORE_SEL(CORE_PUB_LCD, s->lcd_onoff, s->lcd_show, )
#define CORE_MEM_LCD_ONOFF int lcd_onoff, lcd_show; /* stats members, LCD on/off and switch */
/* estats format fragment, LCD brightness */
#define CORE_FMT_LCD_BRIGHT CORE_SEL(CORE_PUB_LCD, "LcdBright[%d] ") ""
#define CORE_ARG_LCD_BRIGHT CORE_SEL(CORE_PUB_LCD, s->lcd_bright, )
#define CORE_MEM_LCD_BRIGHT int lcd_bright;
/* estats format fragment, LED mode and user effect */
#define CORE_FMT_LED CORE_SEL(CORE_PUB_LED, "LED[%d-%d] LEDUser[%d-%d-%u-%u-%u-%u] ") ""
#define CORE_ARG_LED \
    CORE_SEL(CORE_PUB_LED, s->ledmode, s->led_runeffect, s->led_effect, s->led_bright, \
            s->led_temper, (s->led_rgb >> 16) & 0xFFu, (s->led_rgb >> 8) & 0xFFu, \
            s->led_rgb & 0xFFu, )
/* stats members, LED user effect, brightness, run effect, color temperature and RGB */
#define CORE_MEM_LED \
    int      led_effect, led_bright; \
    int      led_runeffect; \
    uint32_t led_temper, led_rgb;
/* estats format fragment, volume */
#define CORE_FMT_VOLUME CORE_SEL(CORE_PUB_AUDIO, "Volume[%d] ") ""
#define CORE_ARG_VOLUME CORE_SEL(CORE_PUB_AUDIO, s->volume, )
#define CORE_MEM_VOLUME int volume;

#endif /* CORE_BOARD_H */
