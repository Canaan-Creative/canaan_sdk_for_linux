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

#ifndef BOARD_SENSORS_H
#define BOARD_SENSORS_H

#include <stdint.h>

/* one PSU reading; every field is 0 when the read fails */
typedef struct {
    uint16_t vout_mv;    /* PSU voltage: DC-DC PD input, mV */
    uint16_t iout_ma;    /* PSU-reported current, PSU units (mA on DC-DC) */
    uint16_t pout_w;     /* PSU-reported power, W */
    uint16_t voutcmd_mv; /* commanded output voltage, rail counts */
    uint16_t err;        /* PSU error code, 0 when the PSU has none */
    uint16_t vout12;     /* 12 V reading, 10 mV counts, 0 when the PSU has none */
} board_power;

int     board_sensors_init(void);
int     board_power_read(board_power *out);
int     board_ntc_read(int *degc);
uint8_t dcdc_mv_to_reg(uint16_t mv);
int     board_power_set_volt(uint16_t counts);
int     board_power_rail_on(uint16_t counts);
int     board_power_rail_off(void);

#endif /* BOARD_SENSORS_H */
