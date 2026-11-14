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
#include "a3197s_pll_seq.h"

#define MAX_CS 64 /* capacity of the call logs */

static uint8_t  g_cs_m[MAX_CS]; /* chain id of each chip_select call */
static uint16_t g_cs_c[MAX_CS]; /* chip address of each chip_select call */
static int      g_cs_n;         /* number of chip_select calls */

static uint8_t g_getcs_m[MAX_CS]; /* chain id of each get_chip_select call */
static int     g_getcs_n;         /* number of get_chip_select calls */

static uint32_t g_pllcnt_script[4]; /* values get_reg returns in turn, cycling */
static int      g_pllcnt_i;         /* count of get_reg calls */

static void     m_cs(uint8_t m, uint16_t c);
static void     m_set(uint8_t m, uint32_t a, uint32_t v);
static uint32_t m_get(uint8_t m, uint32_t a);
static uint16_t m_getcs(uint8_t m);
static void     m_dly(uint32_t t);

/* hooks that log the calls instead of driving a chain */
static const a3197s_pll_io_t IO = { m_cs, m_set, m_get, m_getcs, m_dly };

static uint16_t prior_sel(uint8_t miner_id)
{
    return (uint16_t)(0x100 + miner_id);
}

static void m_cs(uint8_t m, uint16_t c)
{
    assert(g_cs_n < MAX_CS);
    g_cs_m[g_cs_n] = m;
    g_cs_c[g_cs_n] = c;
    g_cs_n++;
}
static void m_set(uint8_t m, uint32_t a, uint32_t v)
{
    (void)m;
    (void)a;
    (void)v;
}
static uint32_t m_get(uint8_t m, uint32_t a)
{
    (void)m;
    (void)a;
    return g_pllcnt_script[g_pllcnt_i++ & 3];
}
static uint16_t m_getcs(uint8_t m)
{
    assert(g_getcs_n < MAX_CS);
    g_getcs_m[g_getcs_n++] = m;
    return prior_sel(m);
}
static void m_dly(uint32_t t)
{
    (void)t;
}

static a3197s_pll_cfg_t cfg(void)
{
    a3197s_pll_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.freq[0]    = 392;
    c.freq[1]    = 412;
    c.freq[2]    = 432;
    c.freq[3]    = 452;
    c.init_freq  = 100;
    c.ssdn_pro   = 1;
    c.spdlow     = 2;
    c.spdhigh    = 5;
    c.ss_enable  = 1;
    c.pll_select = 2;
    c.ss_sel     = 1;
    c.ss_pri     = 0;
    c.ss_pd      = 0x1234;
    return c;
}

static void check_bracket(uint8_t miner_id, uint16_t asic_id, uint8_t pll0_update)
{
    a3197s_pll_cfg_t c = cfg();
    int              i;

    g_cs_n             = 0;
    g_getcs_n          = 0;
    g_pllcnt_i         = 0;
    g_pllcnt_script[0] = (uint32_t)A3197S_CORE_COUNT;
    g_pllcnt_script[1] = (uint32_t)A3197S_CORE_COUNT << 8;
    g_pllcnt_script[2] = (uint32_t)A3197S_CORE_COUNT << 16;
    g_pllcnt_script[3] = (uint32_t)A3197S_CORE_COUNT << 16;

    a3197s_set_pll_seq(&IO, miner_id, asic_id, pll0_update, &c);

    assert(g_getcs_n == 1);
    assert(g_getcs_m[0] == miner_id);

    assert(g_cs_n > 0);
    for (i = 0; i < g_cs_n; i++)
        assert(g_cs_m[i] == miner_id);

    assert(g_cs_c[g_cs_n - 1] == prior_sel(miner_id));
}

int main(void)
{
    check_bracket(0, 5, 0);
    check_bracket(0, 5, 1);
    check_bracket(0, A3197S_CHIP_BROADCAST, 1);

    check_bracket(1, 5, 0);
    check_bracket(1, 5, 1);
    check_bracket(1, A3197S_CHIP_BROADCAST, 1);
    check_bracket(2, 5, 1);
    check_bracket(2, A3197S_CHIP_BROADCAST, 0);

    printf("a3197s pll chip-select save/restore is per miner OK\n");
    return 0;
}
