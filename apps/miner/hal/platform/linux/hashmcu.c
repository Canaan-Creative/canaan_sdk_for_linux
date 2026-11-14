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

#include "hashmcu.h"

#include "board_tables.h"
#include "boardconf.h"
#include "gpio_dev.h"
#include "hbmcu_ops.h"
#include "i2c_dev.h"
#include "miner_ipc.h"
#include "osif.h"

#define TCA9546A_ADDR     0x70 /* TCA9546A i2c mux address */
#define HASHMCU_ADDR      0x10 /* hashboard monitor MCU i2c address */
#define REG_TMP_BOARD_OUT 0x15
#define REG_TMP_BOARD_IN  0x16
#define READ_LEN          3 /* register read length, bytes: checksum, low, high */
#define RCV_VALUE_INVALID 0xFFFF
#define T_COEF            0.0625 /* temperature per count, degC */
#define MCU_PACE_MS       10     /* pause between two register reads, ms */

static int g_boards; /* hashboards the MCU bus serves, 0 when the bus is unavailable */

static int hbmcu_init(void)
{
    if (i2c_init(I2C_MCU) != 0) {
        g_boards = 0;
        osif_log(OSIF_LOG_ERR,
                "Hashboard controller bus unavailable; inlet/outlet "
                "temperatures will read as missing (%d)",
                ASIC_TEMP_NONE);
        return 0;
    }
    g_boards = HBOARD_COUNT;
    return g_boards;
}

static int hashmcu_select(int board)
{
    uint8_t nothing = 0;
    return i2c_write(I2C_MCU, TCA9546A_ADDR, (uint8_t)(1 << board), &nothing, 0);
}

static int reg_value_get(uint8_t reg, uint16_t *out)
{
    uint8_t v[READ_LEN];
    if (i2c_read(I2C_MCU, HASHMCU_ADDR, reg, v, READ_LEN) != 0)
        return -1;
    if ((uint8_t)(v[1] ^ v[2]) != v[0])
        return -1;
    uint16_t val = (uint16_t)((uint16_t)v[2] << 8 | v[1]);
    if (val == RCV_VALUE_INVALID)
        return -1;
    *out = val;
    return 0;
}

static int temp_12bit_complement(uint16_t temp_val)
{
    int16_t temp = (int16_t)(temp_val & 0x0fff);
    if (temp & 0x0800) {
        temp = (int16_t)(temp & 0x07ff);
        temp = (int16_t)~temp;
        temp = (int16_t)(temp + 1);
        temp = (int16_t)(temp & 0x07ff);
        temp = (int16_t)-temp;
    }
    return temp;
}

static int temp_read(uint8_t reg, int *degc)
{
    uint16_t v = 0;
    if (reg_value_get(reg, &v) != 0)
        return -1;
    *degc = (int)(temp_12bit_complement(v) * T_COEF);
    return 0;
}

static int hbmcu_read(int board, int *outlet_degc, int *inlet_degc)
{
    *outlet_degc = *inlet_degc = ASIC_TEMP_NONE;
    if (board < 0 || board >= g_boards)
        return -1;

    if (hashmcu_select(board) != 0)
        return -1;
    if (temp_read(REG_TMP_BOARD_OUT, outlet_degc) != 0)
        return -1;
    osif_sleep_ms(MCU_PACE_MS);
    if (temp_read(REG_TMP_BOARD_IN, inlet_degc) != 0)
        return -1;
    return 0;
}

int hashmcu_init(void)
{
    const hal_hbmcu_ops_t *ops = hal_hbmcu();
    return ops ? ops->init() : 0;
}

int hashmcu_count(void)
{
    return g_boards;
}

int hashmcu_read(int board, hashmcu_temps *out)
{
    const hal_hbmcu_ops_t *ops = hal_hbmcu();
    out->outlet_degc = out->inlet_degc = ASIC_TEMP_NONE;
    return ops ? ops->read(board, &out->outlet_degc, &out->inlet_degc) : -1;
}

static void mcu_start(int rst, int bsl)
{
    gpio_set_direction(bsl, GPIO_OUT_LOW);
    osif_sleep_ms(100);
    gpio_set_direction(rst, GPIO_OUT_LOW);
    osif_sleep_ms(1);
    gpio_set_direction(rst, GPIO_OUT_HIGH);
    osif_sleep_ms(10);
}

int mm_hashboard_bringup(void)
{
    if (!CAP_HB_MCU)
        return 0;

    if (I2C_MUX_RESET_GPIO >= 0)
        gpio_set_direction(I2C_MUX_RESET_GPIO, GPIO_OUT_HIGH);

    int n = 0;
    for (size_t i = 0; i < board_hb_mcu_count && i < HBOARD_COUNT; i++) {
        mcu_start(board_hb_mcus[i].rst_gpio, board_hb_mcus[i].bsl_gpio);
        n++;
    }

    osif_log(OSIF_LOG_INFO, "Hashboards: controller started on %d board(s)", n);
    return n;
}

static const hal_hbmcu_ops_t g_hbmcu_ops = {
    .init = hbmcu_init,
    .read = hbmcu_read,
};

const hal_hbmcu_ops_t *hal_hbmcu(void)
{
    return CAP_HB_MCU ? &g_hbmcu_ops : NULL;
}
