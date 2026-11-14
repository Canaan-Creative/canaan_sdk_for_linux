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

#include "pout_reg.h"

#define IS_OVER_THRESHOLD(x, threshold)  ((x) > (threshold))
#define IS_LOWER_THRESHOLD(x, threshold) ((x) < (threshold))
#define IS_BETWEEN(val, min, max)        ((val) >= (min) && (val) <= (max))
#define CLAMP(v, lo, hi)                 ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

static short decrease_voltage_by_pout(const pout_reg_params_t *pp, const pout_reg_telem_t *t)
{
    short cur_pout    = (short)t->pout;
    short cur_voltage = t->voutcmd;
    if ((cur_voltage > pp->vol_max) || (cur_voltage < pp->vol_min) ||
            (cur_pout > pp->abnormal_pout))
        return -1;

    cur_voltage -= pp->volt_step;

    if (cur_voltage < pp->vol_min)
        cur_voltage = pp->vol_min;

    return cur_voltage;
}

static short increase_voltage_by_pout(const pout_reg_params_t *pp, const pout_reg_telem_t *t)
{
    short cur_pout    = (short)t->pout;
    short cur_voltage = t->voutcmd;
    if ((cur_voltage > pp->vol_max) || (cur_voltage < pp->vol_min) ||
            (cur_pout > pp->abnormal_pout))
        return -1;

    cur_voltage += pp->volt_step;

    if (cur_voltage > pp->vol_max)
        cur_voltage = pp->vol_max;

    return cur_voltage;
}

int pout_reg_step(pout_reg_state_t *st, const pout_reg_params_t *pp, const pout_reg_telem_t *t,
        short *volt_out)
{
    short    cur_pout = (short)t->pout;
    uint16_t stability_threshold;
    uint16_t pout_hi;
    short    volt_pout = t->voutcmd;
    short    volt_cap  = t->volt_cap > st->start_volt ? t->volt_cap : st->start_volt;

    if (!t->tuning)
        return 0;

    stability_threshold = t->aging_finished ? pp->stab_thresh_aged : pp->stab_thresh_normal;

    if ((!t->pout) || (!t->vout))
        return 0;

    pout_hi = t->poutlimit > pp->pout_min ? t->poutlimit : pp->pout_min;

    switch (st->stage) {
    case POUT_STAGE_MAX_LIMIT_ONLY:
        st->stability++;
        st->max_pout = CLAMP(st->max_pout, pp->pout_min, pout_hi);
        if (volt_cap > pp->vol_max)
            volt_cap = pp->vol_max;
        if (IS_OVER_THRESHOLD(cur_pout, (short)st->max_pout + pp->up_limit)) {
            volt_pout = decrease_voltage_by_pout(pp, t);
            if (IS_BETWEEN(volt_pout, pp->vol_min, pp->vol_max)) {
                *volt_out = volt_pout;
                return 1;
            }
        } else if (st->stability >= stability_threshold) {
            st->stage = POUT_STAGE_FIT_POUT;
        } else if (IS_LOWER_THRESHOLD(cur_pout, (short)st->max_pout + pp->down_limit) &&
                t->voutcmd < volt_cap) {
            volt_pout = increase_voltage_by_pout(pp, t);
            if (volt_pout > volt_cap)
                volt_pout = volt_cap;
            if (IS_BETWEEN(volt_pout, pp->vol_min, pp->vol_max)) {
                *volt_out = volt_pout;
                return 1;
            }
        }
        break;
    case POUT_STAGE_FIT_POUT:
        st->max_pout = CLAMP(st->max_pout, pp->pout_min, pout_hi);
        if (!IS_BETWEEN(cur_pout, ((short)st->max_pout + pp->down_limit),
                    ((short)st->max_pout + pp->up_limit))) {
            if (IS_OVER_THRESHOLD(cur_pout, ((short)st->max_pout + pp->up_limit))) {
                volt_pout = decrease_voltage_by_pout(pp, t);
            } else if (IS_LOWER_THRESHOLD(cur_pout, (short)st->max_pout + pp->down_limit)) {
                volt_pout = increase_voltage_by_pout(pp, t);
            }
            *volt_out = volt_pout;
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}
