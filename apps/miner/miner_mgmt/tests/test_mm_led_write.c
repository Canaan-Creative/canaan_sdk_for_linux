// SPDX-License-Identifier: MIT
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

#include <assert.h>
#include "../../app/mm_led.c"

/* w2812_set() calls; log lines; w2812_set() calls left to fail */
static int      writes, errors, failures;
static uint32_t last_frame[LED_PIXELS]; /* last frame written, 0xRRGGBB per pixel */

int w2812_set(uint32_t *frame)
{
    writes++;
    memcpy(last_frame, frame, sizeof last_frame);
    if (failures) {
        failures--;
        return -1;
    }
    return 0;
}

void osif_log(osif_loglvl_t level, const char *format, ...)
{
    (void)level;
    (void)format;
    errors++;
}

int main(void)
{
    led_write_state output = { 0 };
    failures               = 2;
    led_write_tick(&output, LED_FX_CONTINUE, 0x010203, 0);
    assert(writes == 1 && errors == 1);
    led_write_tick(&output, LED_FX_CONTINUE, 0x010203, 20);
    assert(writes == 1);
    led_write_tick(&output, LED_FX_CONTINUE, 0x010203, 999);
    assert(writes == 1);
    led_write_tick(&output, LED_FX_CONTINUE, 0x010203, 1000);
    assert(writes == 2 && errors == 2);
    led_write_tick(&output, LED_FX_CONTINUE, 0x010203, 2000);
    assert(writes == 3 && errors == 2 && last_frame[0] == 0x010203);
    led_write_tick(&output, LED_FX_CONTINUE, 0x010203, 2100);
    assert(writes == 3);

    failures = 1;
    led_write_tick(&output, LED_FX_DARK, 0x010203, 2200);
    assert(writes == 4 && errors == 3 && last_frame[0] == 0);
    led_write_tick(&output, LED_FX_DARK, 0x010203, 3199);
    assert(writes == 4);
    led_write_tick(&output, LED_FX_DARK, 0x010203, 3200);
    assert(writes == 5 && errors == 3 && last_frame[0] == 0);

    failures = 1;
    led_write_tick(&output, LED_FX_CONTINUE, 0xFF0000, 3300);
    led_write_tick(&output, LED_FX_DARK, 0xFF0000, 3320);
    assert(writes == 7 && errors == 4 && last_frame[0] == 0);
    led_write_tick(&output, LED_FX_DARK, 0xFF0000, 4400);
    assert(writes == 7);

    memset(&output, 0, sizeof output);
    writes = errors = 0;
    failures        = 1000;
    for (uint64_t now = 0; now < 10000; now += 20)
        led_write_tick(&output, LED_FX_BREATH, 0xFFFFFF, now);
    assert(writes == 10 && errors == 10);
    failures = 0;
    led_write_tick(&output, LED_FX_BREATH, 0xFFFFFF, 10000);
    assert(writes == 11 && errors == 10 && !output.pending);
    return 0;
}
