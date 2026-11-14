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

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include "i2c_dev.h"
#include "tst_i2c.h"

static int tst_i2c_ioctl(int fd, unsigned long req, unsigned long arg);
#define open  tst_i2c_open /* routes i2c_dev.c opens to the test node */
#define ioctl tst_i2c_ioctl
#include "../platform/linux/i2c_dev.c"
#undef ioctl
#undef open

static int            tst_rdwr_n;
static int            tst_rdwr_rc = -1; /* value the I2C_RDWR ioctl returns */
static uint32_t       tst_nmsgs;        /* message count of the last I2C_RDWR */
static struct i2c_msg tst_msgs[2];      /* first two messages of the last I2C_RDWR */
static uint8_t        tst_bufs[2][256]; /* write payloads of those messages, zero for reads */

static int tst_i2c_ioctl(int fd, unsigned long req, unsigned long arg)
{
    (void)fd;
    if (req != I2C_RDWR)
        return -1;
    const struct i2c_rdwr_ioctl_data *q = (const struct i2c_rdwr_ioctl_data *)arg;
    tst_rdwr_n++;
    tst_nmsgs = q->nmsgs;
    memset(tst_msgs, 0, sizeof tst_msgs);
    memset(tst_bufs, 0, sizeof tst_bufs);
    for (uint32_t i = 0; i < q->nmsgs && i < 2; i++) {
        tst_msgs[i] = q->msgs[i];
        if (!(q->msgs[i].flags & I2C_M_RD))
            memcpy(tst_bufs[i], q->msgs[i].buf, q->msgs[i].len);
    }
    return tst_rdwr_rc;
}

int main(void)
{
    const char *node = TST_I2C_NODE;
    unlink(node);
    FILE *tf = fopen(node, "w");
    assert(tf);
    fclose(tf);

    assert(i2c_init(I2C_PD) == 0);
    assert(i2c_fd(I2C_PD) >= 0);
    assert(i2c_init(I2C_PD) == 0);
    assert(i2c_init(I2C_POWER_E2P) == 0);

    assert(i2c_init(I2C_MAX) < 0);
    assert(i2c_fd(I2C_MAX) < 0);

    assert(i2c_write(I2C_PD, 0x50, 0x00, NULL, 2) < 0);
    assert(i2c_read(I2C_PD, 0x50, 0x00, NULL, 1) < 0);
    uint8_t b[2] = { 0x12, 0x34 };
    assert(i2c_read(I2C_PD, 0x50, 0x00, b, 0) < 0);
    assert(tst_rdwr_n == 0);

    assert(i2c_write(I2C_PD, 0x50, 0x01, b, 2) < 0);
    assert(tst_rdwr_n == 1);

    tst_rdwr_rc = 0;
    tst_rdwr_n  = 0;
    assert(i2c_write(I2C_PD, 0x40, 0x05, b, 2) == 0);
    assert(tst_rdwr_n == 1);
    assert(tst_nmsgs == 1);
    assert(tst_msgs[0].addr == 0x40);
    assert(tst_msgs[0].flags == 0);
    assert(tst_msgs[0].len == 3);
    assert(tst_bufs[0][0] == 0x05);
    assert(tst_bufs[0][1] == 0x12 && tst_bufs[0][2] == 0x34);

    tst_rdwr_n = 0;
    assert(i2c_write(I2C_PD, 0x40, 0x07, NULL, 0) == 0);
    assert(tst_rdwr_n == 1 && tst_nmsgs == 1);
    assert(tst_msgs[0].len == 1 && tst_bufs[0][0] == 0x07);

    uint8_t rd[3];
    tst_rdwr_n = 0;
    assert(i2c_read(I2C_PD, 0x50, 0x09, rd, 3) == 0);
    assert(tst_rdwr_n == 1);
    assert(tst_nmsgs == 2);
    assert(tst_msgs[0].addr == 0x50 && tst_msgs[0].flags == 0);
    assert(tst_msgs[0].len == 1 && tst_bufs[0][0] == 0x09);
    assert(tst_msgs[1].addr == 0x50 && tst_msgs[1].flags == I2C_M_RD);
    assert(tst_msgs[1].len == 3 && tst_msgs[1].buf == rd);

    tst_rdwr_rc = -1;
    assert(i2c_read(I2C_PD, 0x50, 0x09, rd, 3) < 0);

    assert(i2c_deinit(I2C_PD) == 0);
    assert(i2c_fd(I2C_PD) < 0);
    tst_rdwr_rc = 0;
    tst_rdwr_n  = 0;
    assert(i2c_write(I2C_PD, 0x40, 0x05, b, 2) < 0);
    assert(i2c_read(I2C_PD, 0x50, 0x09, rd, 3) < 0);
    assert(tst_rdwr_n == 0);
    assert(i2c_deinit(I2C_POWER_E2P) == 0);

    unlink(node);
    printf("i2c_dev OK\n");
    return 0;
}
