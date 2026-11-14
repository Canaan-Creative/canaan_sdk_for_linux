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

#include "a3197s_ops.h"
#include "uart_pro.h"

static void w(uint8_t ch, uint32_t addr, uint32_t val)
{
    uart_write_fifo(ch, addr, &val, 1, UART_WRITE_MODE);
}

static void core_w(uint8_t ch, uint32_t sel, uint32_t val)
{
    w(ch, A3197S_CORE_CFG_ADDR, sel);
    w(ch, A3197S_CORE_WR_DATA_ADDR, val);
}

void a3197s_config_asic(uint8_t channel)
{
    uart_chip_select(channel, A3197S_BROADCAST);
    uart_clear_rxfifo(channel);

    uint32_t ecc[7] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
        0xffffffff };
    uart_write_fifo(channel, A3197S_UART_TIMEOUT_ADDR, ecc, 7, UART_WRITE_MODE);

    uart_chip_select(channel, A3197S_BROADCAST);
    w(channel, A3197S_FB_SEL_ADDR, 0);
    w(channel, A3197S_IO_N_PAD_CFG_ADDR, A3197S_DEFAULT_IO_N_PAD_CFG);
    w(channel, A3197S_IO_S_PAD_CFG_ADDR, A3197S_DEFAULT_IO_S_PAD_CFG);
    w(channel, A3197S_CFG_ADDR, A3197S_DEFAULT_CFG);
    w(channel, A3197S_TBASE_ADDR, A3197S_DEFAULT_TBASE);
    w(channel, A3197S_ROLLTIME_ADDR, A3197S_DEFAULT_ROLLTIME);
    w(channel, A3197S_CG_EN_ADDR, A3197S_DEFAULT_CG_EN);
    w(channel, A3197S_SPDSYC_CFG_ADDR, A3197S_DEFAULT_SPDSYC_CFG);
    w(channel, A3197S_FT_ADDR, A3197S_DEFAULT_FT);
    w(channel, A3197S_FTDBG_ADDR, A3197S_DEFAULT_FTDBG);
    w(channel, A3197S_FT_RAZOR_ADDR, A3197S_DEFAULT_FT_RAZOR);
    w(channel, A3197S_SS_CTRL_ADDR, A3197S_DEFAULT_SS_CTRL);
    w(channel, A3197S_PLLTABLE_ADDR, A3197S_DEFAULT_PLLTABLE);
    w(channel, A3197S_SPDLOG_TIMER_ADDR, 0x10000 + 3000);
    w(channel, A3197S_DH_CNT_WDT_CTRL_ADDR, A3197S_DEFAULT_DH_CNT_WDT_CTRL);
    w(channel, A3197S_GBP_CTRL_ADDR, A3197S_DEFAULT_GBP_CTRL);
    w(channel, A3197S_LFB_CTRL_ADDR, A3197S_DEFAULT_LFB_CTRL);
    w(channel, A3197S_SELFCAL_CTRL_ADDR, A3197S_DEFAULT_SELFCAL_CTRL);
    w(channel, A3197S_SELFCAL_WDT_CTRL_ADDR, A3197S_DEFAULT_SELFCAL_WDT_CTRL);
    w(channel, A3197S_THSR_WDT_TIMER_CTRL_ADDR, A3197S_DEFAULT_THSR_WDT_TIMER_CTRL);
    w(channel, A3197S_SELFVEC_AGING_CTRL_ADDR, A3197S_DEFAULT_SELFVEC_AGING_CTRL);
    w(channel, A3197S_SELFVEC_AGING_WDT_CTRL_ADDR, A3197S_DEFAULT_SELFVEC_AGING_WDT_CTRL);
    w(channel, A3197S_SELFVEC_AGING_CFG_ADDR, A3197S_DEFAULT_SELFVEC_AGING_CFG);
    w(channel, A3197S_SELFVEC_AGING_TIMER_ADDR, A3197S_DEFAULT_SELFVEC_AGING_TIMER);
    w(channel, A3197S_TOPRX_EN_0_ADDR, A3197S_DEFAULT_TOPRX_EN_0);
    w(channel, A3197S_TOPRX_EN_1_ADDR, A3197S_DEFAULT_TOPRX_EN_1);
    w(channel, A3197S_TOPRX_EN_2_ADDR, A3197S_DEFAULT_TOPRX_EN_2);
    w(channel, A3197S_TOPRX_EN_3_ADDR, A3197S_DEFAULT_TOPRX_EN_3);
    w(channel, A3197S_TOPRX_EN_4_ADDR, A3197S_DEFAULT_TOPRX_EN_4);
    w(channel, A3197S_PLL_COM_CFG_ADDR, A3197S_DEFAULT_PLL_COM_CFG);
    w(channel, A3197S_PLL_CTRL01_ADDR, A3197S_DEFAULT_PLL_CTRL01);
    w(channel, A3197S_PLL_CTRL23_ADDR, A3197S_DEFAULT_PLL_CTRL23);
    w(channel, A3197S_PLL1L1_ADDR, A3197S_DEFAULT_PLL1L1);
    w(channel, A3197S_PLL2L1_ADDR, A3197S_DEFAULT_PLL2L1);
    w(channel, A3197S_PLL3L1_ADDR, A3197S_DEFAULT_PLL3L1);
    w(channel, A3197S_MUX_ADDR, A3197S_DEFAULT_MUX);
    w(channel, A3197S_DIVCNT_CTRL_ADDR, A3197S_DEFAULT_DIVCNT_CTRL);
    w(channel, A3197S_SPDTH0_ADDR, (170u << 16) | 10000u);
    w(channel, A3197S_SPDTH1_ADDR, 32767u);
    w(channel, A3197S_SPDTH2_ADDR, 1300000u);
    w(channel, A3197S_SPDTH3_ADDR, 8u);
    w(channel, A3197S_SPDTH4_ADDR, 0u);
    w(channel, A3197S_RI_WEN_ADDR, 1);
    w(channel, A3197S_REGRI_ADDR, 0);
    w(channel, A3197S_RI_WEN_ADDR, 0);
    w(channel, A3197S_CHKDROP_ADDR, 1);
    w(channel, A3197S_NONCE_CNT_ADDR, 1);

    uart_chip_select(channel, A3197S_BROADCAST);
    w(channel, A3197S_TVSENW_ADDR, A3197S_DEFAULT_TVSENW);
    w(channel, A3197S_TVSENW_ADDR, A3197S_DEFAULT_TVSENW | (1 << 1));
    w(channel, A3197S_TVSENW_ADDR, A3197S_DEFAULT_TVSENW | (2 << 3) | (1 << 1));
    w(channel, A3197S_TVSENW_ADDR, A3197S_DEFAULT_TVSENW | (2 << 3) | (1 << 2) | (1 << 1));

    w(channel, A3197S_SPDLOG_RESET_ADDR, 1);
}

void a3197s_mx2_reset(uint8_t channel)
{
    w(channel, A3197S_MX2_RST_ADDR, 1);
}

void a3197s_config_coreregs(uint8_t channel)
{
    core_w(channel, 0x000075ff, 0x11f);
    core_w(channel, 0x000081ff, 0x01);
    core_w(channel, 0x000090ff, 0xff001001);
    core_w(channel, 0x000075ff, 0x1f);
}
