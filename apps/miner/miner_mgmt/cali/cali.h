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

/*
 * Per-mode V/F calibration: the factory table, persistence in
 * hashrate_cali.ini, and persisted-over-default selection. No hardware
 * access: callers apply the values.
 */
#ifndef CALI_H
#define CALI_H

#include <stdint.h>
#include <stddef.h>

#include "boardconf.h"
#include "mc_paths.h"

#define CALI_MODE_MAX     3
#define CALI_MODE_DEFAULT WORK_MODE_DEFAULT_ID /* work mode used when none is saved */
/* Mode bits lo..hi of one BOARD_WORKMODE_RANGES span. */
#define CALI_MODE_SPAN_BITS(lo, hi) ((2u << (hi)) - (1u << (lo))) |
/* Bit mask of the work modes BOARD_WORKMODE_RANGES admits. */
#define CALI_MODE_BOARD_MASK (BOARD_WORKMODE_RANGES(CALI_MODE_SPAN_BITS) 0u)
#define CALI_LEVEL_COUNT     WORK_LEVEL_COUNT /* calibration levels per work mode */
#define CALI_FIELD_COUNT     5                /* fields in a calibration string */
#define CALI_STR_MAX         64               /* calibration string buffer, bytes incl. NUL */
#define CALI_WORKMODE_SECT   "syscfg"         /* systemcfg.ini section of the saved work mode */
#define CALI_WORKMODE_KEY    "work_mode"      /* systemcfg.ini key of the saved work mode */

/* Calibration of one work mode and level */
typedef struct {
    uint16_t max_pout;     /* maximum output power, W */
    uint16_t temp;         /* target temperature, degrees C */
    uint16_t volt;         /* core rail voltage, rail counts */
    uint16_t pll_start;    /* start frequency, MHz */
    uint16_t pll_interval; /* frequency step between PLL points, MHz */
} cali_param;

/* Settings store that holds the saved work mode */
typedef struct {
    int (*get)(const char *section, const char *key, char *buf, size_t cap); /* 0 when found */
    int (*set)(const char *section, const char *key, const char *value);     /* 0 when stored */
} cali_wm_store;

static inline uint8_t cali_mode_top(unsigned allowed)
{
    uint8_t mode = CALI_MODE_MAX - 1;
    while (mode && !(allowed & (1u << mode)))
        mode--;
    return mode;
}

const cali_param *cali_default_param(uint16_t asic_cnt, uint8_t mode);
int               cali_format(const cali_param *p, char *buf, size_t cap);
int               cali_parse(const char *str, cali_param *out);
int               cali_get(const char *path, uint8_t mode, int8_t level, char *out, size_t cap);
int               cali_set(const char *path, uint8_t mode, int8_t level, const char *val);
uint8_t           cali_workmode_get(const cali_wm_store *s);
int               cali_workmode_set(const cali_wm_store *s, uint8_t mode);
int cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level, cali_param *out,
        uint8_t *aging_finished);

#endif /* CALI_H */
