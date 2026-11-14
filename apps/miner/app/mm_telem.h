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

#ifndef MM_TELEM_H
#define MM_TELEM_H

#include <stddef.h>
#include <stdint.h>

#include "boardconf.h"

#define MM_PLL_POINTS          4    /* PLL points per chip, count */
#define MM_TELEM_POUT_NONE     (-1) /* output power not read */
#define MM_TELEM_VOUT_NONE     (-1) /* output voltage not read */
#define MM_TELEM_WORKMODE_NONE (-1) /* work mode unknown */

typedef struct {
    int      poll_fresh;
    int      tmax;             /* hottest chip, degC */
    int      tavg;             /* average chip temperature, degC */
    int      temp_valid_chips; /* chips reporting a temperature, count */
    int      in_work;          /* any board mining flag */
    int      asic_live;        /* asic_miner healthy and recent flag */
    int      core_live;        /* core_miner healthy and recent flag */
    int      pout_w;           /* PSU output power, W */
    int      ps_vout_mv;       /* PSU voltage: DC-DC PD input, mV */
    int      ps_vout12;        /* PSU 12 V output reading */
    int      workmode;
    double   ghs_spd;   /* current hashrate, GH/s */
    uint32_t job_gap_s; /* time since the last pool job, s */

    int hb_count;                /* hashboards with an MCU, count */
    int hb_outlet[HBOARD_COUNT]; /* hashboard outlet temperature, degC */
    int hb_inlet[HBOARD_COUNT];  /* hashboard inlet temperature, degC */

    int asic_chips;
    int asic_boards; /* hashboards reported, count */
    int asic_chips_board[HBOARD_COUNT];
    int pll_cores[MM_PLL_POINTS]; /* cores per PLL point, count */
    int pll_mhz[MM_PLL_POINTS];   /* frequency per PLL point, MHz */
} mm_telem_t;

enum {
    MM_HBMCU_OK    = 0,
    MM_HBMCU_DEAD  = 1, /* hashboard MCU unreadable */
    MM_HBMCU_INLET = 2, /* inlet unreadable, outlet readable */
};

void mm_telem_ingest(const void *data, int len, unsigned conn);
void mm_telem_ingest_core(const void *data, int len);
void mm_telem_poll(void);
void mm_telem_get(mm_telem_t *out);
void mm_telem_asic_reset(unsigned conn_min);

static inline int mm_telem_real_freq(const mm_telem_t *t)
{
    long long core_mhz = 0;
    long      cores    = 0;
    for (int k = 0; k < MM_PLL_POINTS; k++) {
        if (t->pll_mhz[k] <= 0)
            continue;
        core_mhz += (long long)t->pll_cores[k] * t->pll_mhz[k];
        cores += t->pll_cores[k];
    }
    return cores ? (int)((core_mhz + cores / 2) / cores) : 0;
}

static inline uint32_t mm_hbmcu_fault_code(int rc, int outlet_valid)
{
    if (rc == 0)
        return MM_HBMCU_OK;
    return outlet_valid ? MM_HBMCU_INLET : MM_HBMCU_DEAD;
}

#endif /* MM_TELEM_H */
