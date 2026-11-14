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

#ifndef BOARD_TABLES_H
#define BOARD_TABLES_H

#include <stddef.h>
#include <stdint.h>

/* board_fan_t initializer; a PWM pad above 5 fails to compile */
#define BOARD_FAN_ROW(pad, node, ppr) \
    { (pad) + 0 * (int)sizeof(char[1 - 2 * ((pad) > 5)]), (node), (ppr) }

typedef struct {
    uint8_t     pwm;
    const char *tach_node; /* tachometer timer device path, or NULL */
    uint8_t     pulses_per_rev;
} board_fan_t;

typedef struct {
    const char *uart_dev; /* ASIC UART device path, or NULL */
    uint16_t    ri_gpio;  /* chain RI GPIO, or 0 */
} board_hboard_t;

typedef struct {
    uint16_t rst_gpio;
    uint16_t bsl_gpio; /* MCU BSL entry GPIO */
} board_hb_mcu_t;

extern const board_fan_t    board_fans[];
extern const size_t         board_fan_count;
extern const board_hboard_t board_hboards[];
extern const board_hb_mcu_t board_hb_mcus[];
extern const size_t         board_hb_mcu_count;

#endif /* BOARD_TABLES_H */
