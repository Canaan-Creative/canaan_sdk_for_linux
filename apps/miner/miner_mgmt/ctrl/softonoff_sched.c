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

#include "softonoff_sched.h"

static int crossed(int64_t ts, int64_t prev_s, int64_t now_s)
{
    return now_s >= ts - SOFT_SCHED_WINDOW_S && prev_s < ts + SOFT_SCHED_WINDOW_S;
}

void soft_sched_arm(soft_sched_t *st, int is_on, uint8_t valid, int64_t ts)
{
    if (!st)
        return;
    if (is_on) {
        st->on_valid = valid ? 1 : 0;
        st->on_ts    = ts;
    } else {
        st->off_valid = valid ? 1 : 0;
        st->off_ts    = ts;
    }
}

soft_sched_action soft_sched_due(soft_sched_t *st, int64_t now_s, int mining)
{
    if (!st)
        return SOFT_SCHED_NONE;

    int64_t prev_s = st->checked && st->checked_s < now_s ? st->checked_s : now_s - 1;
    st->checked    = 1;
    st->checked_s  = now_s;

    if (mining && st->off_valid && crossed(st->off_ts, prev_s, now_s)) {
        st->off_valid = 0;
        return SOFT_SCHED_OFF;
    }
    if (!mining && st->on_valid && crossed(st->on_ts, prev_s, now_s)) {
        st->on_valid = 0;
        return SOFT_SCHED_ON;
    }
    return SOFT_SCHED_NONE;
}
