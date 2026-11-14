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
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fan_pid.h"
#include "pout_reg.h"

#define T_AMB         25.0
#define HEAT_CAP_J_K  400.0
#define G_BASE_W_K    0.6
#define G_FAN_W_K     4.7
#define INLET_BLOCKED 0.45
#define P_ASIC_REF_W  130.0
#define V_REF         3450.0
#define LEAK_PER_K    0.004
#define P_MISC_W      2.0
#define TARGET_TEMP   90
#define BUDGET_W      133
#define CALI_VOLT     3450
#define BLOCK_AT_S    60
#define UNBLOCK_AT_S  300
#define SETTLED_AT_S  540
#define STAB_WINDOW_S 720
#define RUN_S         1500

typedef struct {
    double temp;
    short  rail;
    int    duty_x10;
    int    pout;
} plant_t;

typedef struct {
    int    max_duty_blocked;
    int    pout_settled;
    int    duty_settled;
    double temp_settled;
    int    pout_end;
    int    stage_settled;
} run_result_t;

static int g_dev_duty_x10 = -1;

static const fan_pid_params_t NANO3S_FAN = {
    .p              = 35,
    .i              = 1,
    .d              = 1,
    .temp_max       = 100,
    .temp_min       = 50,
    .fan_min        = 154,
    .fan_max        = 0x3FF,
    .dlimit_up      = 50,
    .dlimit_down    = -100,
    .duty_init_pct  = 25,
    .duty_max_pct   = 100,
    .err_quant      = 1,
    .duty_whole_pct = 0,
    .instart_hold   = 0,
};

static const pout_reg_params_t NANO3S_POUT = {
    .up_limit           = 1,
    .down_limit         = -2,
    .pout_min           = 50,
    .vol_min            = 3200,
    .vol_max            = 4000,
    .volt_step          = 26,
    .abnormal_pout      = 155,
    .stab_thresh_normal = 10,
    .stab_thresh_aged   = 120,
};

static double asic_w(const plant_t *p)
{
    double v = p->rail / V_REF;

    return P_ASIC_REF_W * v * v * (1.0 + LEAK_PER_K * (p->temp - TARGET_TEMP));
}

static double fan_w(int duty_x10)
{
    double d = duty_x10 / 1000.0;

    return 0.5 + 13.0 * d * d * d;
}

static void plant_step(plant_t *p, double inlet)
{
    double d  = p->duty_x10 / 1000.0;
    double g  = G_BASE_W_K + G_FAN_W_K * d * inlet;
    double pa = asic_w(p);

    p->temp += (pa - g * (p->temp - T_AMB)) / HEAT_CAP_J_K;
    p->pout = (int)lround(pa + fan_w(p->duty_x10) + P_MISC_W);
}

static void run(short volt_cap, run_result_t *r)
{
    plant_t          p  = { .temp = TARGET_TEMP, .rail = CALI_VOLT, .duty_x10 = 300 };
    fan_pid_state_t  fs = { .fan_val = 307, .last_target = TARGET_TEMP, .fixed_duty = -1 };
    pout_reg_state_t ps = { .stage = POUT_STAGE_MAX_LIMIT_ONLY, .max_pout = BUDGET_W };

    memset(r, 0, sizeof *r);
    g_dev_duty_x10 = p.duty_x10;
    p.pout         = (int)lround(asic_w(&p) + fan_w(p.duty_x10) + P_MISC_W);
    for (int s = 0; s < RUN_S; s++) {
        double inlet = (s >= BLOCK_AT_S && s < UNBLOCK_AT_S) ? INLET_BLOCKED : 1.0;

        plant_step(&p, inlet);
        if (s % 2 == 0) {
            int x10 = 0, ch = 0;

            fan_pid_step(&fs, &NANO3S_FAN, (float)lround(p.temp), TARGET_TEMP, g_dev_duty_x10, &x10,
                    &ch);
            if (ch)
                g_dev_duty_x10 = x10;
            p.duty_x10 = x10;
        }
        if (s % 6 == 0) {
            pout_reg_telem_t t = {
                .tuning         = 1,
                .aging_finished = 1,
                .pout           = (uint16_t)p.pout,
                .vout           = 28000,
                .poutlimit      = BUDGET_W,
                .voutcmd        = p.rail,
                .volt_cap       = volt_cap,
            };
            short v = 0;

            if (pout_reg_step(&ps, &NANO3S_POUT, &t, &v) == 1 && v > 0)
                p.rail = v;
        }
        if (inlet < 1.0 && p.duty_x10 > r->max_duty_blocked)
            r->max_duty_blocked = p.duty_x10;
        if (s == SETTLED_AT_S) {
            r->pout_settled  = p.pout;
            r->duty_settled  = p.duty_x10;
            r->temp_settled  = p.temp;
            r->stage_settled = ps.stage;
        }
        assert(p.temp < 113.0);
        assert(p.pout <= BUDGET_W + 12);
    }
    r->pout_end = p.pout;
}

static void report(const char *name, const run_result_t *r)
{
    printf("%s: blocked max duty %d.%d%%, at %d s: %d W, duty %d.%d%%, %.1f degC, "
           "stage %d; at %d s: %d W\n",
            name, r->max_duty_blocked / 10, r->max_duty_blocked % 10, SETTLED_AT_S, r->pout_settled,
            r->duty_settled / 10, r->duty_settled % 10, r->temp_settled, r->stage_settled, RUN_S,
            r->pout_end);
}

static void blocked_inlet_recovers(void)
{
    run_result_t r;

    run(CALI_VOLT, &r);
    report("volt_cap", &r);
    assert(SETTLED_AT_S < STAB_WINDOW_S);
    assert(r.stage_settled == POUT_STAGE_MAX_LIMIT_ONLY);
    assert(r.max_duty_blocked > 500);
    assert(r.pout_settled >= BUDGET_W - 2 && r.pout_settled <= BUDGET_W + 1);
    assert(fabs(r.temp_settled - TARGET_TEMP) <= 2.0);
    assert(r.pout_end >= BUDGET_W - 2 && r.pout_end <= BUDGET_W + 1);
}

static void without_cap_rail_stays_lowered(void)
{
    run_result_t r;

    run(0, &r);
    report("no volt_cap", &r);
    assert(r.max_duty_blocked > 500);
    assert(r.pout_settled < BUDGET_W - 2);
    assert(r.pout_end >= BUDGET_W - 2 && r.pout_end <= BUDGET_W + 1);
}

static void cap_never_exceeded(void)
{
    pout_reg_state_t st = { .stage = POUT_STAGE_MAX_LIMIT_ONLY, .max_pout = BUDGET_W };
    pout_reg_telem_t t  = {
         .tuning         = 1,
         .aging_finished = 1,
         .pout           = 120,
         .vout           = 28000,
         .poutlimit      = BUDGET_W,
         .voutcmd        = CALI_VOLT - 10,
         .volt_cap       = CALI_VOLT,
    };
    short v = 0;

    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 1 && v == CALI_VOLT);
    t.voutcmd = CALI_VOLT;
    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 0);
    t.voutcmd = CALI_VOLT + 26;
    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 0);
    t.voutcmd = CALI_VOLT - 26;
    t.pout    = BUDGET_W - 2;
    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 0);
    assert(st.stage == POUT_STAGE_MAX_LIMIT_ONLY);
}

static void start_rail_raises_cap(void)
{
    pout_reg_state_t st = {
        .stage      = POUT_STAGE_MAX_LIMIT_ONLY,
        .max_pout   = BUDGET_W,
        .start_volt = 3736,
    };
    pout_reg_telem_t t = {
        .tuning         = 1,
        .aging_finished = 1,
        .pout           = 120,
        .vout           = 28000,
        .poutlimit      = BUDGET_W,
        .voutcmd        = 3684,
        .volt_cap       = CALI_VOLT,
    };
    short v = 0;

    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 1 && v == 3710);
    t.voutcmd = 3720;
    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 1 && v == 3736);
    t.voutcmd = 3736;
    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 0);
    st.start_volt = NANO3S_POUT.vol_max + 100;
    t.voutcmd     = NANO3S_POUT.vol_max - 10;
    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 1 && v == NANO3S_POUT.vol_max);
    t.voutcmd = NANO3S_POUT.vol_max;
    assert(pout_reg_step(&st, &NANO3S_POUT, &t, &v) == 0);
    assert(st.stage == POUT_STAGE_MAX_LIMIT_ONLY);
}

int main(void)
{
    blocked_inlet_recovers();
    without_cap_rail_stays_lowered();
    cap_never_exceeded();
    start_rail_raises_cap();
    printf("fan_power_loop: ok\n");
    return 0;
}
