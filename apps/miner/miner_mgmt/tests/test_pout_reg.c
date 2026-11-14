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
#include "pout_reg.h"

static const pout_reg_params_t NANO3S = {
    .up_limit           = 1,
    .down_limit         = -2,
    .pout_min           = 50,
    .vol_min            = 3200,
    .vol_max            = 4000,
    .volt_step          = 26,
    .abnormal_pout      = 150,
    .stab_thresh_normal = 10,
    .stab_thresh_aged   = 120,
};

static const pout_reg_params_t X3 = {
    .up_limit           = 7,
    .down_limit         = -7,
    .pout_min           = 50,
    .vol_min            = 2400,
    .vol_max            = 3000,
    .volt_step          = 26,
    .abnormal_pout      = 900,
    .stab_thresh_normal = 10,
    .stab_thresh_aged   = 120,
};

static pout_reg_telem_t T(uint16_t pout, short voutcmd)
{
    pout_reg_telem_t t;
    memset(&t, 0, sizeof(t));
    t.tuning    = 1;
    t.pout      = pout;
    t.vout      = 3700;
    t.voutcmd   = voutcmd;
    t.poutlimit = 100;
    return t;
}

static void x3_bench_point(void)
{
    pout_reg_state_t st = { .stage = POUT_STAGE_FIT_POUT, .max_pout = 300 };
    pout_reg_telem_t t;
    short            v    = 0;
    short            rail = 2410;

    for (int i = 0; i < 40; i++) {
        t           = T(163, rail);
        t.vout      = 2409;
        t.poutlimit = 300;
        v           = 0;
        assert(pout_reg_step(&st, &X3, &t, &v) == 1);
        assert(v == (short)(rail + 26) || v == 3000);
        assert(v >= rail);
        rail = v;
    }
    assert(rail == 3000);

    t           = T(320, 3000);
    t.vout      = 2999;
    t.poutlimit = 300;
    v           = 0;
    assert(pout_reg_step(&st, &X3, &t, &v) == 1 && v == 2974);
}

static void x3_regulates(void)
{
    int acted = 0, in_band = 0;
    for (short mv = 2400; mv <= 3000; mv += 10) {
        for (short pout = 160; pout <= 360; pout++) {
            pout_reg_state_t st = { .stage = POUT_STAGE_FIT_POUT, .max_pout = 300 };
            pout_reg_telem_t t  = T((uint16_t)pout, mv);
            t.vout              = (uint16_t)(mv - 1);
            t.poutlimit         = 300;
            short v             = 0;
            if (pout_reg_step(&st, &X3, &t, &v) == 0) {
                assert(pout >= 293 && pout <= 307);
                in_band++;
            } else {
                short want = pout > 307 ? (short)(mv - 26) : (short)(mv + 26);
                if (want < 2400)
                    want = 2400;
                if (want > 3000)
                    want = 3000;
                assert(v == want);
                acted++;
            }
        }
    }
    assert(in_band == 61 * 15 && acted == 61 * 186);
}

static int guard_reachable(const pout_reg_params_t *pp, long max_pout)
{
    for (long pout = 0; pout <= 65535; pout++)
        if (pout < max_pout + pp->down_limit && pout > pp->abnormal_pout)
            return 1;
    return 0;
}

static void increase_unguarded(const pout_reg_params_t *pp, short max_pout)
{
    for (short mv = pp->vol_min; mv <= pp->vol_max; mv++) {
        for (short pout = 1; pout < (short)(max_pout + pp->down_limit); pout++) {
            pout_reg_state_t st = { .stage = POUT_STAGE_FIT_POUT, .max_pout = (uint16_t)max_pout };
            pout_reg_telem_t t  = T((uint16_t)pout, mv);
            t.poutlimit         = (uint16_t)max_pout;
            short v             = 0;
            assert(pout_reg_step(&st, pp, &t, &v) == 1);
            assert(v >= mv && v >= pp->vol_min && v <= pp->vol_max);
        }
    }
}

static void budget_floor_steady(void)
{
    static const uint16_t LOW[]    = { 0, 1, 30, 49 };
    static const short    STAGES[] = { POUT_STAGE_MAX_LIMIT_ONLY, POUT_STAGE_FIT_POUT };

    for (size_t s = 0; s < sizeof STAGES / sizeof *STAGES; s++) {
        for (size_t i = 0; i < sizeof LOW / sizeof *LOW; i++) {
            pout_reg_state_t st = { .stage = STAGES[s], .max_pout = LOW[i] };
            pout_reg_telem_t t  = T(50, 3700);
            short            v  = 0;

            t.poutlimit = LOW[i];
            for (int k = 0; k < 20; k++) {
                pout_reg_step(&st, &NANO3S, &t, &v);
                assert(st.max_pout == 50);
            }
        }

        {
            pout_reg_state_t st = { .stage = STAGES[s], .max_pout = 90 };
            pout_reg_telem_t t  = T(50, 3700);
            short            v  = 0;

            t.poutlimit = 50;
            for (int k = 0; k < 20; k++) {
                pout_reg_step(&st, &NANO3S, &t, &v);
                assert(st.max_pout == 50);
            }
            st.max_pout = 90;
            t.poutlimit = 80;
            pout_reg_step(&st, &NANO3S, &t, &v);
            assert(st.max_pout == 80);
            st.max_pout = 30;
            pout_reg_step(&st, &NANO3S, &t, &v);
            assert(st.max_pout == 50);
            st.max_pout = 70;
            pout_reg_step(&st, &NANO3S, &t, &v);
            assert(st.max_pout == 70);
        }
    }
}

int main(void)
{
    pout_reg_state_t st = { .stage = POUT_STAGE_MAX_LIMIT_ONLY, .max_pout = 90 };
    pout_reg_telem_t t;
    short            v = 0;

    t = T(95, 3700);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == 3674);
    assert(st.stage == POUT_STAGE_MAX_LIMIT_ONLY && st.max_pout == 90);

    t = T(90, 3674);
    for (int i = 0; i < 8; i++)
        assert(pout_reg_step(&st, &NANO3S, &t, &v) == 0 && st.stage == POUT_STAGE_MAX_LIMIT_ONLY);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 0);
    assert(st.stage == POUT_STAGE_FIT_POUT && st.stability == 10);

    t = T(89, 3674);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 0);
    t = T(92, 3674);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == 3648);
    t = T(85, 3674);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == 3700);

    t = T(92, 3210);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == 3200);
    t = T(85, 3990);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == 4000);

    t = T(92, 4100);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == -1);

    t = T(200, 3700);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == -1);

    {
        pout_reg_state_t g  = { .stage = POUT_STAGE_FIT_POUT, .max_pout = 3400 };
        pout_reg_telem_t gt = T(200, 3700);
        gt.poutlimit        = 3400;
        short gv            = 0;
        assert(pout_reg_step(&g, &NANO3S, &gt, &gv) == 1 && gv == -1);
    }

    t           = T(89, 3700);
    t.poutlimit = 80;
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 1 && v == 3674);
    assert(st.max_pout == 80);

    t = T(0, 3700);
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 0);
    t      = T(90, 3700);
    t.vout = 0;
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 0);

    st.stage     = POUT_STAGE_MAX_LIMIT_ONLY;
    st.stability = 5;
    t            = T(95, 3700);
    t.tuning     = 0;
    assert(pout_reg_step(&st, &NANO3S, &t, &v) == 0 && st.stage == POUT_STAGE_MAX_LIMIT_ONLY);
    assert(st.stability == 5);

    x3_bench_point();
    x3_regulates();

    static const short NANO3S_BUDGETS[] = { 62, 95, 133, 150 };
    for (size_t i = 0; i < sizeof NANO3S_BUDGETS / sizeof *NANO3S_BUDGETS; i++) {
        assert(!guard_reachable(&NANO3S, NANO3S_BUDGETS[i]));
        increase_unguarded(&NANO3S, NANO3S_BUDGETS[i]);
    }
    static const short X3_BUDGETS[] = { 180, 240, 300, 900 };
    for (size_t i = 0; i < sizeof X3_BUDGETS / sizeof *X3_BUDGETS; i++) {
        assert(!guard_reachable(&X3, X3_BUDGETS[i]));
        increase_unguarded(&X3, X3_BUDGETS[i]);
    }
    assert(guard_reachable(&NANO3S, 3400));
    budget_floor_steady();

    printf("pout-reg rule goldens OK\n");
    return 0;
}
