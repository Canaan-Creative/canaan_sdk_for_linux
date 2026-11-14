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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osif.h"
#include "boardconf.h"
#include "miner_ipc.h"

#include "../../tests/include/tst_check.h"

extern int stub_pll_freq_n, stub_pll_commit_n; /* Stub a3197s_set_freq() calls and PLL commits */
/* correct_freq() of the base point at the last set_freq, the first commit and the last commit */
extern uint32_t stub_pll_last_base, stub_pll_commit_base_first, stub_pll_commit_base_last;
extern int      stub_pll_commit_by_board[]; /* Stub PLL commits per board */

static uint64_t tst_now_ms; /* Simulated clock returned by osif_now_ms(), ms */

static uint64_t tst_slept_ms; /* Total time slept, ms */

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
    (void)type;
    (void)data;
    (void)len;
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
    return tst_now_ms;
}
void osif_sleep_ms(uint32_t ms)
{
    tst_slept_ms += ms;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static void reset_counters(void)
{
    stub_pll_freq_n = stub_pll_commit_n = 0;
    stub_pll_last_base = stub_pll_commit_base_first = stub_pll_commit_base_last = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        stub_pll_commit_by_board[b] = 0;
    tst_slept_ms = 0;
}

static void arm(int up)
{
    tst_now_ms = 1000;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        g_bd[b].chips_n   = b < up ? (int)MINER_MAX_CHIPS : 0;
        g_bd[b].work_freq = (uint16_t)INIT_FREQ;
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            g_bd[b].pll_pts[i] = (uint16_t)INIT_FREQ;
        memset(g_bd[b].cmd_pts, 0, sizeof g_bd[b].cmd_pts);
        g_bd[b].ri_sta = 0;
        g_bd[b].stage  = BS_RI;
    }
    reset_counters();
}

int main(void)
{
    uint16_t vf = (uint16_t)((unsigned)INIT_FREQ + 30u);

    printf("descriptor %s: %d board(s), %d chip(s)/board, INIT %d MHz\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, (int)INIT_FREQ);

    arm(HBOARD_COUNT);
    set_asic_freq(0);

    CHECK(g_bd[0].work_freq == (uint16_t)INIT_FREQ &&
                    !memcmp(g_bd[0].cmd_pts, g_bd[0].pll_pts, sizeof g_bd[0].cmd_pts),
            "the first pass programs the chain at INIT_FREQ %u MHz (shadow %u, "
            "commanded points recorded)",
            (unsigned)INIT_FREQ, (unsigned)g_bd[0].work_freq);
    CHECK(stub_pll_last_base == (uint32_t)INIT_FREQ &&
                    stub_pll_commit_base_first == (uint32_t)INIT_FREQ,
            "...laid down and committed at INIT_FREQ (lay-down %u, first commit %u)",
            stub_pll_last_base, stub_pll_commit_base_first);
    CHECK(stub_pll_commit_by_board[0] == 1,
            "...as ONE broadcast commit, not one per chip — the "
            "TOAST_CHIP_BROADCAST arm (got %d for %d chip(s))",
            stub_pll_commit_by_board[0], (int)MINER_MAX_CHIPS);

    {
        int commits = stub_pll_commit_n;
        for (int i = 0; i < 200; i++) {
            tst_now_ms += 500;
            set_asic_freq(0);
        }
        CHECK(stub_pll_commit_n == commits && g_bd[0].work_freq == (uint16_t)INIT_FREQ,
                "NO RAMP OF ITS OWN: 100 s and 200 passes later the chain is still "
                "at INIT_FREQ %u MHz and nothing more was written — mm_miner owns "
                "the ramp and has commanded nothing (%d commits, shadow %u)",
                (unsigned)INIT_FREQ, stub_pll_commit_n, (unsigned)g_bd[0].work_freq);
    }

    {
        reset_counters();
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            g_bd[0].pll_pts[i] = vf;
        CHECK(set_asic_freq(0), "a commanded frequency makes the set_pll item write the chain");
        CHECK(g_bd[0].work_freq == vf && stub_pll_last_base == (uint32_t)vf,
                "...and it is programmed IN ONE PASS, whole, not stepped: %u -> %u "
                "MHz (shadow %u, lay-down %u)",
                (unsigned)INIT_FREQ, (unsigned)vf, (unsigned)g_bd[0].work_freq, stub_pll_last_base);
        CHECK(stub_pll_commit_base_last == (uint32_t)vf,
                "...with the commit following that lay-down (commit saw %u)",
                stub_pll_commit_base_last);
        CHECK(stub_pll_freq_n == 1 && stub_pll_commit_by_board[0] == 1,
                "...one lay-down and ONE broadcast commit, whatever the chain "
                "length (%d lay-down(s), %d commit(s))",
                stub_pll_freq_n, stub_pll_commit_by_board[0]);
        CHECK(!set_asic_freq(0), "...after which the item stops asking for the chain");
    }

    {
        int commits = stub_pll_commit_n;
        for (int i = 0; i < 50; i++) {
            tst_now_ms += 5000;
            set_asic_freq(0);
        }
        CHECK(stub_pll_commit_n == commits,
                "AT THE COMMANDED FREQ THE ITEM IS STILL JUDGED BUT SAYS NO: 50 "
                "more passes cost the chain not one write (%d commits, want %d)",
                stub_pll_commit_n, commits);
    }

    {
        arm(HBOARD_COUNT);
        set_asic_freq(0);
        reset_counters();
        uint16_t lo = (uint16_t)INIT_FREQ > 10 ? (uint16_t)((unsigned)INIT_FREQ - 10u) :
                                                 (uint16_t)INIT_FREQ;
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            g_bd[0].pll_pts[i] = lo;
        if (lo != (uint16_t)INIT_FREQ) {
            set_asic_freq(0);
            CHECK(g_bd[0].work_freq == lo,
                    "a commanded freq BELOW the current one is programmed the same "
                    "way, in one pass (shadow %u)",
                    (unsigned)g_bd[0].work_freq);
        }
    }

    if (HBOARD_COUNT > 1) {
        arm(HBOARD_COUNT);
        set_asic_freq(0);

        int others = 0;
        for (int b = 1; b < HBOARD_COUNT; b++)
            others += stub_pll_commit_by_board[b] +
                    !memcmp(g_bd[b].cmd_pts, g_bd[b].pll_pts, sizeof g_bd[b].cmd_pts);
        CHECK(stub_pll_commit_by_board[0] > 0 && others == 0,
                "PER-BOARD STATE: programming board 0 touches no other board (%d "
                "commits on board 0, %d elsewhere)",
                stub_pll_commit_by_board[0], others);

        set_asic_freq(1);
        CHECK(stub_pll_commit_by_board[1] > 0 &&
                        !memcmp(g_bd[1].cmd_pts, g_bd[1].pll_pts, sizeof g_bd[1].cmd_pts),
                "...and board 1 is programmed on its own pass (%d commits)",
                stub_pll_commit_by_board[1]);
    }

    {
        extern uint32_t stub_correct_snap;
        arm(HBOARD_COUNT);
        set_asic_freq(0);
        reset_counters();
        stub_correct_snap = 1;
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            g_bd[0].pll_pts[i] = vf;
        set_asic_freq(0);
        stub_correct_snap = 0;

        CHECK(stub_pll_last_base == (uint32_t)(vf - 1),
                "the COMMANDED value is snapped to one the cpm table supports "
                "before it reaches the chain: %u commanded, %u laid down",
                (unsigned)vf, stub_pll_last_base);
        CHECK(g_bd[0].work_freq == (uint16_t)(vf - 1),
                "...and the shadow carries the SNAPPED value, not the commanded "
                "one, so telemetry reports what the chips were actually given "
                "(shadow %u)",
                (unsigned)g_bd[0].work_freq);
        CHECK(!set_asic_freq(0),
                "...and the item then STOPS asking, even though the shadow no "
                "longer equals the commanded value — comparing the target against "
                "the snapped shadow would re-run the safe PLL sequence on every pass "
                "forever (target %u, shadow %u)",
                (unsigned)g_bd[0].pll_pts[0], (unsigned)g_bd[0].work_freq);
    }

    {
        arm(0);
        set_asic_freq(0);
        CHECK(stub_pll_commit_by_board[0] == 1,
                "a board whose chain length is not known yet is programmed the "
                "same way — one broadcast commit (got %d)",
                stub_pll_commit_by_board[0]);
    }

    {
        arm(HBOARD_COUNT);
        set_asic_freq(0);
        reset_counters();
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            g_pll_freq[i] = (uint16_t)INIT_FREQ;
        g_pll_freq[ASIC_PLL_POINTS - 1] = (uint16_t)(INIT_FREQ + 25u);
        g_bd[0].pll_pending             = 1;
        CHECK(set_asic_freq(0) && stub_pll_commit_by_board[0] == 1,
                "a command that moves only PLL point %d (point 0 unchanged at %u) "
                "reprograms the chain (%d commits)",
                ASIC_PLL_POINTS - 1, (unsigned)INIT_FREQ, stub_pll_commit_by_board[0]);
        CHECK(!set_asic_freq(0) && stub_pll_commit_by_board[0] == 1,
                "...once, then the item stops asking (%d commits)", stub_pll_commit_by_board[0]);
        g_bd[0].pll_pending = 1;
        CHECK(!set_asic_freq(0) && stub_pll_commit_by_board[0] == 1,
                "the same four points commanded again cost the chain no write "
                "(%d commits)",
                stub_pll_commit_by_board[0]);
    }

    return tst_done();
}
