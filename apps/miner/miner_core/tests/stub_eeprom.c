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
#include <string.h>
#include <stdio.h>

int eeprom_init(void)
{
    return 0;
}

int eeprom_read(uint8_t addr, uint8_t *pbuf, uint8_t data_len, uint8_t miner_id)
{
    (void)addr;
    (void)miner_id;
    if (pbuf && data_len) {
        memset(pbuf, 0, data_len);
        snprintf((char *)pbuf, data_len, "CACHEDSN0001");
    }
    return 0;
}
