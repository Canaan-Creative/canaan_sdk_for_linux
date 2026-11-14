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

#ifndef THERMAL_H
#define THERMAL_H

#include <stdint.h>

#include "fault_report.h"
#include "miner_ipc.h"

/* over-temperature soft-off trip points */
typedef struct thermal_params {
    int over_temp;        /* hottest chip limit, degC */
    int outlet_over_temp; /* hottest hashboard outlet limit, degC */
    /* consecutive samples above outlet_over_temp that trip, 0 = no outlet check */
    int outlet_over_count;
} thermal_params_t;

typedef struct thermal_softoff_state {
    /* consecutive samples above outlet_over_temp, capped at outlet_over_count */
    int outlet_hot_run;
} thermal_softoff_state_t;

typedef enum {
    THERMAL_TELEM_OK = 0,   /* a usable chip temperature arrived */
    THERMAL_TELEM_HOLD,     /* none usable, still within the grace time */
    THERMAL_TELEM_FAILSAFE, /* none usable for longer than the grace time */
} thermal_telem_verdict;

typedef struct thermal_telem_state {
    uint64_t ok_ms;    /* last usable temperature time or first check time, ms; 0 none */
    int      failsafe; /* grace time exceeded flag */
} thermal_telem_state_t;

typedef enum {
    THERMAL_COVERAGE_QUIET = 0, /* nothing to report */
    THERMAL_COVERAGE_SHORT,     /* some chips stopped reporting a temperature */
    THERMAL_COVERAGE_FULL,      /* all chips report a temperature again */
} thermal_coverage_verdict;

typedef struct thermal_coverage_state {
    uint64_t       ripe_ms; /* time the chips started reporting, ms; 0 not reporting */
    fault_report_t rep;
} thermal_coverage_state_t;

int thermal_softoff_check(const thermal_params_t *pp, thermal_softoff_state_t *st, int temp_summax,
        const int *outlet_temp, int n_outlet, int in_work);
thermal_telem_verdict thermal_telem_check(thermal_telem_state_t *st, int asic_live, int temp_valid,
        uint64_t now_ms, uint32_t grace_ms);
thermal_coverage_verdict thermal_coverage_check(thermal_coverage_state_t *st, int valid_chips,
        int chips, int tavg, uint64_t now_ms, uint32_t ripen_ms);

#endif /* THERMAL_H */
