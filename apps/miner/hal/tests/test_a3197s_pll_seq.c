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
#include <stdio.h>
#include <string.h>
#include "a3197s_decode.h"
#include "a3197s_pll_seq.h"

/* SS_CTRL (SmartSpeed control) register word */
union ss_mirror {
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

typedef enum { EV_CS, EV_SET, EV_GET, EV_DLY } ev_kind_t;
/* Logged hook call: CS (chain, chip), SET (addr, value), GET (addr), DLY (ticks) */
typedef struct {
    ev_kind_t k;
    uint32_t  a, b;
} ev_t;

static ev_t     g_ev[128];          /* hook calls in call order */
static int      g_n;                /* number of logged calls */
static uint32_t g_pllcnt_script[4]; /* values get_reg returns in turn */
static int      g_pllcnt_i;         /* index of the next get_reg value */
static int      g_i;                /* index of the next g_ev entry to verify */

static void     m_cs(uint8_t m, uint16_t c);
static void     m_set(uint8_t m, uint32_t a, uint32_t v);
static uint32_t m_get(uint8_t m, uint32_t a);
static uint16_t m_getcs(uint8_t m);
static void     m_dly(uint32_t t);

static const a3197s_pll_io_t IO = { m_cs, m_set, m_get, m_getcs, m_dly };

static void m_cs(uint8_t m, uint16_t c)
{
    g_ev[g_n++] = (ev_t){ EV_CS, m, c };
}
static void m_set(uint8_t m, uint32_t a, uint32_t v)
{
    (void)m;
    g_ev[g_n++] = (ev_t){ EV_SET, a, v };
}
static uint32_t m_get(uint8_t m, uint32_t a)
{
    (void)m;
    g_ev[g_n++] = (ev_t){ EV_GET, a, 0 };
    return g_pllcnt_script[g_pllcnt_i++];
}
static uint16_t m_getcs(uint8_t m)
{
    (void)m;
    return 7;
}
static void m_dly(uint32_t t)
{
    g_ev[g_n++] = (ev_t){ EV_DLY, t, 0 };
}

static void x_cs(uint32_t m, uint32_t c)
{
    assert(g_i < g_n);
    assert(g_ev[g_i].k == EV_CS && g_ev[g_i].a == m && g_ev[g_i].b == c);
    g_i++;
}
static void x_set(uint32_t a, uint32_t v)
{
    assert(g_i < g_n);
    assert(g_ev[g_i].k == EV_SET && g_ev[g_i].a == a && g_ev[g_i].b == v);
    g_i++;
}
static void x_get(uint32_t a)
{
    assert(g_i < g_n);
    assert(g_ev[g_i].k == EV_GET && g_ev[g_i].a == a);
    g_i++;
}
static void x_dly(uint32_t t)
{
    assert(g_i < g_n);
    assert(g_ev[g_i].k == EV_DLY && g_ev[g_i].a == t);
    g_i++;
}

static a3197s_pll_cfg_t base_cfg(void)
{
    a3197s_pll_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.freq[0]    = 409;
    c.freq[1]    = 412;
    c.freq[2]    = 415;
    c.freq[3]    = 418;
    c.init_freq  = 100;
    c.ssdn_pro   = 1;
    c.spdlow     = 2;
    c.spdhigh    = 5;
    c.ss_enable  = 1;
    c.pll_select = 3;
    c.ss_sel     = 1;
    c.ss_pri     = 0;
    c.ss_pd      = 0x1234;
    return c;
}

static void ss_words(const a3197s_pll_cfg_t *c, uint32_t *park, uint32_t *sw, uint32_t *en)
{
    union ss_mirror s;
    s.raw          = 0;
    s.ssdn_protect = c->ssdn_pro;
    s.spdlow       = c->spdlow;
    s.spdhigh      = c->spdhigh;
    s.spd_en       = 0;
    s.glbsetvld    = 1;
    s.glbset       = 1;
    s.ss_sel       = c->ss_sel;
    s.ss_pri       = c->ss_pri;
    s.ss_pd        = c->ss_pd;
    *park          = s.raw;
    s.glbset       = 2;
    *sw            = s.raw;
    s.spd_en       = c->ss_enable;
    s.glbset       = c->pll_select;
    *en            = s.raw;
}

static void run(uint16_t asic, uint8_t pll0_update, const a3197s_pll_cfg_t *c)
{
    g_n                = 0;
    g_i                = 0;
    g_pllcnt_i         = 0;
    g_pllcnt_script[0] = (uint32_t)A3197S_CORE_COUNT;
    g_pllcnt_script[1] = (uint32_t)A3197S_CORE_COUNT << 8;
    a3197s_set_pll_seq(&IO, 0, asic, pll0_update, c);
}

static void expect_steps12(uint16_t asic, const a3197s_pll_cfg_t *c, uint32_t ss_park)
{
    uint16_t verify_cs = (asic == A3197S_CHIP_BROADCAST);
    x_cs(0, asic);
    x_set(A3197S_SS_CTRL_ADDR, ss_park);
    x_set(A3197S_SPDSYC_CFG_ADDR, A3197S_DEFAULT_SPDSYC_CFG);
    x_dly(5);
    if (verify_cs)
        x_cs(0, 0);
    x_get(A3197S_PLLCNT1_ADDR);
    x_cs(0, asic);
    x_set(A3197S_PLL1L1_ADDR, get_cpm_pll(c->freq[1]));
    x_set(A3197S_PLL2L1_ADDR, get_cpm_pll(c->freq[2]));
    x_set(A3197S_PLL3L1_ADDR, get_cpm_pll(c->freq[3]));
    x_set(A3197S_ROLLTIME_ADDR, A3197S_TIMEOUT_CONST / c->freq[3]);
    x_dly(1);
}

static void expect_steps34(uint16_t asic, const a3197s_pll_cfg_t *c, uint32_t ss_sw)
{
    uint16_t verify_cs = (asic == A3197S_CHIP_BROADCAST);
    uint32_t mux1      = (A3197S_DEFAULT_MUX & (uint32_t)(~(0x0F << 21))) |
            ((uint32_t)calc_mux_div_mode(c->freq[1]) << 21) | (1u << 4);
    uint32_t mux0 = (A3197S_DEFAULT_MUX & (uint32_t)(~(0x0F << 21))) |
            ((uint32_t)calc_mux_div_mode(c->freq[0]) << 21);
    x_cs(0, asic);
    x_set(A3197S_SS_CTRL_ADDR, ss_sw);
    x_set(A3197S_SPDSYC_CFG_ADDR, A3197S_DEFAULT_SPDSYC_CFG);
    x_dly(5);
    if (verify_cs)
        x_cs(0, 0);
    x_get(A3197S_PLLCNT1_ADDR);
    x_cs(0, asic);
    x_set(A3197S_MUX_ADDR, mux1);
    x_set(A3197S_PLL0L1_ADDR, get_cpm_pll(c->freq[0]));
    x_dly(1);
    x_set(A3197S_MUX_ADDR, mux0);
    x_dly(1);
}

static void expect_step5(uint16_t asic, uint32_t ss_en)
{
    x_cs(0, asic);
    x_set(A3197S_SS_CTRL_ADDR, ss_en);
    x_set(A3197S_SPDSYC_CFG_ADDR, A3197S_DEFAULT_SPDSYC_CFG);
    x_cs(0, 7);
}

int main(void)
{
    a3197s_pll_cfg_t c = base_cfg();
    uint32_t         ss_park, ss_sw, ss_en;
    ss_words(&c, &ss_park, &ss_sw, &ss_en);

    run(5, 0, &c);
    x_cs(0, 5);
    x_set(A3197S_PLL0L1_ADDR, get_cpm_pll(c.init_freq) & 0xf7fffffe);
    x_dly(1);
    expect_steps12(5, &c, ss_park);
    expect_step5(5, ss_en);
    assert(g_i == g_n);

    run(5, 1, &c);
    expect_steps12(5, &c, ss_park);
    expect_steps34(5, &c, ss_sw);
    expect_step5(5, ss_en);
    assert(g_i == g_n);

    run(A3197S_CHIP_BROADCAST, 1, &c);
    expect_steps12(A3197S_CHIP_BROADCAST, &c, ss_park);
    expect_steps34(A3197S_CHIP_BROADCAST, &c, ss_sw);
    expect_step5(A3197S_CHIP_BROADCAST, ss_en);
    assert(g_i == g_n);

    printf("a3197s pll safe-sequence pinned OK\n");
    return 0;
}
