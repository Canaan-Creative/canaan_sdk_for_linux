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

#include "fan_health.h"

int fan_health_stopped_mask(const int *rpm, int n)
{
    int mask = 0;

    if (n > FAN_HEALTH_MAX_FANS)
        n = FAN_HEALTH_MAX_FANS;
    for (int i = 0; i < n; i++)
        if (rpm[i] >= 0 && rpm[i] < FAN_STALL_RPM)
            mask |= 1 << i;
    return mask;
}

fan_health_verdict fan_health_check(const fan_health_params_t *pp, fan_health_state_t *st,
        const int *rpm, int n, int duty_pct, int in_work)
{
    if (pp->err_count <= 0 || duty_pct <= 0 || !fan_health_stopped_mask(rpm, n)) {
        st->bad_run = 0;
        return FAN_HEALTH_OK;
    }

    if (st->bad_run < pp->err_count)
        st->bad_run++;
    if (!in_work)
        return FAN_HEALTH_BAD;
    return st->bad_run >= pp->err_count ? FAN_HEALTH_TRIP : FAN_HEALTH_BAD;
}
