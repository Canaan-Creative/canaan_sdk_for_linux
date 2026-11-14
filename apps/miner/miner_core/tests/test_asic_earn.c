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

#define ENUM_N     4                           /* Chips enumerated on each present board */
#define CADENCE_MS ((unsigned)SCAN_CADENCE_MS) /* Main-loop nonce scan sleep, ms */

#define RD_MS 40u /* Simulated duration of one register read, ms */

#define VIS_MAX 4096 /* Entries kept in the status-read visit log */

#define SETTLE_PASSES 400 /* Loop ticks with every present board hashing before the window opens */
#define WINDOW_MS     40000u /* Length of the measurement window, simulated ms */
#define PASS_CAP      200000 /* Loop ticks after which the run stops */

/* Stub enumerate chip count; non-zero makes stub work generation succeed */
extern int stub_enum_chips, stub_genwork_ok;
extern int stub_nonce_arm_n[]; /* Stub a3197s_enable_nonce_return() calls per board */
/* Replaces the stub a3197s_read_reg() when set */
extern int (*stub_read_reg_hook)(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n);

static uint64_t tst_now_ms; /* Simulated clock returned by osif_now_ms(), ms */
/* Non-zero once the main loop has started; nonce scan sleeps taken */
static int s_loop_live, s_iters;
static int s_vis_n; /* Visits logged in the measurement window */
/* Board, read type and chip of each logged visit */
static uint8_t s_vis_board[VIS_MAX], s_vis_type[VIS_MAX], s_vis_chip[VIS_MAX];
/* Last read type and chip per board; s_vis_seen is non-zero once a board has been read */
static uint8_t  s_vis_last_type[8], s_vis_last_chip[8], s_vis_seen[8];
static uint64_t s_walk_ms;   /* Simulated time spent in register reads, ms */
static int      s_win_open;  /* Non-zero while the measurement window is open */
static int      s_win_vis;   /* Visits in the window, including those not logged */
static int      s_win_steps; /* Read-cursor steps summed over boards across the window */
/* Last sampled read-cursor position per board; non-zero once sampled */
static int      s_cur_pos[8], s_cur_seen[8];
static uint64_t s_win_walk_ms; /* Simulated register-read time inside the window, ms */
static int      s_win_iters0;  /* s_iters when the window opened */

/* Loop ticks; settled ticks; phase: 0 settling, 1 window open */
static int      s_passes, s_settle, s_phase;
static uint64_t s_win_t0, s_win_t1; /* Simulated time the window opened and closed, ms */
static int      s_absent;           /* Boards at the top of the chain that enumerate no chips */

extern int stub_pvt_select_n[]; /* Stub a3197s_pvt_select_mode() calls per board */
static int s_win_sel0;          /* PVT selects summed over boards when the window opened */
/* Chip count for the stub enumerate when set */
extern int (*stub_enum_hook)(uint8_t ch, int try_n);

uint64_t osif_now_ms(void)
{
    return tst_now_ms;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}
int fan_dev_get_rpm(int fan)
{
    (void)fan;
    return 0;
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
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)type;
    (void)data;
    (void)len;
    return OSIF_OK;
}
void osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
}
_Static_assert(ENUM_N <= (int)MINER_MAX_CHIPS, "chain longer than the descriptor's");
_Static_assert(CADENCE_MS > 1u,
        "the cadence sleep must differ from the 1 ms "
        "tx-gate sleep, or counting it does not count iterations");

static void tst_loop_tick(void);
static void tst_sample_cursor(void);
void        osif_sleep_ms(uint32_t ms)
{
    tst_now_ms += ms;
    if (s_loop_live) {
        if (ms == CADENCE_MS) {
            s_iters++;
            tst_sample_cursor();
        }
        tst_loop_tick();
    }
}

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    s_loop_live = 1;
    return OSIF_OK;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static int read_reg_cost(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    (void)addr;
    if (out)
        memset(out, 0, (size_t)n * sizeof *out);
    int     b  = ch < HBOARD_COUNT ? ch : 0;
    uint8_t ty = g_rs.type, cp = g_rs.chip;
    if (!s_vis_seen[b] || s_vis_last_type[b] != ty || s_vis_last_chip[b] != cp) {
        s_vis_seen[b]      = 1;
        s_vis_last_type[b] = ty;
        s_vis_last_chip[b] = cp;
        if (s_win_open) {
            if (s_vis_n < VIS_MAX) {
                s_vis_board[s_vis_n] = (uint8_t)b;
                s_vis_type[s_vis_n]  = ty;
                s_vis_chip[s_vis_n]  = cp;
                s_vis_n++;
            }
            s_win_vis++;
        }
    }
    tst_now_ms += RD_MS;
    s_walk_ms += RD_MS;
    if (s_win_open)
        s_win_walk_ms += RD_MS;
    return 0;
}

static void tst_sample_cursor(void)
{
    int total = RS_TYPES * ENUM_N;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        int pos = (int)g_rs.type * ENUM_N + (int)g_rs.chip;
        if (s_cur_seen[b] && s_win_open) {
            int d = (pos - s_cur_pos[b] + total) % total;
            s_win_steps += d;
        }
        s_cur_pos[b]  = pos;
        s_cur_seen[b] = 1;
    }
}
static int tst_pvt_selects(void)
{
    int n = 0;
    for (int b = 0; b < (int)HBOARD_COUNT; b++)
        n += stub_pvt_select_n[b];
    return n;
}

static void tst_loop_tick(void)
{
    if (++s_passes >= PASS_CAP) {
        g_run = 0;
        return;
    }
    if (s_phase == 0) {
        int hashing = 0, want = (int)HBOARD_COUNT - s_absent;
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (stub_nonce_arm_n[b] > 0)
                hashing++;
        if (hashing == want && ++s_settle >= SETTLE_PASSES) {
            s_win_t0     = tst_now_ms;
            s_win_iters0 = s_iters;
            s_win_sel0   = tst_pvt_selects();
            s_win_open   = 1;
            s_phase      = 1;
        }
        return;
    }
    if (tst_now_ms - s_win_t0 >= WINDOW_MS) {
        s_win_t1   = tst_now_ms;
        s_win_open = 0;
        g_run      = 0;
    }
}

static int enum_dead_high(uint8_t ch, int try_n)
{
    (void)try_n;
    return (int)ch >= (int)HBOARD_COUNT - s_absent ? 0 : ENUM_N;
}

static void run_loop(int absent)
{
    s_absent           = absent;
    stub_enum_chips    = ENUM_N;
    stub_enum_hook     = absent ? enum_dead_high : NULL;
    stub_genwork_ok    = 1;
    stub_read_reg_hook = read_reg_cost;
    memset(stub_nonce_arm_n, 0, sizeof(int) * 8);
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].ri_sta = g_bd[bd].stage = 0;
    g_pll_at_target = 1;
    memset(&g_rs, 0, sizeof g_rs);
    memset(s_vis_seen, 0, sizeof s_vis_seen);
    memset(&g_job, 0, sizeof g_job);
    g_job.seq     = 1;
    g_job.n2_base = 0x6000u;
    g_job_cnt++;
    s_passes = s_settle = s_phase = 0;
    s_iters = s_win_iters0 = s_win_vis = s_vis_n = s_win_steps = 0;
    memset(s_cur_seen, 0, sizeof s_cur_seen);
    s_walk_ms = s_win_walk_ms = 0;
    s_win_open                = 0;
    s_loop_live               = 0;
    g_run                     = 1;
    (void)asic_miner_main_unused();
    s_loop_live        = 0;
    stub_read_reg_hook = NULL;
    stub_enum_hook     = NULL;
}

static void next_visible(int *ty, int *cp, int chips)
{
    do {
        if (++(*cp) >= chips) {
            *cp = 0;
            *ty = (*ty + 1) % RS_TYPES;
        }
    } while (*ty > RS_SPDLOG);
}

static int roundrobin_gaps(int chips)
{
    int prev_ty[8], prev_cp[8], seen[8] = { 0 }, gaps = 0;
    for (int i = 0; i < s_vis_n; i++) {
        int b = s_vis_board[i], ty = s_vis_type[i], cp = s_vis_chip[i];
        if (b < 0 || b >= 8) {
            gaps++;
            continue;
        }
        if (seen[b]) {
            int wty = prev_ty[b], wcp = prev_cp[b];
            next_visible(&wty, &wcp, chips);
            if (ty != wty || cp != wcp)
                gaps++;
        }
        seen[b]    = 1;
        prev_ty[b] = ty;
        prev_cp[b] = cp;
    }
    return gaps;
}

static unsigned duty_pct(void)
{
    uint64_t span = s_win_t1 - s_win_t0;
    return span ? (unsigned)(s_win_walk_ms * 100u / span) : 0u;
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chip(s)/board, cadence %u ms\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, CADENCE_MS);

    run_loop(0);

    CHECK(s_passes < PASS_CAP && s_win_t1 > s_win_t0,
            "the loop ran a %u ms measurement window (%llu ms, %d passes)", WINDOW_MS,
            (unsigned long long)(s_win_t1 - s_win_t0), s_passes);

    int iters = s_iters - s_win_iters0;
    printf("  window %llu ms: %d iteration(s), %d cursor step(s), %d read "
           "visit(s), %llu ms of UART = %u%%\n",
            (unsigned long long)(s_win_t1 - s_win_t0), iters, s_win_steps, s_win_vis,
            (unsigned long long)s_win_walk_ms, duty_pct());

    CHECK(s_win_steps > 0 && s_win_vis > 0 && iters > 0,
            "the telemetry walk stepped %d time(s) (%d of them issuing register "
            "reads) over %d iteration(s)",
            s_win_steps, s_win_vis, iters);

    CHECK(roundrobin_gaps(ENUM_N) == 0,
            "every board's (type, chip) walk is the round-robin continued without "
            "a gap over %d visit(s) (%d out of place)",
            s_vis_n, roundrobin_gaps(ENUM_N));
    {
        int types = 0, mask = 0;
        for (int i = 0; i < s_vis_n; i++)
            mask |= 1 << s_vis_type[i];
        for (int t = 0; t < RS_TYPES; t++)
            if (mask & (1 << t))
                types++;
        CHECK(types >= 2,
                "...and the window covered %d of the %d register types, so the "
                "chain-wrap arm of that rule was exercised",
                types, (int)RS_TYPES);
    }

    if (HBOARD_COUNT > 1) {
        run_loop(1);
        unsigned duty_part = duty_pct();
        int      iters2    = s_iters - s_win_iters0;
        printf("  with 1 board absent: %d iteration(s), %d visit(s), %llu ms "
               "= %u%%\n",
                iters2, s_win_vis, (unsigned long long)s_win_walk_ms, duty_part);
        CHECK(s_win_vis > 0, "a machine with one absent board still walks (%d visit(s))",
                s_win_vis);
        CHECK(roundrobin_gaps(ENUM_N) == 0,
                "...with the round-robin still gapless (%d out of place)", roundrobin_gaps(ENUM_N));
        {
            int on_absent = 0, dead = (int)HBOARD_COUNT - 1;
            for (int i = 0; i < s_vis_n; i++)
                if (s_vis_board[i] == dead)
                    on_absent++;
            CHECK(on_absent == 0,
                    "...and the absent board is never walked (%d visit(s) on a "
                    "chain that never enumerated)",
                    on_absent);
        }
    }

    return tst_done();
}
