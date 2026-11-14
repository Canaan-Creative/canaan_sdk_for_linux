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

#include "board_sensors.h"
#include <stdint.h>
#include <stdio.h>

/* Counts a failure in fails when dcdc_mv_to_reg(mv), mV, differs from register code exp */
#define EQ(mv, exp) \
    do { \
        uint8_t g = dcdc_mv_to_reg(mv); \
        if (g != (exp)) { \
            printf("FAIL %d mV -> 0x%02x (want 0x%02x)\n", (mv), g, (exp)); \
            fails++; \
        } \
    } while (0)

static int fails;

int i2c_init(uint8_t id)
{
    (void)id;
    return 0;
}
int i2c_write(uint8_t id, uint16_t a, uint8_t r, uint8_t *v, uint8_t n)
{
    (void)id;
    (void)a;
    (void)r;
    (void)v;
    (void)n;
    return 0;
}
int i2c_read(uint8_t id, uint16_t a, uint8_t r, uint8_t *v, uint8_t n)
{
    (void)id;
    (void)a;
    (void)r;
    (void)v;
    (void)n;
    return 0;
}
int adc_get_raw(int ch, int *raw)
{
    (void)ch;
    (void)raw;
    return -1;
}

int main(void)
{
    EQ(3600, 0x00);
    EQ(3450, 0x85);
    EQ(3574, 0x81);
    EQ(3626, 0x01);
    EQ(3200, 0x8F);
    EQ(4000, 0x0F);
    EQ(3000, 0x8F);
    EQ(4200, 0x0F);
    if (fails) {
        printf("test_board_volt: %d FAILED\n", fails);
        return 1;
    }
    printf("test_board_volt: OK\n");
    return 0;
}
