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

#ifndef PSU_OPS_H
#define PSU_OPS_H

#include <stdint.h>

/* one PSU reading; a field that could not be read is 0 */
typedef struct hal_psu_reading {
    uint16_t vout_units;    /* PSU voltage: DC-DC PD input, mV */
    uint16_t iout_units;    /* PSU current: DC-DC PD input (INA226), mA */
    uint16_t pout_w;        /* PSU-reported power, W */
    uint16_t voutcmd_units; /* commanded output voltage, rail counts */
    uint16_t err;           /* PSU error code, 0 when the PSU has none */
    uint16_t vout12_units;  /* 12 V reading, 10 mV counts, 0 when the PSU has none */
} hal_psu_reading;

enum {
    HAL_PSU_RAIL_OK            = 0,  /* commanded and confirmed */
    HAL_PSU_RAIL_E_UNSUPPORTED = -1, /* the PSU has no such command */
    HAL_PSU_RAIL_E_NODEV       = -2, /* the PSU control bus would not open */
    HAL_PSU_RAIL_E_REFUSED     = -3, /* the request was refused */
    HAL_PSU_RAIL_E_UNCONFIRMED = -4, /* commanded, never confirmed */
};

/* PSU behavior flags, bits of hal_psu_ops_t.flags */
enum {
    /* Unconfirmed rail-off: ERR_MM_POWER, fan at max; a failed rail-on is followed by rail-off. */
    HAL_PSU_F_OFF_CONFIRM   = 1u << 0,
    HAL_PSU_F_LATCH_ON_FAIL = 1u << 1, /* failed rail_on latches a power fault */
};

/* PSU backend operations; a NULL member is not supported */
typedef struct hal_psu_ops {
    int (*probe)(void);
    int (*read)(hal_psu_reading *out); /* fills *out; non-zero, all fields 0, if unusable */
    int (*set_volt)(uint16_t units);   /* sets the rail voltage, rail counts */
    int (*pout_limit)(uint16_t *w);    /* negotiates the input power limit, W */
    int (*rail_on)(uint16_t units);    /* powers the hashboard on at a voltage, rail counts */
    int (*rail_off)(void);
    int (*vout_fault)(int vout, int pout_w, int vout12); /* vout <= 0: unread */
    unsigned flags;                                      /* HAL_PSU_F_* bits */
} hal_psu_ops_t;

const char          *hal_psu_rail_strerror(int rc);
const hal_psu_ops_t *hal_psu(void);

#endif /* PSU_OPS_H */
