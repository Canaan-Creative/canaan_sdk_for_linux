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

#ifndef AGING_H
#define AGING_H

#include <stdint.h>

#define AGING_PLL_COUNT 4  /* PLL points per chip */
#define RAIL_VOLT_STEP  26 /* core-rail voltage step, rail counts */
#define AGING_MIN_PE    8  /* lowest power per hashrate a best point may have, W per TH/s */

typedef struct aging_params {
    short    up_stages;
    short    down_stages;
    int      freq_step;     /* swept frequency change per stage, MHz */
    short    pout_up_limit; /* output power a best point may exceed max_pout by, W */
    uint8_t  pll_offset;    /* index of the swept PLL point */
    uint16_t pll_interval;  /* frequency step between PLL points, MHz */
} aging_params_t;

typedef struct aging_rec {
    int      hashrate_Mhz;          /* hashrate, MH/s */
    short    pout;                  /* output power, W */
    short    volt;                  /* core-rail voltage, rail counts */
    short    temp;                  /* average chip temperature, degC */
    unsigned pe;                    /* power per hashrate, mJ/TH; 999999 none */
    uint16_t freq[AGING_PLL_COUNT]; /* PLL frequencies, MHz */
} aging_rec_t;

typedef struct aging_telem {
    int      hashrate;  /* hashrate, MH/s */
    uint16_t pout;      /* output power, W */
    short    voutcmd;   /* commanded core-rail voltage, rail counts */
    short    temp;      /* average chip temperature, degC */
    int      power_ok;  /* power reading and rail control available flag */
    uint16_t max_pout;  /* output power budget, W */
    uint16_t pll_start; /* calibration start frequency, MHz */
} aging_telem_t;

typedef struct aging_state {
    short       enum_count;                   /* sweep stages done */
    short       move_direction;               /* direction of the last stage: 1 up, -1 down */
    uint16_t    current_pll[AGING_PLL_COUNT]; /* PLL frequencies under test, MHz */
    aging_rec_t best_rate;                    /* highest-hashrate sample */
    aging_rec_t best_pe;                      /* lowest power-per-hashrate sample */
    aging_rec_t last;                         /* sample measured on the last tick */
    short       measured;                     /* stage of last, 0 none on the last tick */
    uint8_t     progress;                     /* sweep progress, percent */
    short       aging_direction;              /* sweep phase: 1 up, -1 down, 0 finished */
} aging_state_t;

typedef enum {
    AGING_ACT_NONE = 0,
    AGING_ACT_MOVE_WINDOW,   /* send current_pll */
    AGING_ACT_SET_PLL_START, /* up-sweep done: send the start frequency */
    AGING_ACT_SET_PLL_BEST,  /* down-sweep done: apply the best-hashrate point */
    AGING_ACT_DONE,
} aging_action_t;

unsigned       aging_pe(uint32_t pout, uint32_t hashrate);
void           aging_pll_fill(uint16_t *pll, const aging_params_t *pp, uint16_t freq);
void           aging_reset(aging_state_t *st, uint16_t init_freq);
aging_action_t aging_rule_tick(aging_state_t *st, const aging_params_t *pp, const aging_telem_t *t,
        uint16_t *freq_out, short *volt_out);

#endif /* AGING_H */
