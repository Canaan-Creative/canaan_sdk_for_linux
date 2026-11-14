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
#include <stdio.h>
#include <string.h>
#include "aging.h"

/* Aging sweep parameters: 6 up / 3 down stages of 6 MHz, PLL points 3 MHz apart */
static const aging_params_t P = {
    .up_stages     = 6,
    .down_stages   = 3,
    .freq_step     = 6,
    .pout_up_limit = 1,
    .pll_offset    = 1,
    .pll_interval  = 3,
};

/* As P but with 14 up stages */
static const aging_params_t P_LONG = {
    .up_stages     = 14,
    .down_stages   = 3,
    .freq_step     = 6,
    .pout_up_limit = 1,
    .pll_offset    = 1,
    .pll_interval  = 3,
};

static aging_telem_t T(int rate, uint16_t pout, short v)
{
    aging_telem_t t;
    memset(&t, 0, sizeof(t));
    t.hashrate  = rate;
    t.pout      = pout;
    t.voutcmd   = v;
    t.temp      = 88;
    t.power_ok  = 1;
    t.max_pout  = 90;
    t.pll_start = 400;
    return t;
}

static aging_action_t due(aging_state_t *st, const aging_telem_t *t, uint16_t *f, short *v)
{
    return aging_rule_tick(st, &P, t, f, v);
}

int main(void)
{
    aging_state_t st;
    uint16_t      f = 0;
    short         v = 0;

    assert(aging_pe(90, 0) == 999999);
    assert(aging_pe(90, 3000000) == 90u * 1000000 / 3000);
    assert(aging_pe(90, 999) == 999999);
    assert(aging_pe(90, 1) == 999999);
    assert(aging_pe(4500, 300000000) == 15000);
    uint16_t pll[AGING_PLL_COUNT] = { 100, 100, 100, 100 };
    aging_pll_fill(pll, &P, 400);
    assert(pll[0] == 100 && pll[1] == 400 && pll[2] == 403 && pll[3] == 406);

    aging_reset(&st, 100);
    assert(st.current_pll[1] == 100 && st.best_pe.pe == 999999);

    aging_telem_t t = T(3000000, 90, 3700);
    for (int i = 0; i < 6; i++) {
        assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
        if (i == 0)
            assert(st.best_rate.hashrate_Mhz == 0);
        if (i == 1)
            assert(st.best_rate.hashrate_Mhz == 3100000);
        t.hashrate += 100000;
    }
    assert(st.current_pll[1] == 100 + 6 * 6);

    assert(due(&st, &t, &f, &v) == AGING_ACT_SET_PLL_START);
    assert(f == 400 && st.current_pll[1] == 400);
    assert(st.best_rate.hashrate_Mhz == 3600000);
    assert(st.best_rate.pe == aging_pe(90, 3600000));
    assert(st.best_pe.hashrate_Mhz == 3600000);
    assert(st.best_rate.temp == 88);

    t.hashrate = 2000000;
    for (int i = 0; i < 3; i++)
        assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.current_pll[1] == 400 - 3 * 6);
    assert(due(&st, &t, &f, &v) == AGING_ACT_SET_PLL_BEST);
    assert(f == st.best_rate.freq[1]);
    assert(v == st.best_rate.volt - 26);

    assert(due(&st, &t, &f, &v) == AGING_ACT_DONE);

    aging_reset(&st, 100);
    t = T(9000000, 92, 3700);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.best_rate.hashrate_Mhz == 0);
    t = T(9000000, 87, 3700);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.best_rate.hashrate_Mhz == 9000000);
    t = T(20000000, 20 * AGING_MIN_PE - 1, 3700);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.best_rate.hashrate_Mhz == 9000000);
    t = T(10000000, 88, 3700);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.best_rate.hashrate_Mhz == 10000000);
    assert(st.best_pe.pe == aging_pe(88, 10000000));
    t = T(11000000, 91, 3700);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.best_rate.hashrate_Mhz == 11000000);
    t = T(11000000, AGING_MIN_PE * 11 - 1, 3700);
    assert(due(&st, &t, &f, &v) == AGING_ACT_SET_PLL_START);
    assert(st.best_pe.pe == aging_pe(91, 11000000));
    assert(st.best_pe.hashrate_Mhz == 11000000);

    aging_reset(&st, 100);
    aging_pll_fill(st.current_pll, &P, 400);
    t = T(3000000, 92, 3700);
    for (int i = 0; i < 6; i++)
        assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(due(&st, &t, &f, &v) == AGING_ACT_SET_PLL_START);
    for (int i = 0; i < 3; i++)
        assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(due(&st, &t, &f, &v) == AGING_ACT_SET_PLL_BEST);
    assert(st.best_rate.hashrate_Mhz == 0 && st.best_pe.pe == 999999);
    assert(f == 0 && v <= 0);
    assert(due(&st, &t, &f, &v) == AGING_ACT_DONE);

    aging_reset(&st, 100);
    aging_pll_fill(st.current_pll, &P, 400);
    t = T(3000000, 90, 3700);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.measured == 0);
    t = T(3200000, 85, 3650);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.measured == 1);
    assert(st.last.hashrate_Mhz == 3200000 && st.last.pout == 85 && st.last.volt == 3650);
    assert(st.last.temp == 88 && st.last.pe == aging_pe(85, 3200000));
    assert(st.last.freq[1] == 406 && st.last.freq[2] == 409);
    t.power_ok = 0;
    assert(due(&st, &t, &f, &v) == AGING_ACT_NONE);
    assert(st.measured == 0);
    t = T(3300000, 86, 3600);
    assert(due(&st, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.measured == 2 && st.last.hashrate_Mhz == 3300000 && st.last.freq[1] == 412);

    aging_reset(&st, 100);
    t          = T(3000000, 90, 3700);
    t.power_ok = 0;
    for (int i = 0; i < 600; i++)
        assert(aging_rule_tick(&st, &P, &t, &f, &v) == AGING_ACT_NONE);
    assert(st.enum_count == 0 && st.current_pll[1] == 100);
    assert(st.measured == 0 && st.last.hashrate_Mhz == 0);

    aging_reset(&st, 100);
    t = T(3000000, 90, 3700);
    for (int i = 0; i < 14; i++)
        assert(aging_rule_tick(&st, &P_LONG, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(aging_rule_tick(&st, &P_LONG, &t, &f, &v) == AGING_ACT_SET_PLL_START);
    for (int i = 0; i < 3; i++)
        assert(aging_rule_tick(&st, &P_LONG, &t, &f, &v) == AGING_ACT_MOVE_WINDOW);
    assert(st.enum_count == 18 && st.current_pll[1] == 400 - 3 * 6);
    t = T(5000000, 90, 3650);
    assert(aging_rule_tick(&st, &P_LONG, &t, &f, &v) == AGING_ACT_SET_PLL_BEST);
    assert(st.best_rate.hashrate_Mhz == 5000000);
    assert(f == 400 - 3 * 6 && v == 3650 - 26);
    assert(st.best_pe.pe == aging_pe(90, 5000000));

    printf("aging rule goldens OK\n");
    return 0;
}
