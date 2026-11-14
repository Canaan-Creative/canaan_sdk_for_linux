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

#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osif.h"
#include "boardconf.h"
#include "miner_ipc.h"

#include "../../tests/include/tst_check.h"

#undef ASIC_MINER_VERSION
/* Fixed asic_miner version reported in MSG_ASIC_VERSION */
#define ASIC_MINER_VERSION "26091103_2138_1234567"

#define TST_PASSCNT 200000u /* Speed-log pass count every chip reports */

/* Frequency in the MSG_ASIC_VF sent during bring-up, MHz */
#define TST_VF_FREQ ((uint16_t)(INIT_FREQ + 50))

/* Chips whose CF read fails in the failed-read case */
#define CFFAIL_N (MINER_MAX_CHIPS >= 44 ? 4 : 2)
/* Expected shipped Freq with CFFAIL_N chips unread and the rest at 357 MHz, MHz */
#define CFFAIL_SHIP (MINER_MAX_CHIPS >= 44 ? 324 : 297)

/* Simulated PLL registers of one chip */
typedef struct {
    uint8_t  cores[ASIC_PLL_POINTS]; /* Cores on each PLL point */
    uint32_t word[ASIC_PLL_POINTS];  /* PLL word of each point, read back as MHz */
    uint8_t  gated;                  /* Value of A3197S_CHKDROP_ADDR */
} tst_chipreg;

static asic_telem tst_tm;                        /* Last MSG_ASIC_TELEM payload sent */
static int        tst_tm_n;                      /* MSG_ASIC_TELEM messages sent */
static int        version_messages;              /* MSG_ASIC_VERSION messages sent */
static char reported_version[MINER_VERSION_CAP]; /* Version string of the last MSG_ASIC_VERSION */

/* Replaces the stub a3197s_read_reg() when set */
extern int (*stub_read_reg_hook)(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n);
extern uint32_t (*stub_cpm_freq_hook)(uint32_t pll); /* Replaces the stub get_cpm_freq() when set */
extern int stub_enum_chips;
extern int stub_pll_commit_by_board[]; /* Stub PLL commits per board */
extern int stub_seq_n;                 /* Stub PLL commits, including those not logged */
/* Board and update flag of each logged stub PLL commit */
extern uint8_t  stub_seq_board[], stub_seq_upd[];
extern uint32_t stub_seq_base[]; /* Base frequency of each logged stub PLL commit */

/* Sleeps taken; sleep count that stops the main loop, 0 for none */
static int tst_sleeps, tst_sleep_cap;
/* Simulated PLL registers per board and chip */
static tst_chipreg tst_reg[HBOARD_COUNT][MINER_MAX_CHIPS];

/* Send MSG_ASIC_VF during bring-up; it was sent; board 0 was in BS_RI then */
static int tst_vf_arm, tst_vf_sent, tst_vf_in_ri;

static void tst_on_sleep(void);

int fan_dev_get_rpm(int fan)
{
    (void)fan;
    return 0;
}

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    return OSIF_OK;
}
void osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (type == MSG_ASIC_VERSION && data && len == sizeof ASIC_MINER_VERSION) {
        memcpy(reported_version, data, (size_t)len);
        version_messages++;
    }
    if (type == MSG_ASIC_TELEM && data && len == (int)sizeof(asic_chip_telem)) {
        memcpy(&tst_tm, data, sizeof tst_tm);
        tst_tm_n++;
    }
    return OSIF_OK;
}
osif_status_t osif_lock_create(osif_lock_t **l)
{
    if (l)
        *l = NULL;
    return OSIF_OK;
}
void osif_lock_acquire(osif_lock_t *l)
{
    (void)l;
}
void osif_lock_release(osif_lock_t *l)
{
    (void)l;
}
uint64_t osif_now_ms(void)
{
    return 0;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
    if (tst_sleep_cap)
        tst_on_sleep();
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static uint32_t tst_cpm_freq(uint32_t pll)
{
    return pll;
}

static int tst_read_reg(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    if (out)
        memset(out, 0, (size_t)n * sizeof *out);
    if (!out || n < 1)
        return 0;
    const int          b = ch < HBOARD_COUNT ? ch : 0;
    const tst_chipreg *r = &tst_reg[b][g_rs.chip];
    switch (addr) {
    case A3197S_CHKDROP_ADDR:
        out[0] = r->gated;
        break;
    case A3197S_PLLCNT1_ADDR:
        out[0] = (uint32_t)r->cores[0] | ((uint32_t)r->cores[1] << 8) |
                ((uint32_t)r->cores[2] << 16) | ((uint32_t)r->cores[3] << 24);
        break;
    case A3197S_PLL0L1_ADDR:
        out[0] = r->word[0];
        break;
    case A3197S_PLL1L1_ADDR:
        out[0] = r->word[1];
        break;
    case A3197S_PLL2L1_ADDR:
        out[0] = r->word[2];
        break;
    case A3197S_PLL3L1_ADDR:
        out[0] = r->word[3];
        break;
    case A3197S_SPDLOG_TIMER_ADDR:
        out[0] = SS_TIMER_VALID_VALUE;
        break;
    case A3197S_SPDLOG_PASSCNT_ADDR:
        out[0] = TST_PASSCNT;
        break;
    case A3197S_SPDLOG_FAILCNT_ADDR:
        out[0] = 0u;
        break;
    default:
        break;
    }
    return 0;
}

static void chain_set(int b, int chips, const uint8_t cores[ASIC_PLL_POINTS],
        const uint32_t word[ASIC_PLL_POINTS], uint8_t gated)
{
    for (int c = 0; c < chips; c++) {
        for (int k = 0; k < ASIC_PLL_POINTS; k++) {
            tst_reg[b][c].cores[k] = cores[k];
            tst_reg[b][c].word[k]  = word[k];
        }
        tst_reg[b][c].gated = gated;
    }
}

static void run_window(uint8_t ch, int chips)
{
    g_bd[ch].chips_n = chips;
    g_rs.type        = RS_SPDCNT;
    g_rs.chip        = 0;
    for (int i = 0; i < chips; i++) {
        g_rs.board = ch;
        asic_collect_status();
    }
    g_rs.type = RS_SPDLOG;
    g_rs.chip = 0;
    for (int i = 0; i < chips; i++) {
        g_rs.board = ch;
        asic_collect_status();
    }
}

static void publish(uint8_t ch, int chips)
{
    tst_tm_n         = 0;
    version_messages = 0;
    memset(&tst_tm, 0, sizeof tst_tm);
    g_bd[ch].chips_n = chips;
    asic_send_info(ch);
}

static uint16_t helper_freq(int b, int chips, uint16_t fallback)
{
    uint64_t core_mhz = 0;
    uint32_t cores    = 0;
    for (int c = 0; c < chips && c < MINER_MAX_CHIPS; c++) {
        for (int k = 0; k < ASIC_PLL_POINTS; k++) {
            if (g_chip_cf[b][c][k] == 0)
                continue;
            core_mhz += (uint64_t)g_chip_pllcnt[b][c][k] * g_chip_cf[b][c][k];
            cores += g_chip_pllcnt[b][c][k];
        }
    }
    if (cores == 0)
        return fallback;
    return (uint16_t)((core_mhz + cores / 2) / cores);
}

static void tst_on_sleep(void)
{
    if (++tst_sleeps >= tst_sleep_cap)
        g_run = 0;
    if (!tst_vf_arm || tst_vf_sent)
        return;
    if (g_bd[0].chips_n <= 0 || stub_pll_commit_by_board[0] < 1)
        return;
    mc_asic_vf v;
    memset(&v, 0, sizeof v);
    for (int i = 0; i < ASIC_PLL_POINTS; i++)
        v.freq[i] = TST_VF_FREQ;
    tst_vf_in_ri = g_bd[0].stage == BS_RI;
    ipc_cb(MSG_ASIC_VF, &v, (int)sizeof v);
    tst_vf_sent = 1;
}

static void run_main_from_ri(int vf)
{
    memset(g_bd, 0, sizeof g_bd);
    memset(g_pll_freq, 0, sizeof g_pll_freq);
    g_pll_at_target = 0;
    memset(stub_pll_commit_by_board, 0, sizeof(int) * HBOARD_COUNT);
    stub_seq_n      = 0;
    stub_enum_chips = MINER_MAX_CHIPS;
    tst_vf_arm      = vf;
    tst_vf_sent = tst_vf_in_ri = 0;
    tst_sleeps                 = 0;
    tst_sleep_cap              = 200;
    g_run                      = 1;
    (void)asic_miner_main_unused();
    tst_sleep_cap = 0;
    tst_vf_arm    = 0;
}

static int board0_commits(uint32_t base, int upd)
{
    int n = 0;
    for (int i = 0; i < stub_seq_n; i++)
        if (stub_seq_board[i] == 0 && stub_seq_base[i] == base && stub_seq_upd[i] == upd)
            n++;
    return n;
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chips/board, %d cores/chip\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, (int)A3197S_CORE_COUNT);

    stub_read_reg_hook = tst_read_reg;
    stub_cpm_freq_hook = tst_cpm_freq;

    const int      chips         = MINER_MAX_CHIPS;
    const uint32_t nominal_cores = (uint32_t)chips * A3197S_CORE_COUNT;

    g_bd[0].work_freq = 357;
    g_mhsmm_win       = 0;
    publish(0, chips);
    CHECK(version_messages == 1 && !strcmp(reported_version, ASIC_MINER_VERSION),
            "ASIC publishes its own compiled version once to core");
    CHECK(tst_tm_n == 2,
            "asic_send_info published MSG_ASIC_TELEM to BOTH peers — core_miner "
            "for the :4028 display and mm_miner for the control loops (got %d)",
            tst_tm_n);
    CHECK(tst_tm.mhs_mm == nominal_cores * 357u * 8u,
            "no window yet: GHSmm is the NOMINAL %u cores x 357 MHz x 8 (%u, got %u)",
            nominal_cores, nominal_cores * 357u * 8u, tst_tm.mhs_mm);
    CHECK(tst_tm.freq_mhz == 357,
            "no window yet: Freq round-trips the ramp base EXACTLY (357, got %u)",
            (unsigned)tst_tm.freq_mhz);

    {
        const uint8_t  cores[4] = { 0, 0, 0, A3197S_CORE_COUNT };
        const uint32_t word[4]  = { 0, 0, 0, 357 };
        g_bd[0].work_freq       = 100;
        chain_set(0, chips, cores, word, 0);
        run_window(0, chips);
        publish(0, chips);

        CHECK(g_chip_pllcnt[0][chips - 1][3] == A3197S_CORE_COUNT &&
                        g_chip_cf[0][chips - 1][3] == 357,
                "RS_SPDCNT decoded PLLCNT1 byte 3 and PLL3L1 into the last chip's "
                "row (%u cores @ %u MHz)",
                (unsigned)g_chip_pllcnt[0][chips - 1][3], (unsigned)g_chip_cf[0][chips - 1][3]);

        CHECK(tst_tm.mhs_mm == nominal_cores * 357u * 8u,
                "all CF landed: GHSmm is the weighted sum x8 (%u, got %u)",
                nominal_cores * 357u * 8u, tst_tm.mhs_mm);
        CHECK(tst_tm.freq_mhz == 357, "all CF landed, nothing gated: shipped Freq = 357 (got %u)",
                (unsigned)tst_tm.freq_mhz);
        CHECK(helper_freq(0, chips, 0) == 357,
                "...and the helper AGREES at 357 (got %u) — the two routes are "
                "identical only here",
                (unsigned)helper_freq(0, chips, 0));
    }

    {
        const uint8_t  half     = (uint8_t)(A3197S_CORE_COUNT / 2);
        const uint8_t  cores[4] = { half, 0, 0, half };
        const uint32_t word[4]  = { 300, 0, 0, 357 };
        chain_set(0, chips, cores, word, 0);
        run_window(0, chips);
        publish(0, chips);

        CHECK(g_chip_pllcnt[0][0][0] == half && g_chip_pllcnt[0][0][3] == half &&
                        g_chip_cf[0][0][0] == 300 && g_chip_cf[0][0][3] == 357,
                "RS_SPDCNT keeps the PLL points APART (pll0 %u cores @ %u MHz, "
                "pll3 %u @ %u)",
                (unsigned)g_chip_pllcnt[0][0][0], (unsigned)g_chip_cf[0][0][0],
                (unsigned)g_chip_pllcnt[0][0][3], (unsigned)g_chip_cf[0][0][3]);

        CHECK(tst_tm.freq_mhz == 328, "split 300/357: shipped Freq TRUNCATES 328.5 to 328 (got %u)",
                (unsigned)tst_tm.freq_mhz);
        CHECK(helper_freq(0, chips, 0) == 329,
                "...while the helper ROUNDS the same sums to 329 (got %u) — "
                "1 MHz apart with no gating and no failed read",
                (unsigned)helper_freq(0, chips, 0));
    }

    {
        const uint8_t  cores[4] = { 0, 0, 0, A3197S_CORE_COUNT };
        const uint32_t ok[4]    = { 0, 0, 0, 357 };
        const uint32_t bad[4]   = { 0, 0, 0, 0 };
        chain_set(0, chips, cores, ok, 0);
        for (int c = 0; c < CFFAIL_N; c++)
            for (int k = 0; k < ASIC_PLL_POINTS; k++)
                tst_reg[0][c].word[k] = bad[k];
        run_window(0, chips);
        publish(0, chips);

        CHECK(g_chip_pllcnt[0][0][3] == A3197S_CORE_COUNT && g_chip_cf[0][0][3] == 0,
                "a failed CF read leaves the cores KNOWN and the clock 0 "
                "(%u cores @ %u MHz)",
                (unsigned)g_chip_pllcnt[0][0][3], (unsigned)g_chip_cf[0][0][3]);
        CHECK(tst_tm.freq_mhz == CFFAIL_SHIP,
                "%d of %d CF reads timed out: shipped Freq = %d (got %u) — the "
                "silent chips are still in the denominator",
                CFFAIL_N, chips, CFFAIL_SHIP, (unsigned)tst_tm.freq_mhz);
        CHECK(helper_freq(0, chips, 0) == 357,
                "...while the helper reports 357 (got %u) — the clock of the "
                "cores that ANSWERED",
                (unsigned)helper_freq(0, chips, 0));
    }

    {
        const uint8_t  locked   = (uint8_t)(A3197S_CORE_COUNT - 12);
        const uint8_t  cores[4] = { 0, 0, 0, locked };
        const uint32_t word[4]  = { 0, 0, 0, 357 };
        chain_set(0, chips, cores, word, 12);
        run_window(0, chips);
        publish(0, chips);

        CHECK(tst_tm.freq_mhz == 328,
                "%u of %d cores gated: shipped Freq = 328 (got %u) — gated cores "
                "are in the divisor at an implied 0 MHz",
                (unsigned)(chips * 12), (int)nominal_cores, (unsigned)tst_tm.freq_mhz);
        CHECK(helper_freq(0, chips, 0) == 357,
                "...while the helper reports 357 (got %u) — the clock of the "
                "cores that are RUNNING",
                (unsigned)helper_freq(0, chips, 0));
    }

    {
        const uint8_t  cores[4] = { 0, 0, 0, A3197S_CORE_COUNT };
        const uint32_t word[4]  = { 0, 0, 0, 0 };
        g_bd[0].work_freq       = 400;
        chain_set(0, chips, cores, word, 0);
        run_window(0, chips);
        publish(0, chips);

        CHECK(g_mhsmm_win == 0,
                "every CF read failed: the window computes 0, not a low number "
                "(got %u)",
                g_mhsmm_win);
        CHECK(tst_tm.freq_mhz == 400, "...so shipped Freq falls back to the ramp base 400 (got %u)",
                (unsigned)tst_tm.freq_mhz);
        CHECK(helper_freq(0, chips, 400) == 400,
                "...and the helper returns its fallback too (got %u)",
                (unsigned)helper_freq(0, chips, 400));
    }

#if HBOARD_COUNT > 1
    {
        const uint8_t  cores[4] = { 0, 0, 0, A3197S_CORE_COUNT };
        const uint32_t w0[4]    = { 0, 0, 0, 400 };
        const uint32_t w1[4]    = { 0, 0, 0, 300 };
        const uint32_t want_mhs = (uint32_t)chips * A3197S_CORE_COUNT * 8u * (400u + 300u);

        g_bd[0].work_freq = 357;
        g_bd[1].work_freq = 357;
        chain_set(0, chips, cores, w0, 0);
        run_window(0, chips);
        publish(0, chips);
        const uint32_t spd_one = tst_tm.ghs_spd_mhs;
        CHECK(tst_tm.freq_mhz == 400, "one board swept so far: Freq is that board's 400 (got %u)",
                (unsigned)tst_tm.freq_mhz);

        chain_set(1, chips, cores, w1, 0);
        run_window(1, chips);

        publish(1, chips);
        CHECK(tst_tm.mhs_mm == want_mhs,
                "board 1's tick reports the WHOLE machine's GHSmm — %d chips at "
                "400 MHz plus %d at 300 (%u, got %u)",
                chips, chips, want_mhs, tst_tm.mhs_mm);
        CHECK(tst_tm.freq_mhz == 350,
                "...so Freq against core_miner's machine-wide divisor is the "
                "350 MHz average, not board 1's own 300 (got %u)",
                (unsigned)tst_tm.freq_mhz);
        CHECK(tst_tm.ghs_spd_mhs == spd_one * 2u,
                "...and GHSspd sums both chains' spdlog pass counts (%u, got %u)", spd_one * 2u,
                tst_tm.ghs_spd_mhs);

        const uint32_t mhs1 = tst_tm.mhs_mm, spd1 = tst_tm.ghs_spd_mhs;
        const uint16_t f1 = tst_tm.freq_mhz;
        publish(0, chips);
        CHECK(tst_tm.mhs_mm == mhs1 && tst_tm.freq_mhz == f1 && tst_tm.ghs_spd_mhs == spd1,
                "...and board 0's tick reports the SAME three numbers — the rate "
                "may not depend on whose tick published it (got %u/%u/%u)",
                tst_tm.mhs_mm, (unsigned)tst_tm.freq_mhz, tst_tm.ghs_spd_mhs);

        g_bd[0].stage = BS_RUN;
        g_bd[1].stage = BS_COREREGS;
        publish(1, chips);
        CHECK(tst_tm.in_work == 1,
                "board 1's tick reports the MACHINE in work while board 0 hashes "
                "(got %u)",
                (unsigned)tst_tm.in_work);
    }
#endif /* HBOARD_COUNT > 1 */

    run_main_from_ri(1);
    CHECK(tst_vf_sent && tst_vf_in_ri,
            "a VF of %u MHz reached asic_miner while board 0 was in BS_RI "
            "(sent %d, in RI %d)",
            (unsigned)TST_VF_FREQ, tst_vf_sent, tst_vf_in_ri);
    CHECK(board0_commits(TST_VF_FREQ, 0) == 1,
            "...and was applied there once with pll0_update=0 (got %d)",
            board0_commits(TST_VF_FREQ, 0));
    CHECK(g_bd[0].ri_sta == 1 && board0_commits(TST_VF_FREQ, 1) == 1,
            "...so after RI exits the same points are applied again with "
            "pll0_update=1 (ri_sta %u, %d commit(s))",
            (unsigned)g_bd[0].ri_sta, board0_commits(TST_VF_FREQ, 1));
    CHECK(stub_pll_commit_by_board[0] == 3,
            "...board 0 commits INIT, VF in RI, VF after RI and nothing more "
            "(got %d)",
            stub_pll_commit_by_board[0]);

    run_main_from_ri(0);
    CHECK(g_bd[0].ri_sta == 1 && board0_commits((uint32_t)INIT_FREQ, 0) == 1,
            "no VF during RI: board 0 commits INIT_FREQ once in RI (ri_sta %u, "
            "%d commit(s))",
            (unsigned)g_bd[0].ri_sta, board0_commits((uint32_t)INIT_FREQ, 0));
    CHECK(stub_pll_commit_by_board[0] == 1, "...and leaving RI adds no PLL write (got %d)",
            stub_pll_commit_by_board[0]);

    return tst_done();
}
