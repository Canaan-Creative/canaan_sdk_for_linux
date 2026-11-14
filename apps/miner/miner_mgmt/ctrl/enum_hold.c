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

#include "enum_hold.h"

void enum_hold_rearm(enum_hold_t *eh)
{
    eh->phase = ENUM_HOLD_PHASE_ARMED;
}

enum_hold_action enum_hold_step(enum_hold_t *eh, int asic_live, int enum_duty_pct)
{
    if (enum_duty_pct <= 0) {
        eh->phase = ENUM_HOLD_PHASE_NONE;
        return ENUM_HOLD_NONE;
    }
    switch (eh->phase) {
    case ENUM_HOLD_PHASE_ARMED:
        eh->phase = ENUM_HOLD_PHASE_STALE;
        return ENUM_HOLD_PIN;
    case ENUM_HOLD_PHASE_STALE:
        if (!asic_live)
            eh->phase = ENUM_HOLD_PHASE_LIVE;
        return ENUM_HOLD_NONE;
    case ENUM_HOLD_PHASE_LIVE:
        if (!asic_live)
            return ENUM_HOLD_NONE;
        eh->phase = ENUM_HOLD_PHASE_NONE;
        return ENUM_HOLD_RELEASE;
    case ENUM_HOLD_PHASE_NONE:
    default:
        return ENUM_HOLD_NONE;
    }
}

int enum_hold_active(const enum_hold_t *eh, int held_off)
{
    if (held_off)
        return 0;
    return eh->phase == ENUM_HOLD_PHASE_STALE || eh->phase == ENUM_HOLD_PHASE_LIVE;
}
