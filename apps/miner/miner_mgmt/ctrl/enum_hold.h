/* SPDX-License-Identifier: MIT */
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

/* "enum" here means ASIC enumeration, not a C enum. */
#ifndef ENUM_HOLD_H
#define ENUM_HOLD_H

#include <stdint.h>

enum {
    ENUM_HOLD_PHASE_NONE = 0, /* no fan pin: before the first spawn, after the
                               * release, or the enum duty is 0               */
    ENUM_HOLD_PHASE_ARMED,    /* ASIC child spawned; pin on the next tick     */
    ENUM_HOLD_PHASE_STALE,    /* pinned; asic_live may still be the dead
                               * child's, so wait for it to read 0            */
    ENUM_HOLD_PHASE_LIVE,     /* pinned; release when this child is live      */
};

/* Caller-owned; zero-init means nothing is pinned. */
typedef struct enum_hold {
    uint8_t phase; /* ENUM_HOLD_PHASE_* */
} enum_hold_t;

typedef enum {
    ENUM_HOLD_NONE = 0,
    ENUM_HOLD_PIN,     /* enumeration started: pin the enum duty */
    ENUM_HOLD_RELEASE, /* enumeration succeeded: fan to PID */
} enum_hold_action;

void             enum_hold_rearm(enum_hold_t *eh);
enum_hold_action enum_hold_step(enum_hold_t *eh, int asic_live, int enum_duty_pct);
int              enum_hold_active(const enum_hold_t *eh, int held_off);

#endif /* ENUM_HOLD_H */
