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

#include "led_fx.h"

#define R_OF(rgb) (uint8_t)(((rgb) >> 16) & 0xFFu) /* red byte of 0xRRGGBB */
#define G_OF(rgb) (uint8_t)(((rgb) >> 8) & 0xFFu)  /* green byte of 0xRRGGBB */
#define B_OF(rgb) (uint8_t)((rgb) & 0xFFu)         /* blue byte of 0xRRGGBB */
/* 0xRRGGBB from bytes */
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#define BREATH_STEPS    100 /* ticks of one breath ramp up and down */
#define BREATH_PCT_STEP 2   /* breath brightness change per tick, percent */
#define ES_CHG_PER_STEP 15  /* extend/shrink pixel level change per tick, 0..255 scale */
#define ES_TOTAL_STEPS  30  /* extend/shrink length, ticks */
#define ES_HALF_PIXELS  ((LED_PIXELS + 1) / 2) /* pixels in one half, middle included */
#define ES_COPY_PIXELS  (LED_PIXELS / 2)       /* pixels mirrored onto the other half */
#define ES_MID          (ES_HALF_PIXELS - 1)   /* middle pixel index */
/* ticks per extra changing pixel */
#define ES_STEP_RANGE ((ES_TOTAL_STEPS / 2) / ES_HALF_PIXELS)

_Static_assert(ES_STEP_RANGE > 0, "extend/shrink needs ES_TOTAL_STEPS / 2 >= ES_HALF_PIXELS");

static void inner_reset(led_state_t *st)
{
    st->step = 0;
    for (int i = 0; i < LED_PIXELS; i++)
        st->pixel[i] = LED_RGB_BLACK;
}

static void fill(uint32_t out[LED_PIXELS], uint32_t rgb)
{
    for (int i = 0; i < LED_PIXELS; i++)
        out[i] = rgb;
}

static int fx_solid(led_state_t *st, uint32_t rgb, uint32_t out[LED_PIXELS])
{
    if (st->step >= 1)
        return 0;
    fill(out, rgb);
    st->step++;
    return 1;
}

static int fx_blink(led_state_t *st, uint32_t rgb, uint32_t out[LED_PIXELS])
{
    fill(out, (st->step % 2) ? rgb : LED_RGB_BLACK);
    st->step++;
    return 1;
}

static int fx_cycle(led_state_t *st, uint32_t out[LED_PIXELS])
{
    static const uint32_t rainbow[] = {
        LED_RGB_WHITE,
        LED_RGB_YELLOW,
        0xFF6100u,
        LED_RGB_RED,
        0xA020F0u,
        LED_RGB_BLUE,
        LED_RGB_GREEN,
    };
    const uint8_t n = (uint8_t)(sizeof rainbow / sizeof rainbow[0]);
    fill(out, rainbow[st->cycle_ofst]);
    st->cycle_ofst = (uint8_t)((st->cycle_ofst + 1) % n);
    return 1;
}

static int fx_breath(led_state_t *st, uint32_t rgb, uint32_t out[LED_PIXELS])
{
    if (st->step == 0)
        st->breath_mult = 0;
    if (st->step < (BREATH_STEPS / 2))
        st->breath_mult += BREATH_PCT_STEP;
    else if (st->step == (BREATH_STEPS / 2)) {
    } else if (st->step < BREATH_STEPS)
        st->breath_mult -= BREATH_PCT_STEP;
    else {
        st->step        = 0;
        st->breath_mult = 0;
    }

    uint32_t m = st->breath_mult;
    fill(out, RGB(R_OF(rgb) * m / 100u, G_OF(rgb) * m / 100u, B_OF(rgb) * m / 100u));
    st->step++;
    return 1;
}

static uint32_t virtual_white(uint32_t pixel, int chg)
{
    int v = (int)R_OF(pixel) + chg;
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return RGB(v, v, v);
}

static int fx_extend_shrink(led_state_t *st, int shrink, uint32_t out[LED_PIXELS])
{
    if (st->step >= ES_TOTAL_STEPS)
        return 0;
    const int scale = shrink ? -ES_CHG_PER_STEP : ES_CHG_PER_STEP;

    if (st->step == 0)
        fill(st->pixel, shrink ? LED_RGB_WHITE : LED_RGB_BLACK);
    if (st->step == (ES_TOTAL_STEPS - 1)) {
        fill(st->pixel, shrink ? LED_RGB_BLACK : LED_RGB_WHITE);
    } else {
        int nchg = (int)(st->step / ES_STEP_RANGE);
        if (nchg > ES_HALF_PIXELS)
            nchg = ES_HALF_PIXELS;
        for (int i = 0; i <= nchg; i++) {
            int at = shrink ? i : (ES_MID - i);
            if (at < 0 || at >= LED_PIXELS)
                continue;
            st->pixel[at] = virtual_white(st->pixel[at], scale);
        }
        for (int i = 0; i < ES_COPY_PIXELS; i++)
            st->pixel[LED_PIXELS - 1 - i] = st->pixel[i];
    }
    st->step++;
    for (int i = 0; i < LED_PIXELS; i++)
        out[i] = st->pixel[i];
    return 1;
}

int led_fx_tick(led_state_t *st, uint8_t fx, uint32_t rgb, uint32_t out[LED_PIXELS])
{
    if (!st || !out)
        return 0;

    if (st->prev_fx != fx || st->prev_rgb != rgb) {
        st->prev_fx  = fx;
        st->prev_rgb = rgb;
        inner_reset(st);
    }
    st->loop++;

    switch (fx) {
    case LED_FX_DARK:
        return fx_solid(st, LED_RGB_BLACK, out);
    case LED_FX_CONTINUE:
        return fx_solid(st, rgb, out);
    case LED_FX_BLINK:
        return (st->loop % 25) == 0 ? fx_blink(st, rgb, out) : 0;
    case LED_FX_BREATH:
        return fx_breath(st, rgb, out);
    case LED_FX_COLORCYCLE:
        return (st->loop % 100) == 0 ? fx_cycle(st, out) : 0;
    case LED_FX_SHRINK:
        return fx_extend_shrink(st, 1, out);
    case LED_FX_EXTEND:
        return fx_extend_shrink(st, 0, out);
    default:
        return 0;
    }
}
