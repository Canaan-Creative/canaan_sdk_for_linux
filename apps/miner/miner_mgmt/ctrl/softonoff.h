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

#ifndef SOFTONOFF_H
#define SOFTONOFF_H

#include <stdint.h>

/* Caller-owned; zero-init means mining with nothing held off. */
typedef struct softonoff_state {
    _Atomic uint64_t thermal_until; /* non-zero: thermal hold owns the rail */
    _Atomic int      api_softoff;   /* API soft-off latch */
    _Atomic int      start_wait;    /* 1: first asic_miner spawn held at cold start */
} softonoff_state_t;

typedef enum {
    SOFTONOFF_NONE = 0,
    /* softonoff_thermal_step */
    SOFTONOFF_THERMAL_OFF,        /* stop ASIC, rail off, overheat duty */
    SOFTONOFF_THERMAL_RESUME,     /* hold over: rail up, respawn ASIC, fan to PID */
    SOFTONOFF_THERMAL_TO_SOFTOFF, /* hold over, API soft-off on: stay down, API soft-off duty */
    /* softonoff_api_step */
    SOFTONOFF_API_OFF, /* stop ASIC, rail off, fan to the API soft-off duty */
    SOFTONOFF_API_ON,  /* rail up, respawn ASIC, fan to PID */
    SOFTONOFF_API_OFF_HELD,
    SOFTONOFF_API_IGNORED,    /* soft-on dropped during a thermal hold */
    SOFTONOFF_API_FAULT_HELD, /* softonoff_api_check: soft-on alone leaves a fault stop */
} softonoff_action;

/* child process stopped by a soft-off, bit set */
enum {
    SOFTONOFF_CHILD_ASIC = 1u << 0,
};

softonoff_action softonoff_thermal_step(softonoff_state_t *st, int overheat_req, uint64_t now_ms,
        uint32_t hold_ms);
softonoff_action softonoff_api_check(const softonoff_state_t *st, int fault_off, int want_on);
softonoff_action softonoff_api_step(softonoff_state_t *st, int want_on);
int              softonoff_intentional_exit(const softonoff_state_t *st, int is_asic);
int              softonoff_fan_hold(const softonoff_state_t *st, int fault_off);
unsigned         softonoff_children_stopped(const softonoff_state_t *st, int fault_off);

#endif /* SOFTONOFF_H */
