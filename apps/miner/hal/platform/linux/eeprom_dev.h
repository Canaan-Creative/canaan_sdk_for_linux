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

#ifndef EEPROM_DEV_H
#define EEPROM_DEV_H

#include <stdint.h>

#define EEPROM_ADD 0x50 /* EEPROM 7-bit I2C address */
#define SN_OFST    0    /* hash board serial number offset in the EEPROM, bytes */

int eeprom_init(void);
int eeprom_read(uint8_t addr, uint8_t *pbuf, uint8_t data_len, uint8_t miner_id);

#endif /* EEPROM_DEV_H */
