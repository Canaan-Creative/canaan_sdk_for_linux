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
#include <stdio.h>
#include "../../tests/include/tst_check.h"

/* 0xRRGGBB colour with all three channels set to v */
#define GREY(v) (((uint32_t)(v) << 16) | ((uint32_t)(v) << 8) | (uint32_t)(v))

static int all_of(const uint32_t f[LED_PIXELS], uint32_t rgb)
{
    for (int i = 0; i < LED_PIXELS; i++)
        if (f[i] != rgb)
            return 0;
    return 1;
}

static int run(led_state_t *st, uint8_t fx, uint32_t rgb, int n, uint32_t last[LED_PIXELS])
{
    uint32_t out[LED_PIXELS];
    int      fired = 0;
    for (int i = 0; i < n; i++) {
        if (led_fx_tick(st, fx, rgb, out)) {
            fired++;
            for (int p = 0; p < LED_PIXELS; p++)
                last[p] = out[p];
        }
    }
    return fired;
}

int main(void)
{
    uint32_t    f[LED_PIXELS] = { 0 };
    led_state_t st;

    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_DARK, LED_RGB_RED, 1, f) == 1 && all_of(f, LED_RGB_BLACK),
            "DARK: first tick writes black");
    CHECK(run(&st, LED_FX_DARK, LED_RGB_RED, 50, f) == 0, "DARK: no further writes");

    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_CONTINUE, LED_RGB_BLUE, 1, f) == 1 && all_of(f, LED_RGB_BLUE),
            "CONTINUE: first tick writes the colour");
    CHECK(run(&st, LED_FX_CONTINUE, LED_RGB_BLUE, 99, f) == 0, "CONTINUE: holds, one write");
    CHECK(run(&st, LED_FX_CONTINUE, LED_RGB_GREEN, 1, f) == 1 && all_of(f, LED_RGB_GREEN),
            "CONTINUE: rgb change re-writes");
    CHECK(run(&st, LED_FX_DARK, LED_RGB_GREEN, 1, f) == 1 && all_of(f, LED_RGB_BLACK),
            "fx change resets -> DARK writes");

    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_BLINK, LED_RGB_RED, 24, f) == 0, "BLINK: silent for 24 ticks");
    CHECK(run(&st, LED_FX_BLINK, LED_RGB_RED, 1, f) == 1 && all_of(f, LED_RGB_BLACK),
            "BLINK: tick 25 is off (legacy step parity)");
    CHECK(run(&st, LED_FX_BLINK, LED_RGB_RED, 25, f) == 1 && all_of(f, LED_RGB_RED),
            "BLINK: tick 50 is on");
    CHECK(run(&st, LED_FX_BLINK, LED_RGB_RED, 25, f) == 1 && all_of(f, LED_RGB_BLACK),
            "BLINK: tick 75 is off");

    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 1, f) == 1 && all_of(f, 0x050000u),
            "BREATH: tick 1 = 2%% of red (5)");
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 1, f) == 1 && all_of(f, 0x0A0000u),
            "BREATH: tick 2 = 4%% (10)");
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 48, f) == 48 && all_of(f, LED_RGB_RED),
            "BREATH: tick 50 = full");
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 1, f) == 1 && all_of(f, LED_RGB_RED),
            "BREATH: tick 51 plateaus at full");
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 1, f) == 1 && all_of(f, 0xF90000u),
            "BREATH: tick 52 = 98%% (249)");
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 48, f) == 48 && all_of(f, 0x050000u),
            "BREATH: tick 100 back to 2%%");
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 1, f) == 1 && all_of(f, LED_RGB_BLACK),
            "BREATH: tick 101 wraps through black");
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_RED, 1, f) == 1 && all_of(f, 0x050000u),
            "BREATH: tick 102 restarts the ramp");
    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_BREATH, LED_RGB_WHITE, 1, f) == 1 && all_of(f, 0x050505u),
            "BREATH: scales r,g,b");

    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_COLORCYCLE, LED_RGB_RED, 99, f) == 0, "CYCLE: silent for 99 ticks");
    CHECK(run(&st, LED_FX_COLORCYCLE, LED_RGB_RED, 1, f) == 1 && all_of(f, LED_RGB_WHITE),
            "CYCLE: tick 100 = white");
    CHECK(run(&st, LED_FX_COLORCYCLE, LED_RGB_RED, 100, f) == 1 && all_of(f, LED_RGB_YELLOW),
            "CYCLE: tick 200 = yellow");
    CHECK(run(&st, LED_FX_COLORCYCLE, LED_RGB_RED, 100, f) == 1 && all_of(f, 0xFF6100u),
            "CYCLE: tick 300 = orange");
    CHECK(run(&st, LED_FX_COLORCYCLE, LED_RGB_RED, 400, f) == 4 && all_of(f, LED_RGB_GREEN),
            "CYCLE: tick 700 = green (last of 7)");
    CHECK(run(&st, LED_FX_COLORCYCLE, LED_RGB_RED, 100, f) == 1 && all_of(f, LED_RGB_WHITE),
            "CYCLE: wraps to white");

    st = (led_state_t){ 0 };
    run(&st, LED_FX_EXTEND, LED_RGB_WHITE, 1, f);
    CHECK(f[4] == GREY(15) && f[3] == LED_RGB_BLACK && f[5] == LED_RGB_BLACK,
            "EXTEND: step 0 lights the middle pixel only");
    run(&st, LED_FX_EXTEND, LED_RGB_WHITE, 2, f);
    CHECK(f[4] == GREY(45) && f[3] == LED_RGB_BLACK, "EXTEND: middle ramps 15 per step");
    run(&st, LED_FX_EXTEND, LED_RGB_WHITE, 1, f);
    CHECK(f[4] == GREY(60) && f[3] == GREY(15) && f[5] == GREY(15),
            "EXTEND: step 3 adds pixel 3, mirrored onto 5");
    CHECK(f[0] == LED_RGB_BLACK && f[8] == LED_RGB_BLACK, "EXTEND: ends still dark");
    CHECK(run(&st, LED_FX_EXTEND, LED_RGB_WHITE, 26, f) == 26 && all_of(f, LED_RGB_WHITE),
            "EXTEND: step 29 fills white");
    CHECK(run(&st, LED_FX_EXTEND, LED_RGB_WHITE, 100, f) == 0, "EXTEND: stops after 30 steps");

    st = (led_state_t){ 0 };
    run(&st, LED_FX_SHRINK, LED_RGB_WHITE, 1, f);
    CHECK(f[0] == GREY(240) && f[8] == GREY(240) && f[1] == LED_RGB_WHITE,
            "SHRINK: step 0 dims the outer pixel, mirrored");
    run(&st, LED_FX_SHRINK, LED_RGB_WHITE, 3, f);
    CHECK(f[0] == GREY(195) && f[1] == GREY(240) && f[7] == GREY(240),
            "SHRINK: step 3 dims pixel 1, mirrored onto 7");
    CHECK(run(&st, LED_FX_SHRINK, LED_RGB_WHITE, 26, f) == 26 && all_of(f, LED_RGB_BLACK),
            "SHRINK: step 29 fills black");
    CHECK(run(&st, LED_FX_SHRINK, LED_RGB_WHITE, 100, f) == 0, "SHRINK: stops after 30 steps");
    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_SHRINK, LED_RGB_WHITE, 30, f) == 30, "SHRINK: replays after reset");

    st = (led_state_t){ 0 };
    CHECK(run(&st, LED_FX_MAX, LED_RGB_RED, 200, f) == 0, "unknown fx writes nothing");
    CHECK(run(&st, 200, LED_RGB_RED, 200, f) == 0, "out-of-enum fx writes nothing");

    uint32_t o[LED_PIXELS];
    CHECK(led_fx_tick(NULL, LED_FX_CONTINUE, LED_RGB_RED, o) == 0, "NULL state -> 0");
    st = (led_state_t){ 0 };
    CHECK(led_fx_tick(&st, LED_FX_CONTINUE, LED_RGB_RED, NULL) == 0, "NULL out -> 0");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
