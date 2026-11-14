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

#include <string.h>
#include "aging.h"

unsigned aging_pe(uint32_t pout, uint32_t hashrate)
{
    if (hashrate < 1000)
        return 999999;

    hashrate /= 1000; /* MH/s -> GH/s, 1 GH/s resolution */
    return (unsigned int)((uint64_t)pout * 1000000 / hashrate);
}

void aging_pll_fill(uint16_t *pll, const aging_params_t *pp, uint16_t freq)
{
    pll[pp->pll_offset] = freq;
    for (int i = pp->pll_offset + 1; i < AGING_PLL_COUNT; i++) {
        pll[i] = pll[(i - 1)] + pp->pll_interval;
    }
}

void aging_reset(aging_state_t *st, uint16_t init_freq)
{
    memset(st, 0, sizeof(*st));
    st->best_pe.pe   = 999999;
    st->best_rate.pe = 999999;
    for (int i = 0; i < AGING_PLL_COUNT; i++)
        st->current_pll[i] = init_freq;
    st->move_direction  = 1;
    st->aging_direction = 1;
}

static void record_set(aging_rec_t *rec, const aging_state_t *st, const aging_telem_t *t,
        short cur_pout, int cur_hash_rate, unsigned cur_hash_pe)
{
    memcpy(rec->freq, st->current_pll, sizeof(st->current_pll));
    rec->volt         = t->voutcmd;
    rec->hashrate_Mhz = cur_hash_rate;
    rec->pe           = cur_hash_pe;
    rec->pout         = cur_pout;
    rec->temp         = t->temp;
}

static void record_best(aging_state_t *st, const aging_params_t *pp, const aging_telem_t *t,
        short cur_pout, int cur_hash_rate, unsigned cur_hash_pe)
{
    if (cur_pout < AGING_MIN_PE * (cur_hash_rate / 1000000))
        return;
    if (cur_pout <= (short)t->max_pout + pp->pout_up_limit) {
        if (cur_hash_rate > st->best_rate.hashrate_Mhz)
            record_set(&st->best_rate, st, t, cur_pout, cur_hash_rate, cur_hash_pe);
        if (cur_hash_pe < st->best_pe.pe)
            record_set(&st->best_pe, st, t, cur_pout, cur_hash_rate, cur_hash_pe);
    }
}

static int enum_step(aging_state_t *st, const aging_params_t *pp, const aging_telem_t *t,
        int direction)
{
    unsigned cur_hash_pe;
    short    cur_pout = (short)t->pout;
    uint8_t  adj_need_count;

    if (!t->power_ok) {
        return 0;
    }

    st->progress = 10 + 70 * st->enum_count / (pp->up_stages + pp->down_stages + 1);

    st->move_direction = direction;
    if (st->enum_count > 0) {
        cur_hash_pe = aging_pe(cur_pout, t->hashrate);
        record_best(st, pp, t, cur_pout, t->hashrate, cur_hash_pe);
        record_set(&st->last, st, t, cur_pout, t->hashrate, cur_hash_pe);
        st->measured = st->enum_count;
    }

    adj_need_count = (st->move_direction == 1) ? pp->up_stages :
                                                 (pp->up_stages + pp->down_stages + 1);
    if (st->enum_count++ < adj_need_count) {
        aging_pll_fill(st->current_pll, pp,
                st->current_pll[pp->pll_offset] + (pp->freq_step * direction));
        return 0;
    }
    return 1;
}

aging_action_t aging_rule_tick(aging_state_t *st, const aging_params_t *pp, const aging_telem_t *t,
        uint16_t *freq_out, short *volt_out)
{
    st->measured = 0;
    if (st->aging_direction == 0) {
        st->aging_direction = 1;
        return AGING_ACT_DONE;
    }
    if (enum_step(st, pp, t, st->aging_direction)) {
        if (st->aging_direction == 1) {
            st->aging_direction = -1;
            *freq_out           = t->pll_start;
            aging_pll_fill(st->current_pll, pp, t->pll_start);
            return AGING_ACT_SET_PLL_START;
        } else {
            st->aging_direction = 0;
            *freq_out           = st->best_rate.freq[pp->pll_offset];
            aging_pll_fill(st->current_pll, pp, *freq_out);
            *volt_out = st->best_rate.volt - RAIL_VOLT_STEP;
            return AGING_ACT_SET_PLL_BEST;
        }
    }
    if (!t->power_ok)
        return AGING_ACT_NONE;
    return AGING_ACT_MOVE_WINDOW;
}
