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
#include "driver_avalon.h"

#include "../../tests/include/tst_check.h"

#define ENUM_N        2                             /* Chips enumerated on each board */
#define STALL_PASSES  ((int)(3u * TELEM_PERIOD_MS)) /* Loop ticks board 0 keeps its TX FIFO busy */
#define WARMUP_CAP    20000 /* Loop ticks allowed for every board to start hashing */
#define RESUME_CAP    20000 /* Loop ticks allowed for every board to send work after the stall */
#define SETTLE_PASSES 400   /* Ticks with every board hashing before the stall starts */

/* Stub enumerate chip count; non-zero makes stub work generation succeed */
extern int     stub_enum_chips, stub_genwork_ok;
extern int     stub_work_n;        /* Stub avalon_send_work() calls */
extern uint8_t stub_work_board[];  /* Board of each logged stub work send */
extern int     stub_nonce_arm_n[]; /* Stub a3197s_enable_nonce_return() calls per board */
extern int (*stub_tx_empty_hook)(uint8_t ch); /* Replaces the stub uart_tx_empty() when set */

static uint64_t tst_now_ms;  /* Simulated clock returned by osif_now_ms(), ms */
static int      s_loop_live; /* Non-zero once the main loop has started */

static int s_telem_n;   /* MSG_ASIC_TELEM messages sent */
static int s_track_gap; /* Non-zero while the gap between telemetry sends is tracked */
/* Time of the last telemetry send and largest tracked gap, ms */
static uint64_t s_telem_last_ms, s_telem_max_gap;

/* Phase (0 warm-up, 1 stall, 2 resume); ticks in all, in the stall, after it */
static int      s_phase, s_passes, s_stall_passes, s_resume_passes;
static int      s_settle;               /* Ticks with every board hashing so far */
static int      s_busy;                 /* Non-zero makes board 0 report a busy TX FIFO */
static uint64_t s_stall_t0, s_stall_t1; /* Simulated time the stall started and ended, ms */
/* Work sends at stall start; telemetry sends at stall start and at its end */
static int s_work_at_stall, s_telem_at_stall, s_telem_at_stall_end;
/* Work sends per board at stall start and at its end */
static int s_work_by_board_at_stall[8], s_work_by_board_after_stall[8];
/* Sum over stall ticks of boards other than 0 given work since the stall began */
static int s_other_boards_serviced;

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

static void tst_loop_tick(void);
void        osif_sleep_ms(uint32_t ms)
{
    tst_now_ms += ms;
    if (s_loop_live)
        tst_loop_tick();
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
void osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)data;
    (void)len;
    if (type == MSG_ASIC_TELEM) {
        if (s_telem_n && s_track_gap) {
            uint64_t gap = tst_now_ms - s_telem_last_ms;
            if (gap > s_telem_max_gap)
                s_telem_max_gap = gap;
        }
        s_telem_last_ms = tst_now_ms;
        s_telem_n++;
    }
    return OSIF_OK;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main
_Static_assert(ENUM_N <= (int)MINER_MAX_CHIPS, "chain longer than the descriptor's");

static int tx_empty(uint8_t ch)
{
    return !(s_busy && ch == 0);
}

static void work_by_board(int *out)
{
    memset(out, 0, sizeof(int) * 8);
    for (int i = 0; i < stub_work_n && i < 2048; i++) {
        int b = stub_work_board[i];
        if (b >= 0 && b < 8)
            out[b]++;
    }
}

static void tst_loop_tick(void)
{
    s_passes++;
    if (s_phase == 0) {
        int hashing = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (stub_nonce_arm_n[b] > 0)
                hashing++;
        if (hashing == (int)HBOARD_COUNT && ++s_settle >= SETTLE_PASSES) {
            work_by_board(s_work_by_board_at_stall);
            s_work_at_stall  = stub_work_n;
            s_telem_at_stall = s_telem_n;
            s_stall_t0       = tst_now_ms;
            s_busy           = 1;
            s_track_gap      = 1;
            s_phase          = 1;
        } else if (s_passes >= WARMUP_CAP) {
            g_run = 0;
        }
        return;
    }
    if (s_phase == 1) {
        int cur[8];
        work_by_board(cur);
        for (int b = 1; b < HBOARD_COUNT; b++)
            if (cur[b] != s_work_by_board_at_stall[b])
                s_other_boards_serviced++;
        if (++s_stall_passes >= STALL_PASSES) {
            work_by_board(s_work_by_board_after_stall);
            s_stall_t1           = tst_now_ms;
            s_telem_at_stall_end = s_telem_n;
            s_busy               = 0;
            s_track_gap          = 0;
            s_phase              = 2;
        }
        return;
    }
    int cur[8], resumed = 0;
    work_by_board(cur);
    for (int b = 0; b < HBOARD_COUNT; b++)
        if (cur[b] > s_work_by_board_after_stall[b])
            resumed++;
    if (resumed == (int)HBOARD_COUNT || ++s_resume_passes >= RESUME_CAP)
        g_run = 0;
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chip(s)/board, telem period %u ms\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, (unsigned)TELEM_PERIOD_MS);

    stub_enum_chips    = ENUM_N;
    stub_genwork_ok    = 1;
    stub_tx_empty_hook = tx_empty;
    memset(stub_nonce_arm_n, 0, sizeof(int) * 8);
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].ri_sta = g_bd[bd].stage = 0;
    g_pll_at_target = 1;
    memset(&g_job, 0, sizeof g_job);
    g_job.seq     = 1;
    g_job.n2_base = 0x5000u;
    g_job_cnt++;
    s_loop_live = 0;
    g_run       = 1;
    (void)asic_miner_main_unused();
    s_loop_live        = 0;
    stub_tx_empty_hook = NULL;

    CHECK(s_phase == 2 && s_passes < WARMUP_CAP,
            "the run reached the stall and came out of it (phase %d, %d passes)", s_phase,
            s_passes);

    CHECK(s_work_by_board_after_stall[0] == s_work_by_board_at_stall[0],
            "board 0 dispatched no work while its TX queue was busy (%d -> %d)",
            s_work_by_board_at_stall[0], s_work_by_board_after_stall[0]);

    {
        _Static_assert(STALL_PASSES % HBOARD_COUNT == 0, "stall spans whole board rotations");
        uint64_t span = s_stall_t1 - s_stall_t0;
        uint64_t want = (uint64_t)s_stall_passes / HBOARD_COUNT *
                (1u + (HBOARD_COUNT - 1u) * SCAN_CADENCE_MS);
        CHECK(span == want,
                "%d stalled passes advanced the clock %llu ms (want %llu) — 1 ms per busy-board "
                "pass, the %u ms scan cadence per other-board pass",
                s_stall_passes, (unsigned long long)span, (unsigned long long)want,
                (unsigned)SCAN_CADENCE_MS);
    }

    if (HBOARD_COUNT > 1) {
        CHECK(s_other_boards_serviced > 0,
                "the busy board yields the scan thread: other boards were dispatched work during "
                "the stall (%d passes)",
                s_other_boards_serviced);
        for (int b = 1; b < HBOARD_COUNT; b++)
            CHECK(s_work_by_board_after_stall[b] > s_work_by_board_at_stall[b],
                    "...board %d included (%d -> %d)", b, s_work_by_board_at_stall[b],
                    s_work_by_board_after_stall[b]);
    } else {
        CHECK((int)HBOARD_COUNT == 1,
                "one board: 'the stall starves the others' has no content here, "
                "and the gate is a pure per-chip pacing device");
    }

    {
        int      during = s_telem_at_stall_end - s_telem_at_stall;
        uint64_t span   = s_stall_t1 - s_stall_t0;
        CHECK(during >= 2,
                "the heartbeat kept publishing across a %llu ms stall (%d "
                "MSG_ASIC_TELEM) — this is mm's watchdog feed gate, not a display "
                "field",
                (unsigned long long)span, during);
        CHECK(s_telem_max_gap <= (uint64_t)TELEM_PERIOD_MS + SCAN_CADENCE_MS,
                "...at no worse than the %u ms period + one %u ms pass (largest "
                "gap %llu ms)",
                (unsigned)TELEM_PERIOD_MS, (unsigned)SCAN_CADENCE_MS,
                (unsigned long long)s_telem_max_gap);
    }

    {
        int cur[8], resumed = 0;
        work_by_board(cur);
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (cur[b] > s_work_by_board_after_stall[b])
                resumed++;
        CHECK(s_resume_passes < RESUME_CAP && resumed == (int)HBOARD_COUNT,
                "every board dispatches again once the queue drains (%d of %d, "
                "%d passes)",
                resumed, (int)HBOARD_COUNT, s_resume_passes);
        CHECK(cur[0] > s_work_by_board_after_stall[0], "...the blocked board included (%d -> %d)",
                s_work_by_board_after_stall[0], cur[0]);
    }

    return tst_done();
}
