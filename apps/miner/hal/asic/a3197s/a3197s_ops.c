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

#include "hal.h"
#include "a3197s_ops.h"
#include "uart_pro.h"
#include "a3197s_pll_seq.h"
#include "a3197s_decode.h"
#include "gpio_dev.h"
#include "toast_work.h"
#include "toast_port.h"
#include "boardconf.h"
#include "board/board_tables.h"
#include "osif.h"
#include <unistd.h>
#include <string.h>

#define A3197S_EFUSE_WORDS 16 /* efuse data per chip, 32-bit words */
#define A3197S_PVT_SEL     2u

static void     pll_set_reg(uint8_t m, uint32_t addr, uint32_t val);
static uint32_t pll_get_reg(uint8_t m, uint32_t addr);
static void     pll_delay(uint32_t ticks);

static uint32_t g_pvmask[HBOARD_COUNT][8]; /* last written VERSION0..7 words per board */
static uint32_t g_pvt_mode;
/* PLL sequence config, frequencies in MHz */
static a3197s_pll_cfg_t s_pll_cfg = {
    .freq       = { INIT_FREQ, INIT_FREQ, INIT_FREQ, INIT_FREQ },
    .init_freq  = INIT_FREQ,
    .ssdn_pro   = 0,
    .spdlow     = 1,
    .spdhigh    = 4,
    .ss_enable  = 1,
    .pll_select = 2,
    .ss_sel     = 1,
    .ss_pri     = 0,
    .ss_pd      = 0,
};

static const a3197s_pll_io_t PLL_IO = {
    .chip_select     = uart_chip_select,
    .set_reg         = pll_set_reg,
    .get_reg         = pll_get_reg,
    .get_chip_select = uart_get_chip_select,
    .delay_ticks     = pll_delay,
};

uint32_t a3197s_enumerate(uint8_t channel)
{
    uint32_t idx = 0;

    uart_chip_select(channel, A3197S_BROADCAST);
    uint32_t cfg1 = A3197S_UART_CFG1_DEFAULT;
    uart_write_fifo(channel, A3197S_UART_CFG1_ADDR, &cfg1, 1, UART_WRITE_MODE);
    uart_set_chiplast(channel, 0);
    usleep(1000);
    uart_clear_rxfifo(channel);

    uart_write_fifo(channel, 0x0, &idx, 1, UART_INIT_MODE);
    usleep(100 * 1000);

    uint32_t back = 0;
    uart_read_fifo(channel, 0x0, &back, 1);
    return back;
}

int a3197s_read_reg(uint8_t channel, uint32_t addr, uint32_t *out, uint32_t n)
{
    uart_write_fifo(channel, addr, NULL, n, UART_READ_MODE);
    return uart_read_fifo(channel, addr, out, n);
}

static int a3197s_op_send_work(uint8_t channel, struct toast_work *w, uint32_t len)
{
    return uart_write_fifo(channel, A3197S_WORK_ADDR, (uint32_t *)w + TOAST_WORK_VERSION_WORDS,
            (len >> 2) - TOAST_WORK_VERSION_WORDS, UART_WRITE_MODE);
}

void a3197s_write_version(uint8_t channel, const uint32_t v[8])
{
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t word = v[i];
        uart_write_fifo(channel, i * 4u, &word, 1, UART_WRITE_MODE);
        if (channel < HBOARD_COUNT)
            g_pvmask[channel][i] = v[i];
    }
}

static int a3197s_op_set_target(uint8_t channel, uint32_t target_h, uint32_t target_l)
{
    int r = uart_write_fifo(channel, A3197S_TARGETL_ADDR, &target_l, 1, UART_WRITE_MODE);
    r |= uart_write_fifo(channel, A3197S_TARGETH_ADDR, &target_h, 1, UART_WRITE_MODE);
    return r ? HAL_ERR : HAL_OK;
}

void a3197s_enable_nonce_return(uint8_t channel)
{
    uint32_t v = 0x80000000u;
    uart_chip_select(channel, A3197S_BROADCAST);
    uart_write_fifo(channel, A3197S_NONCE_UPDATE_ADDR, &v, 1, UART_WRITE_MODE);
    uart_write_fifo(channel, A3197S_NONCE_UPDATE_ADDR, &v, 1, UART_WRITE_MODE);
}

static void pvt_write_mode(uint8_t channel, uint32_t mode)
{
    uint32_t v;
    v = A3197S_DEFAULT_TVSENW | (A3197S_PVT_SEL << 3) | (0u << 2) | (0u << 1) | g_pvt_mode;
    uart_write_fifo(channel, A3197S_TVSENW_ADDR, &v, 1, UART_WRITE_MODE);
    g_pvt_mode = mode;
    v          = A3197S_DEFAULT_TVSENW | (A3197S_PVT_SEL << 3) | (0u << 2) | (0u << 1) | g_pvt_mode;
    uart_write_fifo(channel, A3197S_TVSENW_ADDR, &v, 1, UART_WRITE_MODE);
    usleep(1);
    v = A3197S_DEFAULT_TVSENW | (A3197S_PVT_SEL << 3) | (0u << 2) | (1u << 1) | g_pvt_mode;
    uart_write_fifo(channel, A3197S_TVSENW_ADDR, &v, 1, UART_WRITE_MODE);
    v = A3197S_DEFAULT_TVSENW | (A3197S_PVT_SEL << 3) | (1u << 2) | (1u << 1) | g_pvt_mode;
    uart_write_fifo(channel, A3197S_TVSENW_ADDR, &v, 1, UART_WRITE_MODE);
}

static int pvt_fetch(uint8_t channel, uint32_t *out)
{
    int overtime = 0;
    *out         = 0;
    if (a3197s_read_reg(channel, A3197S_TVSENR_ADDR, out, 1))
        *out = 0;
    while (!(*out & A3197S_TVSENR_READY)) {
        if (overtime > 5) {
            *out = 0;
            return -1;
        }
        usleep(10);
        if (a3197s_read_reg(channel, A3197S_TVSENR_ADDR, out, 1))
            *out = 0;
        overtime++;
    }
    return 0;
}

int a3197s_pvt_select_mode(uint8_t channel, int temp_mode, uint16_t chip)
{
    uart_chip_select(channel, A3197S_BROADCAST);
    pvt_write_mode(channel, temp_mode ? 1u : 0u);
    uart_chip_select(channel, chip);
    return 0;
}

void a3197s_fetch_pvt_temp(uint8_t channel, double *degc)
{
    uint32_t r  = 0;
    int      rc = pvt_fetch(channel, &r);
    *degc       = decode_pvt_temp((uint16_t)(r & 0xfff));
    if (rc != 0)
        osif_log(OSIF_LOG_ERR, "board %u: chip temperature sensor read timed out", channel);
}

void a3197s_fetch_pvt_volt(uint8_t channel, uint16_t *mv)
{
    uint32_t r  = 0;
    int      rc = pvt_fetch(channel, &r);
    *mv         = decode_pvt_volt((uint16_t)((r >> 12) & 0xfff));
    if (rc != 0)
        osif_log(OSIF_LOG_ERR, "board %u: chip voltage sensor read timed out", channel);
}

static uint16_t ri_gpio(uint8_t channel)
{
    return board_hboards[channel % HBOARD_COUNT].ri_gpio;
}

void a3197s_enter_config_mode(uint8_t channel)
{
    uint16_t pin = ri_gpio(channel);

    if (pin)
        gpio_set_direction(pin, GPIO_OUT_HIGH);
}

void a3197s_enter_work_mode(uint8_t channel)
{
    uint16_t pin = ri_gpio(channel);

    if (pin)
        gpio_set_value(pin, GPIO_LOW);
}

static void a3197s_op_read_efuse(uint8_t channel, uint32_t *data)
{
    uint32_t v;

    v = 0x08u;
    uart_write_fifo(channel, A3197S_EFUSE_CTRL_ADDR, &v, 1, UART_WRITE_MODE);
    usleep(1000);

    v = 0x01u;
    uart_write_fifo(channel, A3197S_EFUSE_CTRL_ADDR, &v, 1, UART_WRITE_MODE);

    uint32_t ctrl  = 0;
    int      ready = 0;
    for (int tries = 0; tries < 5; tries++) {
        usleep(1000);
        if (a3197s_read_reg(channel, A3197S_EFUSE_CTRL_ADDR, &ctrl, 1) != 0)
            continue;
        if (ctrl & (1u << 5)) {
            ready = 1;
            break;
        }
    }

    if (!ready) {
        osif_log(OSIF_LOG_WARN,
                "board %u: chip efuse not ready after 5 ms "
                "(control 0x%08x); bin grade unavailable",
                channel, ctrl);
        memset(data, 0, A3197S_EFUSE_WORDS * sizeof(uint32_t));
        return;
    }

    uart_write_fifo(channel, A3197S_EFUSE_FIFO_ADDR, NULL, A3197S_EFUSE_WORDS, UART_READ_MODE);
    if (uart_read_fifo(channel, A3197S_EFUSE_FIFO_ADDR, data, A3197S_EFUSE_WORDS) != 0) {
        osif_log(OSIF_LOG_WARN,
                "board %u: chip efuse read failed; bin grade "
                "unavailable",
                channel);
        memset(data, 0, A3197S_EFUSE_WORDS * sizeof(uint32_t));
    }
}

static int a3197s_op_get_nonce(uint8_t channel, struct toast_nonce *n, uint8_t *count)
{
    int ret = uart_get_nonce(channel, n, count, g_pvmask[channel < HBOARD_COUNT ? channel : 0]);
    return ret ? HAL_ERR : HAL_OK;
}

static void pll_set_reg(uint8_t m, uint32_t addr, uint32_t val)
{
    uart_write_fifo(m, addr, &val, 1, UART_WRITE_MODE);
}

static uint32_t pll_get_reg(uint8_t m, uint32_t addr)
{
    uint32_t v = 0;

    a3197s_read_reg(m, addr, &v, 1);
    return v;
}

static void pll_delay(uint32_t ticks)
{
    usleep(ticks * 1000);
}

void a3197s_set_freq(const uint16_t f[A3197S_PLL_POINTS])
{
    for (int i = 0; i < A3197S_PLL_POINTS; i++)
        s_pll_cfg.freq[i] = correct_freq(f[i]);
}

static void a3197s_op_set_pll(uint8_t miner_id, uint16_t asic_id, uint8_t pll0_update)
{
    osif_log(OSIF_LOG_INFO,
            "board %u asic %u: set PLL freq "
            "%u/%u/%u/%u",
            miner_id, asic_id, s_pll_cfg.freq[0], s_pll_cfg.freq[1], s_pll_cfg.freq[2],
            s_pll_cfg.freq[3]);
    a3197s_set_pll_seq(&PLL_IO, miner_id, asic_id, pll0_update, &s_pll_cfg);
}

static const hal_asic_ops_t A3197S_OPS = {
    .send_work  = a3197s_op_send_work,
    .get_nonce  = a3197s_op_get_nonce,
    .set_target = a3197s_op_set_target,
    .set_pll    = a3197s_op_set_pll,
    .read_efuse = a3197s_op_read_efuse,
};

const hal_asic_ops_t *hal_asic(void)
{
    return &A3197S_OPS;
}
