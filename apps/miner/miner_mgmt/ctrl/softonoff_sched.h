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

#ifndef SOFTONOFF_SCHED_H
#define SOFTONOFF_SCHED_H

#include <stdint.h>

#define SOFT_SCHED_WINDOW_S 2 /* half-width of the window around the scheduled time, s */

typedef enum {
    SOFT_SCHED_NONE = 0,
    SOFT_SCHED_ON,
    SOFT_SCHED_OFF,
} soft_sched_action;

typedef struct soft_sched {
    uint8_t on_valid, off_valid;
    int64_t on_ts, off_ts; /* soft-on and soft-off times, Unix s */
    uint8_t checked;
    int64_t checked_s;
} soft_sched_t;

void              soft_sched_arm(soft_sched_t *st, int is_on, uint8_t valid, int64_t ts);
soft_sched_action soft_sched_due(soft_sched_t *st, int64_t now_s, int mining);

#endif /* SOFTONOFF_SCHED_H */
