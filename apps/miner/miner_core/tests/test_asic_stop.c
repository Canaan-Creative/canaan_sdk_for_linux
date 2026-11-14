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

#define WORK_FREQ 297 /* Work frequency set on every board by arm_full_load(), MHz */

extern int stub_pll_freq_n, stub_pll_commit_n; /* Stub a3197s_set_freq() calls and PLL commits */
extern int stub_enum_n;                        /* Stub enumerate calls on all boards */
extern int stub_uart_init_n[];                 /* Stub uart_init() calls per board */
/* Chip count for the stub enumerate when set */
extern int (*stub_enum_hook)(uint8_t ch, int try_n);

static int tst_deinit_n; /* osif_ipc_deinit() calls */

static int      tst_open_n;   /* osif_ipc_init() calls */
static uint64_t tst_now_ms;   /* Simulated clock returned by osif_now_ms(), ms */
static uint64_t tst_slept_ms; /* Total time slept, ms */
/* Log lines: no valid chip count (ERR), no hashboard found (ERR), stopped (INFO) */
static int tst_op_nochips_n, tst_op_noboard_n, tst_op_stopped_n;

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
        *h = (osif_ipc_t *)(void *)(intptr_t)(0x100 + ++tst_open_n);
    return OSIF_OK;
}
void osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
    tst_deinit_n++;
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
    tst_now_ms += ms;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    if (!fmt)
        return;
    if (lvl == OSIF_LOG_ERR && strstr(fmt, "no valid chip count"))
        tst_op_nochips_n++;
    if (lvl == OSIF_LOG_ERR && strstr(fmt, "no hashboard found"))
        tst_op_noboard_n++;
    if (lvl == OSIF_LOG_INFO && !strcmp(fmt, "stopped"))
        tst_op_stopped_n++;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static int stop_in_enum(uint8_t ch, int try_n)
{
    (void)ch;
    (void)try_n;
    g_run = 0;
    return 0;
}

static int no_chips(uint8_t ch, int try_n)
{
    (void)ch;
    (void)try_n;
    return 0;
}

static void arm_full_load(void)
{
    for (int b = 0; b < HBOARD_COUNT; b++) {
        g_bd[b].chips_n   = MINER_MAX_CHIPS;
        g_bd[b].work_freq = (uint16_t)WORK_FREQ;
        g_bd[b].ri_sta    = 1;
        g_bd[b].stage     = BS_RUN;
    }
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chip(s)/board, %d -> %d MHz\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, (int)WORK_FREQ, (int)INIT_FREQ);

    arm_full_load();
    stub_pll_freq_n = stub_pll_commit_n = 0;
    tst_slept_ms                        = 0;
    for (int i = 0; i < ASIC_PLL_POINTS; i++)
        g_bd[0].pll_pts[i] = (uint16_t)INIT_FREQ;
    memset(g_bd[0].cmd_pts, 0, sizeof g_bd[0].cmd_pts);
    set_asic_freq(0);

    CHECK(stub_pll_freq_n >= 1, "a freq lay-down is observable (got %d)", stub_pll_freq_n);
    CHECK(stub_pll_commit_n == 1,
            "a freq move is ONE broadcast commit, not one per chip (got %d for "
            "%d chip(s))",
            stub_pll_commit_n, (int)MINER_MAX_CHIPS);
    CHECK(tst_slept_ms == 0,
            "...and costs no in-rush sleep at all: there is no per-chip walk left "
            "to pace (got %llu ms)",
            (unsigned long long)tst_slept_ms);

    arm_full_load();
    stub_pll_freq_n = stub_pll_commit_n = 0;
    tst_deinit_n                        = 0;
    tst_slept_ms                        = 0;
    asic_deinit();

    CHECK(stub_pll_commit_n == 0,
            "teardown from full load commits NO set_pll to the chain (got %d; one "
            "walk of every board would be %d, and the deleted ramp made five of "
            "them) — a rail cut here is the safe-PLL-sequence brick window",
            stub_pll_commit_n, (int)MINER_MAX_CHIPS * (int)HBOARD_COUNT);
    CHECK(stub_pll_freq_n == 0, "...and lays down no PLL freq either (got %d)", stub_pll_freq_n);
    CHECK(tst_slept_ms == 0,
            "...so the teardown does not sleep at all (got %llu ms) — mm's join "
            "bound has to cover this",
            (unsigned long long)tst_slept_ms);

    CHECK(tst_deinit_n == 2,
            "teardown closes BOTH of asic_miner's channels exactly once each — "
            "the datapath to core_miner and the asic-ctrl to mm_miner (got %d)",
            tst_deinit_n);

    for (int b = 0; b < HBOARD_COUNT; b++)
        CHECK(g_bd[b].work_freq == (uint16_t)WORK_FREQ,
                "board %d is left at its operating freq %u MHz, not parked "
                "(got %u)",
                b, (unsigned)WORK_FREQ, (unsigned)g_bd[b].work_freq);

    for (int b = 0; b < HBOARD_COUNT; b++) {
        g_bd[b].chips_n   = 0;
        g_bd[b].work_freq = (uint16_t)INIT_FREQ;
    }
    stub_pll_freq_n = stub_pll_commit_n = 0;
    tst_deinit_n                        = 0;
    asic_deinit();

    CHECK(stub_pll_commit_n == 0 && stub_pll_freq_n == 0,
            "teardown after a failed enumeration writes no PLL either "
            "(freq %d, commit %d)",
            stub_pll_freq_n, stub_pll_commit_n);
    CHECK(tst_deinit_n == 2, "...and still closes both channels (got %d)", tst_deinit_n);

    stub_enum_hook  = stop_in_enum;
    stub_enum_n     = 0;
    stub_pll_freq_n = stub_pll_commit_n = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        stub_uart_init_n[b] = 0;
    tst_deinit_n     = 0;
    tst_op_nochips_n = tst_op_noboard_n = tst_op_stopped_n = 0;
    g_run                                                  = 1;
    {
        int rc    = asic_miner_main_unused();
        int later = 0;
        for (int b = 1; b < HBOARD_COUNT; b++)
            later += stub_uart_init_n[b];

        CHECK(rc == 0,
                "SIGTERM during enumeration -> main() returns 0, not the "
                "no-hashboard exit (got %d)",
                rc);
        CHECK(stub_enum_n == 1 && later == 0,
                "...bring-up stops at the board being enumerated: %d enumerate "
                "call(s), %d later board(s) brought up",
                stub_enum_n, later);
        CHECK(tst_op_nochips_n == 0 && tst_op_noboard_n == 0,
                "...and logs no board failure (%d 'no valid chip count', %d "
                "'no hashboard found')",
                tst_op_nochips_n, tst_op_noboard_n);
        CHECK(tst_deinit_n == 2 && tst_op_stopped_n == 1,
                "...it exits through asic_deinit: both channels closed (%d) and "
                "'stopped' logged (%d)",
                tst_deinit_n, tst_op_stopped_n);
        CHECK(stub_pll_commit_n == 0, "...without any PLL write (got %d)", stub_pll_commit_n);
    }
    stub_enum_hook   = no_chips;
    stub_enum_n      = 0;
    tst_deinit_n     = 0;
    tst_op_nochips_n = tst_op_noboard_n = tst_op_stopped_n = 0;
    g_run                                                  = 1;
    {
        int rc = asic_miner_main_unused();

        CHECK(rc == 1, "no board enumerates -> main() returns 1 (got %d)", rc);
        CHECK(tst_op_nochips_n == HBOARD_COUNT && tst_op_noboard_n == 1,
                "...after one 'no valid chip count' per board (%d of %d) and one "
                "'no hashboard found' (%d)",
                tst_op_nochips_n, (int)HBOARD_COUNT, tst_op_noboard_n);
        CHECK(tst_deinit_n == 2 && tst_op_stopped_n == 1,
                "...it exits through asic_deinit: both channels closed (%d) and "
                "'stopped' logged (%d)",
                tst_deinit_n, tst_op_stopped_n);
    }
    stub_enum_hook = NULL;

    return tst_done();
}
