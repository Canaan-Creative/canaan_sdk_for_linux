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

#ifndef A3197S_REG_H
#define A3197S_REG_H

#define A3197S_BROADCAST      0x3ff /* chip address that selects all chips */
#define A3197S_CHIP_BROADCAST A3197S_BROADCAST

#define A3197S_WORK_ADDR         0x20
#define A3197S_CORE_CFG_ADDR     0x100
#define A3197S_CORE_WR_DATA_ADDR 0x104
#define A3197S_NONCE_UPDATE_ADDR 0x200 /* nonce return control register */
#define A3197S_CFG_ADDR          0x204
#define A3197S_TARGETL_ADDR      0x208 /* share target low word register */
#define A3197S_TARGETH_ADDR      0x20c /* share target high word register */
#define A3197S_TBASE_ADDR        0x210
#define A3197S_ROLLTIME_ADDR     0x214
#define A3197S_CG_EN_ADDR        0x218
#define A3197S_SPDSYC_CFG_ADDR   0x21c
#define A3197S_FT_ADDR           0x220
#define A3197S_FTDBG_ADDR        0x22c
#define A3197S_FT_RAZOR_ADDR     0x234
#define A3197S_CHKDROP_ADDR      0x238
#define A3197S_NONCE_CNT_ADDR    0x23c

#define A3197S_SS_CTRL_ADDR                0x400
#define A3197S_SPDTH0_ADDR                 0x404 /* speed threshold 0 register */
#define A3197S_SPDTH1_ADDR                 0x408 /* speed threshold 1 register */
#define A3197S_SPDTH2_ADDR                 0x40c /* speed threshold 2 register */
#define A3197S_SPDTH3_ADDR                 0x410 /* speed threshold 3 register */
#define A3197S_SPDTH4_ADDR                 0x414 /* speed threshold 4 register */
#define A3197S_PLLTABLE_ADDR               0x418
#define A3197S_PLLCNT1_ADDR                0x420 /* per-PLL locked core count register */
#define A3197S_SPDLOG_TIMER_ADDR           0x428 /* speed-log window timer register */
#define A3197S_SPDLOG_PASSCNT_ADDR         0x42c
#define A3197S_SPDLOG_FAILCNT_ADDR         0x430
#define A3197S_DH_CNT_WDT_CTRL_ADDR        0x438
#define A3197S_SPDLOG_RESET_ADDR           0x444
#define A3197S_GBP_CTRL_ADDR               0x448
#define A3197S_LFB_CTRL_ADDR               0x450
#define A3197S_SELFCAL_CTRL_ADDR           0x490
#define A3197S_SELFCAL_WDT_CTRL_ADDR       0x498
#define A3197S_THSR_WDT_TIMER_CTRL_ADDR    0x4b0
#define A3197S_SELFVEC_AGING_CTRL_ADDR     0x4b4
#define A3197S_SELFVEC_AGING_WDT_CTRL_ADDR 0x4c0
#define A3197S_SELFVEC_AGING_CFG_ADDR      0x4c4
#define A3197S_SELFVEC_AGING_TIMER_ADDR    0x4cc
#define A3197S_TOPRX_EN_0_ADDR             0x4e0
#define A3197S_TOPRX_EN_1_ADDR             0x4e4
#define A3197S_TOPRX_EN_2_ADDR             0x4e8
#define A3197S_TOPRX_EN_3_ADDR             0x4ec
#define A3197S_TOPRX_EN_4_ADDR             0x4f0

#define A3197S_PLL_COM_CFG_ADDR 0x3000
#define A3197S_PLL_POINTS       4 /* PLL frequency points per chip, count */

#define A3197S_PLL0L1_ADDR      0x3004
#define A3197S_PLL1L1_ADDR      0x300c
#define A3197S_PLL2L1_ADDR      0x3014
#define A3197S_PLL3L1_ADDR      0x301c
#define A3197S_PLL_CTRL01_ADDR  0x3034
#define A3197S_PLL_CTRL23_ADDR  0x3038
#define A3197S_MUX_ADDR         0x303c
#define A3197S_DIVCNT_CTRL_ADDR 0x3060

#define A3197S_EFUSE_CTRL_ADDR 0x3400
#define A3197S_EFUSE_FIFO_ADDR 0x3480

#define A3197S_TVSENW_ADDR  0x3700      /* PVT sensor control register */
#define A3197S_TVSENR_ADDR  0x3704      /* PVT sensor result register */
#define A3197S_TVSENR_READY 0x01000000u /* PVT sensor result ready bit */

#define A3197S_FB_SEL_ADDR       0x3c10
#define A3197S_IO_N_PAD_CFG_ADDR 0x3c14 /* north IO pad config register */
#define A3197S_IO_S_PAD_CFG_ADDR 0x3c18 /* south IO pad config register */

#define A3197S_UART_CFG1_ADDR    0x3c84
#define A3197S_UART_CFG3_ADDR    0x3c8c
#define A3197S_UART_STATUS       0x3ca0 /* UART status register */
#define A3197S_UART_TIMEOUT_ADDR 0x3ca4

#define A3197S_RI_WEN_ADDR  0x3f00
#define A3197S_REGRI_ADDR   0x3f04
#define A3197S_MX2_RST_ADDR 0x3f08

#define A3197S_DEFAULT_IO_N_PAD_CFG           0x3f333333 /* default north IO pad config */
#define A3197S_DEFAULT_IO_S_PAD_CFG           0x01f33333 /* default south IO pad config */
#define A3197S_DEFAULT_CFG                    0x800377ff
#define A3197S_DEFAULT_UART_CFG3              0x00522001
#define A3197S_DEFAULT_TBASE                  0x00000000
#define A3197S_DEFAULT_ROLLTIME               20000
#define A3197S_DEFAULT_CG_EN                  0x00000000
#define A3197S_DEFAULT_SPDSYC_CFG             0x80039700
#define A3197S_DEFAULT_FT                     0x04c08198
#define A3197S_DEFAULT_FTDBG                  0x80000100
#define A3197S_DEFAULT_FT_RAZOR               0x0000000e
#define A3197S_DEFAULT_SS_CTRL                0xffff0421
#define A3197S_DEFAULT_PLLTABLE               0x00004688
#define A3197S_DEFAULT_DH_CNT_WDT_CTRL        0xffff0001
#define A3197S_DEFAULT_GBP_CTRL               0x00000000
#define A3197S_DEFAULT_LFB_CTRL               0xfff80020
#define A3197S_DEFAULT_SELFCAL_CTRL           0x00000000
#define A3197S_DEFAULT_SELFCAL_WDT_CTRL       0x0001ffff
#define A3197S_DEFAULT_THSR_WDT_TIMER_CTRL    0x0001ffff
#define A3197S_DEFAULT_SELFVEC_AGING_CTRL     0x04000000
#define A3197S_DEFAULT_SELFVEC_AGING_WDT_CTRL 0x0001ffff
#define A3197S_DEFAULT_SELFVEC_AGING_CFG      0x00040000
#define A3197S_DEFAULT_SELFVEC_AGING_TIMER    0x00000190
#define A3197S_DEFAULT_TOPRX_EN_0             0xffffffff
#define A3197S_DEFAULT_TOPRX_EN_1             0xffffffff
#define A3197S_DEFAULT_TOPRX_EN_2             0xffffffff
#define A3197S_DEFAULT_TOPRX_EN_3             0xffffffff
#define A3197S_DEFAULT_TOPRX_EN_4             0x07ffffff
#define A3197S_DEFAULT_PLL_COM_CFG            0x005803ed
#define A3197S_DEFAULT_PLL1L1                 0x90084302
#define A3197S_DEFAULT_PLL2L1                 0x90084302
#define A3197S_DEFAULT_PLL3L1                 0x90084302
#define A3197S_DEFAULT_PLL_CTRL01             0x00000000
#define A3197S_DEFAULT_PLL_CTRL23             0x00000000
#define A3197S_DEFAULT_MUX                    0x108c000f
#define A3197S_DEFAULT_DIVCNT_CTRL            0x00000001
#define A3197S_DEFAULT_TVSENW                 0x00016380 /* default PVT sensor control */
#define A3197S_UART_CFG1_DEFAULT              0x12d65a

#endif /* A3197S_REG_H */
