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

#include "osif.h"
#include "a3197s_decode.h"
#include "a3197s_pll_seq.h"

/* SS_CTRL (SmartSpeed control) register word */
union a3197s_ss_ctrl {
    struct {
        uint32_t glbset : 3; /* global PLL select */
        uint32_t glbsetvld : 1;
        uint32_t ssdn_protect : 1;
        uint32_t spdlow : 3;
        uint32_t spdhigh : 3;
        uint32_t spd_en : 1;
        uint32_t ss_sel : 2;
        uint32_t ss_pri : 1;
        uint32_t glb_limit : 1;
        uint32_t ss_pd : 16;
    };
    uint32_t raw;
};

uint8_t calc_mux_div_mode(uint32_t freq)
{
    if (freq < 400)
        return 4;
    if (freq < 600)
        return 6;
    if (freq < 800)
        return 8;
    return 10;
}

static uint32_t mux_word(uint32_t freq)
{
    return (A3197S_DEFAULT_MUX & ~(0x0Fu << 21)) | ((uint32_t)calc_mux_div_mode(freq) << 21);
}

static void ss_ctrl_write(const a3197s_pll_io_t *io, uint8_t miner_id, uint16_t asic_id,
        uint32_t ss_ctrl)
{
    io->chip_select(miner_id, asic_id);
    io->set_reg(miner_id, A3197S_SS_CTRL_ADDR, ss_ctrl);
    io->set_reg(miner_id, A3197S_SPDSYC_CFG_ADDR, A3197S_DEFAULT_SPDSYC_CFG);
}

static void pll_lock_check(const a3197s_pll_io_t *io, uint8_t miner_id, uint16_t asic_id,
        const char *pll, uint32_t want)
{
    uint32_t cnt;

    io->delay_ticks(5);
    if (asic_id == A3197S_CHIP_BROADCAST)
        io->chip_select(miner_id, 0);

    cnt = io->get_reg(miner_id, A3197S_PLLCNT1_ADDR);
    if (cnt != want)
        osif_log(OSIF_LOG_ERR,
                "board %d asic %d: %s did not lock (lock count 0x%08x, expected 0x%08x)", miner_id,
                asic_id, pll, cnt, want);
}

void a3197s_set_pll_seq(const a3197s_pll_io_t *io, uint8_t miner_id, uint16_t asic_id,
        uint8_t pll0_update, const a3197s_pll_cfg_t *cfg)
{
    union a3197s_ss_ctrl ss_ctrl;
    uint16_t             chip_select_id;

    chip_select_id = io->get_chip_select(miner_id);

    if (!pll0_update) {
        io->chip_select(miner_id, asic_id);
        io->set_reg(miner_id, A3197S_PLL0L1_ADDR, get_cpm_pll(cfg->init_freq) & 0xf7fffffe);
        io->delay_ticks(1);
    }

    ss_ctrl.raw          = 0;
    ss_ctrl.ssdn_protect = cfg->ssdn_pro;
    ss_ctrl.spdlow       = cfg->spdlow;
    ss_ctrl.spdhigh      = cfg->spdhigh;
    ss_ctrl.glbsetvld    = 1;
    ss_ctrl.glbset       = 1;
    ss_ctrl.ss_sel       = cfg->ss_sel;
    ss_ctrl.ss_pri       = cfg->ss_pri;
    ss_ctrl.ss_pd        = cfg->ss_pd;
    ss_ctrl_write(io, miner_id, asic_id, ss_ctrl.raw);
    pll_lock_check(io, miner_id, asic_id, "PLL0", A3197S_CORE_COUNT);

    io->chip_select(miner_id, asic_id);
    io->set_reg(miner_id, A3197S_PLL1L1_ADDR, get_cpm_pll(cfg->freq[1]));
    io->set_reg(miner_id, A3197S_PLL2L1_ADDR, get_cpm_pll(cfg->freq[2]));
    io->set_reg(miner_id, A3197S_PLL3L1_ADDR, get_cpm_pll(cfg->freq[3]));
    io->set_reg(miner_id, A3197S_ROLLTIME_ADDR, A3197S_TIMEOUT_CONST / cfg->freq[3]);
    io->delay_ticks(1);

    if (pll0_update) {
        ss_ctrl.glbset = 2;
        ss_ctrl_write(io, miner_id, asic_id, ss_ctrl.raw);
        pll_lock_check(io, miner_id, asic_id, "PLL1", A3197S_CORE_COUNT << 8);

        io->chip_select(miner_id, asic_id);
        io->set_reg(miner_id, A3197S_MUX_ADDR, mux_word(cfg->freq[1]) | (1 << 4));
        io->set_reg(miner_id, A3197S_PLL0L1_ADDR, get_cpm_pll(cfg->freq[0]));
        io->delay_ticks(1);
        io->set_reg(miner_id, A3197S_MUX_ADDR, mux_word(cfg->freq[0]));
        io->delay_ticks(1);
    }

    ss_ctrl.spd_en = cfg->ss_enable;
    ss_ctrl.glbset = cfg->pll_select;
    ss_ctrl_write(io, miner_id, asic_id, ss_ctrl.raw);

    io->chip_select(miner_id, chip_select_id);
}
