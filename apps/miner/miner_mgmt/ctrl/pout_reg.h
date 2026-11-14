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

#ifndef POUT_REG_H
#define POUT_REG_H

#include <stdint.h>

/* servo stage: lower the rail above the budget, raise it below the window up to
 * max(volt_cap, start_volt) */
#define POUT_STAGE_MAX_LIMIT_ONLY 1
#define POUT_STAGE_FIT_POUT       2 /* servo stage: keep output power inside the budget window */

typedef struct pout_reg_params {
    short    up_limit, down_limit; /* window above and below (negative) max_pout, W */
    uint16_t pout_min;             /* lowest power budget, W */
    short    vol_min, vol_max;     /* core-rail voltage range, rail counts */
    short    volt_step;            /* core-rail change per step, rail counts */
    short    abnormal_pout;        /* output power reading above this is invalid, W */
    uint16_t stab_thresh_normal;   /* MAX_LIMIT_ONLY steps before FIT_POUT, mode not aged */
    uint16_t stab_thresh_aged;     /* MAX_LIMIT_ONLY steps before FIT_POUT, mode aged */
} pout_reg_params_t;

typedef struct pout_reg_telem {
    uint8_t  tuning;
    int      aging_finished; /* work mode aged flag */
    uint16_t pout;           /* output power, W */
    uint16_t vout;           /* output voltage, rail counts */
    uint16_t poutlimit;      /* power budget, W */
    short    voutcmd;        /* commanded core-rail voltage, rail counts */
    short    volt_cap;       /* calibrated voltage of the work mode, raise cap, rail counts */
} pout_reg_telem_t;

typedef struct pout_reg_state {
    short    stage;      /* POUT_STAGE_* */
    uint16_t stability;  /* MAX_LIMIT_ONLY steps taken */
    uint16_t max_pout;   /* power budget, W */
    short    start_volt; /* core rail at the first servo step of the mode, rail counts */
} pout_reg_state_t;

int pout_reg_step(pout_reg_state_t *st, const pout_reg_params_t *pp, const pout_reg_telem_t *t,
        short *volt_out);

#endif /* POUT_REG_H */
