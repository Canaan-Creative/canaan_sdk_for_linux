// SPDX-License-Identifier: MIT
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

#include <stdint.h>

#include "eeprom_dev.h"

#include "i2c_dev.h"
#include "osif.h"

int eeprom_read(uint8_t addr, uint8_t *pbuf, uint8_t data_len, uint8_t miner_id)
{
    (void)miner_id;
    if (i2c_fd(I2C_POWER_E2P) < 0) {
        osif_log(OSIF_LOG_ERR, "EEPROM: bus not open");
        return -1;
    }
    if (i2c_read(I2C_POWER_E2P, EEPROM_ADD, addr, pbuf, data_len) != 0) {
        osif_log(OSIF_LOG_ERR, "EEPROM: read failed");
        return -1;
    }
    return 0;
}

int eeprom_init(void)
{
    if (i2c_init(I2C_POWER_E2P) < 0) {
        osif_log(OSIF_LOG_ERR, "EEPROM: bus could not be opened");
        return -1;
    }
    osif_log(OSIF_LOG_DBG, "EEPROM: ready");
    return 0;
}
