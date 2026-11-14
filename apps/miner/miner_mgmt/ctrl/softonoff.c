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

#include "softonoff.h"

softonoff_action softonoff_thermal_step(softonoff_state_t *st, int overheat_req, uint64_t now_ms,
        uint32_t hold_ms)
{
    if (overheat_req && !st->thermal_until) {
        st->thermal_until = now_ms + hold_ms;
        return SOFTONOFF_THERMAL_OFF;
    }
    if (st->thermal_until && (int64_t)(now_ms - st->thermal_until) >= 0) {
        st->thermal_until = 0;
        return st->api_softoff ? SOFTONOFF_THERMAL_TO_SOFTOFF : SOFTONOFF_THERMAL_RESUME;
    }
    return SOFTONOFF_NONE;
}

softonoff_action softonoff_api_check(const softonoff_state_t *st, int fault_off, int want_on)
{
    if (st->thermal_until && want_on)
        return SOFTONOFF_API_IGNORED;
    if (!want_on && !st->api_softoff)
        return st->thermal_until ? SOFTONOFF_API_OFF_HELD : SOFTONOFF_API_OFF;
    if (want_on && st->api_softoff)
        return SOFTONOFF_API_ON;
    return want_on && fault_off ? SOFTONOFF_API_FAULT_HELD : SOFTONOFF_NONE;
}

softonoff_action softonoff_api_step(softonoff_state_t *st, int want_on)
{
    softonoff_action a = softonoff_api_check(st, 0, want_on);

    if (a == SOFTONOFF_API_OFF || a == SOFTONOFF_API_OFF_HELD)
        st->api_softoff = 1;
    else if (a == SOFTONOFF_API_ON)
        st->api_softoff = 0;
    return a;
}

int softonoff_intentional_exit(const softonoff_state_t *st, int is_asic)
{
    return is_asic && (st->thermal_until != 0 || st->api_softoff);
}

int softonoff_fan_hold(const softonoff_state_t *st, int fault_off)
{
    return (softonoff_children_stopped(st, fault_off) & SOFTONOFF_CHILD_ASIC) != 0;
}

unsigned softonoff_children_stopped(const softonoff_state_t *st, int fault_off)
{
    return (st->thermal_until || st->api_softoff || fault_off || st->start_wait) ?
            SOFTONOFF_CHILD_ASIC :
            0;
}
