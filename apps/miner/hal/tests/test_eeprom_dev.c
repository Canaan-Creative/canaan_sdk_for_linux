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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "eeprom_dev.h"
#include "miner_ipc.h"
#include "osif.h"
#include "../../tests/include/tst_check.h"

static int fd_open; /* 1 while the I2C bus is open */
/* slave address, register offset and length of the last read */
static uint8_t rd_slave, rd_reg, rd_len;
static int     n_rd;

static void reset(void)
{
    fd_open = 1;
    n_rd    = 0;
}

int i2c_init(uint8_t id)
{
    (void)id;
    fd_open = 1;
    return 0;
}
int i2c_deinit(uint8_t id)
{
    (void)id;
    fd_open = 0;
    return 0;
}
int i2c_fd(uint8_t id)
{
    (void)id;
    return fd_open ? 3 : -1;
}

int i2c_read(uint8_t id, uint16_t slave, uint8_t reg, uint8_t *val, uint8_t len)
{
    (void)id;
    rd_slave = slave;
    rd_reg   = reg;
    rd_len   = len;
    n_rd++;
    for (uint8_t i = 0; i < len; i++)
        val[i] = 0xFF;
    return 0;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}

int main(void)
{
    uint8_t sn[HASH_SN_LEN];

    reset();
    fd_open = 0;
    CHECK(eeprom_read(SN_OFST, sn, HASH_SN_LEN, 0) == -1, "read w/o bus -> -1");
    CHECK(n_rd == 0, "no i2c_read when bus closed");

    reset();
    uint8_t rb[HASH_SN_LEN];
    CHECK(eeprom_read(SN_OFST, rb, HASH_SN_LEN, 0) == 0, "read 40B ok");
    CHECK(n_rd == 1 && rd_slave == 0x50 && rd_reg == SN_OFST && rd_len == HASH_SN_LEN,
            "one read of 40B @0 to 0x50");
    {
        int blank = 1;
        for (int i = 0; i < HASH_SN_LEN; i++)
            if (rb[i] != 0xFF)
                blank = 0;
        CHECK(blank, "blank EEPROM reads back 0xFF (faithful un-provisioned)");
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
