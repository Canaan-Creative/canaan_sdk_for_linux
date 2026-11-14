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

#include "thermal.h"

int thermal_softoff_check(const thermal_params_t *pp, thermal_softoff_state_t *st, int temp_summax,
        const int *outlet_temp, int n_outlet, int in_work)
{
    int hot = (temp_summax > pp->over_temp);

    if (pp->outlet_over_count > 0) {
        int omax = ASIC_TEMP_NONE;
        for (int i = 0; i < n_outlet; i++)
            if (outlet_temp[i] > omax)
                omax = outlet_temp[i];

        if (omax > pp->outlet_over_temp) {
            if (st->outlet_hot_run < pp->outlet_over_count)
                st->outlet_hot_run++;
        } else {
            st->outlet_hot_run = 0;
        }
        if (st->outlet_hot_run >= pp->outlet_over_count)
            hot = 1;
    }

    if (!in_work)
        return 0;
    if (hot)
        st->outlet_hot_run = 0;
    return hot;
}

thermal_telem_verdict thermal_telem_check(thermal_telem_state_t *st, int asic_live, int temp_valid,
        uint64_t now_ms, uint32_t grace_ms)
{
    if (asic_live && temp_valid) {
        st->ok_ms    = now_ms;
        st->failsafe = 0;
        return THERMAL_TELEM_OK;
    }

    if (st->ok_ms == 0) {
        st->ok_ms = now_ms;
        return THERMAL_TELEM_HOLD;
    }
    if (now_ms - st->ok_ms > grace_ms) {
        st->failsafe = 1;
        return THERMAL_TELEM_FAILSAFE;
    }
    return THERMAL_TELEM_HOLD;
}

thermal_coverage_verdict thermal_coverage_check(thermal_coverage_state_t *st, int valid_chips,
        int chips, int tavg, uint64_t now_ms, uint32_t ripen_ms)
{
    if (chips <= 0 || tavg <= ASIC_TEMP_NONE) {
        st->ripe_ms = 0;
        return THERMAL_COVERAGE_QUIET;
    }
    if (st->ripe_ms == 0)
        st->ripe_ms = now_ms;
    if (now_ms - st->ripe_ms <= ripen_ms)
        return THERMAL_COVERAGE_QUIET;

    uint32_t incomplete = valid_chips < chips;

    if (fault_report_step(&st->rep, incomplete) == FAULT_REPORT_NONE)
        return THERMAL_COVERAGE_QUIET;
    return incomplete ? THERMAL_COVERAGE_SHORT : THERMAL_COVERAGE_FULL;
}
