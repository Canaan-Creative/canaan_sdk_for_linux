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
#include "tst_board_expect.h"

#define F0     392u /* PLL0 target frequency, MHz */
#define F1     412u /* PLL1 target frequency, MHz */
#define F2     432u /* PLL2 target frequency, MHz */
#define F3     452u /* PLL3 target frequency, MHz */
#define F_INIT 100u /* PLL0 park frequency, MHz */

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

static void run(uint8_t pll0_update)
{
    a3197s_pll_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.freq[0]          = F0;
    c.freq[1]          = F1;
    c.freq[2]          = F2;
    c.freq[3]          = F3;
    c.init_freq        = F_INIT;
    c.ssdn_pro         = 1;
    c.spdlow           = 2;
    c.spdhigh          = 5;
    c.ss_enable        = 1;
    c.pll_select       = 2;
    c.ss_sel           = 1;
    c.ss_pri           = 0;
    c.ss_pd            = 0x1234;
    g_n                = 0;
    g_pllcnt_i         = 0;
    g_pllcnt_script[0] = (uint32_t)A3197S_CORE_COUNT;
    g_pllcnt_script[1] = (uint32_t)A3197S_CORE_COUNT << 8;
    a3197s_set_pll_seq(&IO, 0, 5, pll0_update, &c);
}

static int n_writes(uint32_t addr)
{
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_ev[i].k == EV_SET && g_ev[i].a == addr)
            n++;
    return n;
}

static uint32_t nth_write(uint32_t addr, int nth)
{
    for (int i = 0; i < g_n; i++)
        if (g_ev[i].k == EV_SET && g_ev[i].a == addr && nth-- == 0)
            return g_ev[i].b;
    assert(0 && "expected register write not issued");
    return 0;
}

static uint32_t step1_glbset(void)
{
    return nth_write(A3197S_SS_CTRL_ADDR, 0) & 0x7u;
}

static uint32_t mux_word(uint32_t div_mode, int src_pll1)
{
    return (A3197S_DEFAULT_MUX & (uint32_t)(~(0x0F << 21))) | (div_mode << 21) |
            (src_pll1 ? (1u << 4) : 0u);
}

int main(void)
{
    run(0);
    assert(nth_write(A3197S_PLL0L1_ADDR, 0) == (get_cpm_pll(F_INIT) & 0xf7fffffe));

    assert(step1_glbset() == 1);
    assert(n_writes(A3197S_MUX_ADDR) == 0);
    assert(n_writes(A3197S_SS_CTRL_ADDR) == 2);
    assert(n_writes(A3197S_PLL1L1_ADDR) == 1);
    assert(n_writes(A3197S_PLL0L1_ADDR) == 1);

    run(1);
    assert(n_writes(A3197S_SS_CTRL_ADDR) == 3);

    assert(step1_glbset() == 1);
    assert(n_writes(A3197S_PLL0L1_ADDR) == 1);
    assert(nth_write(A3197S_PLL0L1_ADDR, 0) == get_cpm_pll(F0));
    assert(n_writes(A3197S_MUX_ADDR) == 2);
    assert(nth_write(A3197S_MUX_ADDR, 0) == mux_word(calc_mux_div_mode(F1), 1));
    assert(nth_write(A3197S_MUX_ADDR, 1) == mux_word(calc_mux_div_mode(F0), 0));

    printf("a3197s safe PLL sequence for %s pinned OK\n", BOARD_NAME);
    return 0;
}
