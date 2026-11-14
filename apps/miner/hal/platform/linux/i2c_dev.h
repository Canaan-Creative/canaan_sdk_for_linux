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

#ifndef I2C_DEV_H
#define I2C_DEV_H

#include <stdint.h>

typedef enum {
    I2C_PD = 0,    /* power-supply control bus */
    I2C_POWER_E2P, /* hashboard EEPROM bus; also the DC-DC bus on DC-DC boards */
    I2C_MCU,
    I2C_MAX,
} i2c_id;

int i2c_init(uint8_t i2c_id);
int i2c_deinit(uint8_t i2c_id);
int i2c_fd(uint8_t i2c_id);
int i2c_write(uint8_t i2c_id, uint16_t slave_addr, uint8_t reg, uint8_t *val, uint8_t len);
int i2c_read(uint8_t i2c_id, uint16_t slave_addr, uint8_t reg, uint8_t *val, uint8_t len);

#endif /* I2C_DEV_H */
