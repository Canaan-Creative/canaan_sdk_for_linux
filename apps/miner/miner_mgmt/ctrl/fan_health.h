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

#ifndef FAN_HEALTH_H
#define FAN_HEALTH_H

#define FAN_RPM_NONE        (-1) /* unreadable fan speed */
#define FAN_HEALTH_MAX_FANS 32
#define FAN_STALL_RPM       200 /* below this a driven fan is stalled, RPM */

typedef struct fan_health_params {
    int err_count; /* consecutive stalled samples that trip, 0 = no check */
} fan_health_params_t;

typedef struct fan_health_state {
    int bad_run; /* consecutive stalled samples, capped at err_count */
} fan_health_state_t;

typedef enum {
    FAN_HEALTH_OK = 0, /* no driven fan stalled, or the check is off */
    FAN_HEALTH_BAD,    /* a driven fan stalled, no trip */
    FAN_HEALTH_TRIP,   /* stalled for err_count samples in a row while working */
} fan_health_verdict;

fan_health_verdict fan_health_check(const fan_health_params_t *pp, fan_health_state_t *st,
        const int *rpm, int n, int duty_pct, int in_work);
int                fan_health_stopped_mask(const int *rpm, int n);

#endif /* FAN_HEALTH_H */
