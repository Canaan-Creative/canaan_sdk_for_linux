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

#define ENUM_N   3
#define WINDOW   200
#define PASS_CAP 20000
#define POOL_SEQ 5u
#define POOL_N2  0x7000u

extern int      stub_enum_chips, stub_genwork_ok;
extern int      stub_work_n, stub_genwork_n;
extern uint8_t  stub_work_board[];
extern uint16_t stub_work_chip[];
extern uint32_t stub_genwork_seq[], stub_genwork_n2[];

static uint64_t tst_now_ms;
static int      s_loop_live;

static int s_phase, s_passes, s_window;
static int s_static_end;
static int s_work_static, s_work_pool;
static int s_pool_end;

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
    (void)type;
    (void)data;
    (void)len;
    return OSIF_OK;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main
_Static_assert(ENUM_N <= (int)MINER_MAX_CHIPS, "chain longer than the descriptor's");

static void tst_loop_tick(void)
{
    if (++s_passes >= PASS_CAP) {
        g_run = 0;
        return;
    }
    if (s_phase == 0) {
        int running = 0;
        for (int b = 0; b < (int)HBOARD_COUNT; b++)
            if (g_bd[b].stage == BS_RUN)
                running++;
        if (running != (int)HBOARD_COUNT)
            return;
        s_work_static = stub_work_n;
        s_phase       = 1;
        return;
    }
    if (s_phase == 1) {
        if (++s_window < WINDOW)
            return;
        s_work_static = stub_work_n - s_work_static;
        s_static_end  = stub_work_n;
        mc_asic_job j;
        memset(&j, 0, sizeof j);
        j.seq      = POOL_SEQ;
        j.n2_base  = POOL_N2;
        j.target_h = 0x0000ffffu;
        ipc_cb(MSG_ASIC_JOB, &j, (int)sizeof j);
        s_work_pool = stub_work_n;
        s_window    = 0;
        s_phase     = 2;
        return;
    }
    if (++s_window < WINDOW)
        return;
    s_work_pool = stub_work_n - s_work_pool;
    s_pool_end  = stub_work_n;
    g_run       = 0;
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chip(s)/board\n", BOARDCONF_SECTION, (int)HBOARD_COUNT,
            ENUM_N);

    stub_enum_chips = ENUM_N;
    stub_genwork_ok = 1;
    g_pll_at_target = 1;
    g_run           = 1;
    (void)asic_miner_main_unused();
    s_loop_live = 0;

    CHECK(s_phase == 2 && s_pool_end > 0 && s_pool_end <= 2048,
            "the run hashed without a pool, took a pool job and kept hashing "
            "(phase %d, %d passes, %d sends)",
            s_phase, s_passes, stub_work_n);
    CHECK(stub_genwork_n == stub_work_n, "every built work was sent (%d built, %d sent)",
            stub_genwork_n, stub_work_n);

    int      bcast = 0, chip_static = 0, bad_seq = 0, bad_n2 = 0;
    uint32_t want_n2 = 0;
    int      seen[8][ENUM_N];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_static_end; i++) {
        if (stub_genwork_seq[i] != STATIC_WORK_JOB_ID)
            bad_seq++;
        if (stub_work_chip[i] == (uint16_t)A3197S_CHIP_BROADCAST) {
            bcast++;
            continue;
        }
        if (stub_genwork_n2[i] != want_n2)
            bad_n2++;
        want_n2++;
        chip_static++;
        if (stub_work_board[i] < 8 && stub_work_chip[i] < ENUM_N)
            seen[stub_work_board[i]][stub_work_chip[i]]++;
    }
    CHECK(bad_seq == 0,
            "with no pool job every work is built from the static job 0x%08x "
            "(%d were not)",
            (unsigned)STATIC_WORK_JOB_ID, bad_seq);
    CHECK(bcast == (int)HBOARD_COUNT,
            "...each board took one broadcast default work at bring-up (%d)", bcast);
    CHECK(chip_static >= 2 * (int)HBOARD_COUNT * ENUM_N,
            "...and then per-chip static work kept flowing (%d sends over %d "
            "passes)",
            chip_static, WINDOW);
    CHECK(bad_n2 == 0, "...each with the next extranonce2, 0 upward (%d out of order)", bad_n2);
    {
        int missed = 0;
        for (int b = 0; b < (int)HBOARD_COUNT; b++)
            for (int c = 0; c < ENUM_N; c++)
                if (seen[b][c] < 2)
                    missed++;
        CHECK(missed == 0,
                "...rotating over every chip of every board (%d chip(s) got fewer "
                "than 2)",
                missed);
    }

    int pool_bad_seq = 0, pool_bad_n2 = 0;
    want_n2 = POOL_N2;
    for (int i = s_static_end; i < s_pool_end; i++) {
        if (stub_genwork_seq[i] != POOL_SEQ)
            pool_bad_seq++;
        if (stub_genwork_n2[i] != want_n2)
            pool_bad_n2++;
        want_n2++;
    }
    CHECK(s_pool_end > s_static_end && pool_bad_seq == 0,
            "from the first send after the pool job every work is built from it "
            "(%d of %d were not)",
            pool_bad_seq, s_pool_end - s_static_end);
    CHECK(pool_bad_n2 == 0,
            "...with its extranonce2 rebased to 0x%x and counting up (%d out of order)",
            (unsigned)POOL_N2, pool_bad_n2);
    CHECK(g_last_job_nonce2 == (uint32_t)chip_static - 1u,
            "...and last_job_nonce2 holds the last static extranonce2 (%u, want %d)",
            g_last_job_nonce2, chip_static - 1);
    CHECK(s_work_pool >= s_work_static - (int)HBOARD_COUNT &&
                    s_work_pool <= s_work_static + (int)HBOARD_COUNT,
            "static and pool work go out at the same pace (%d vs %d sends per %d "
            "passes)",
            s_work_static, s_work_pool, WINDOW);

    return tst_done();
}
