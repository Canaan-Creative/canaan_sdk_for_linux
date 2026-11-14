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
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "boardconf.h"
#include "hashmcu.h"
#include "board_sensors.h"
#include "osif.h"
#include "miner_ipc.h"
#include "mm_telem.c"
#include "../../tests/include/tst_check.h"

static time_t   tst_now = 1000; /* Simulated clock behind osif_now_ms(), s */
static unsigned tst_conn;       /* asic IPC connection sequence the next push arrives on */

static int tst_pw_rc;       /* Non-zero: board_power_read() fails with this code */
static int tst_pw_w = 143;  /* Output power board_power_read() reports, W */
static int tst_mode = 0;    /* Work mode mm_workmode_running() reports */
static int tst_mcu_boards;  /* Hashboard MCU count hashmcu_init() and hashmcu_count() report */
static int tst_mcu_rc;      /* Non-zero: hashmcu_read() fails with this code and no temperatures */
static int tst_outlet = 42; /* Outlet temperature hashmcu_read() reports, degC; inlet is 3 below */

int hashmcu_init(void)
{
    return tst_mcu_boards;
}
int hashmcu_count(void)
{
    return tst_mcu_boards;
}
int hashmcu_read(int board, hashmcu_temps *out)
{
    (void)board;
    if (tst_mcu_rc) {
        out->outlet_degc = out->inlet_degc = ASIC_TEMP_NONE;
        return tst_mcu_rc;
    }
    out->outlet_degc = tst_outlet;
    out->inlet_degc  = tst_outlet - 3;
    return 0;
}

int board_power_read(board_power *out)
{
    if (tst_pw_rc)
        return tst_pw_rc;
    memset(out, 0, sizeof *out);
    out->pout_w = (uint16_t)tst_pw_w;
    return 0;
}

uint8_t mm_workmode_running(void)
{
    return (uint8_t)tst_mode;
}

uint64_t osif_now_ms(void)
{
    return (uint64_t)tst_now * 1000u;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}

static void push_at(time_t when, int chips, int degc)
{
    tst_now = when;
    static asic_chip_telem ct;
    memset(&ct, 0, sizeof ct);
    ct.tm.in_work     = 1;
    ct.tm.ghs_spd_mhs = 6430000;
    ct.boards         = 1;
    ct.chips[0]       = (uint16_t)chips;
    ct.pvt_lapped[0]  = 1;
    for (int c = 0; c < chips && c < MINER_MAX_CHIPS; c++)
        ct.pvt_t[0][c] = (int16_t)degc;
    mm_telem_ingest(&ct, (int)sizeof ct, tst_conn);
}

static void poll_at(time_t when)
{
    tst_now = when;
    mm_telem_poll();
}

static void core_push_at(time_t when)
{
    tst_now          = when;
    mc_core_stats cs = { .ghs_avg_mhs = 1234, .job_gap_s = 125 };
    mm_telem_ingest_core(&cs, (int)sizeof cs);
}

static mm_telem_t get_at(time_t when)
{
    tst_now = when;
    mm_telem_t t;
    mm_telem_get(&t);
    return t;
}

int main(void)
{
    printf("mm_telem: per-source freshness, failed-source sentinels, "
           "receive-buffer copy (%d hashboard(s))\n",
            HBOARD_COUNT);

    mm_telem_t t = get_at(1000);
    CHECK(!t.poll_fresh, "cold snapshot: poll_fresh=0");
    CHECK(!t.core_live, "cold snapshot: core_live=0 (no MSG_CORE_STATS yet)");
    CHECK(t.pout_w == MM_TELEM_POUT_NONE, "cold pout_w is the sentinel, not 0 W");
    CHECK(t.workmode == MM_TELEM_WORKMODE_NONE, "cold workmode is the sentinel, not mode 0");

    tst_mcu_boards = HBOARD_COUNT;
    poll_at(2000);
    t = get_at(2000);
    CHECK(t.poll_fresh == 1, "after a poll: poll_fresh=1");
    CHECK(t.pout_w == 143, "poll publishes the rail power it read");
    CHECK(t.workmode == 0, "poll publishes the selected work mode");
    CHECK(t.hb_count == HBOARD_COUNT && t.hb_outlet[0] == 42,
            "poll publishes the hashboard MCU row");

    t = get_at(2000 + TELEM_STALE_MS / 1000);
    CHECK(t.poll_fresh == 1, "poll_fresh holds at exactly TELEM_STALE_MS");
    t = get_at(2000 + TELEM_STALE_MS / 1000 + 1);
    CHECK(!t.poll_fresh, "poll_fresh drops one second later");

    push_at(2030, 12, 70);
    t = get_at(2030);
    CHECK(t.poll_fresh == 0,
            "stale poll + live push: poll_fresh=0, so the thermal soft-off's outlet arm and "
            "mm_periph_push see the local reads as ABSENT");
    CHECK(t.hb_outlet[0] == 42 && t.hb_count == HBOARD_COUNT,
            "the stale row is still IN the snapshot — the flag is what withholds it");
    CHECK(t.asic_live == 1 && t.asic_chips == 12,
            "the push arm is unchanged by a stale poll: heartbeat and chip count live");
    CHECK(t.tmax == 70 && t.in_work == 1, "and its own fields are the pushed ones");

    poll_at(2100);
    t = get_at(2100);
    CHECK(t.poll_fresh == 1, "fresh poll + stale push: poll_fresh=1");
    CHECK(t.tmax == -273 && t.tavg == -273 && !t.asic_live && !t.asic_chips,
            "a stale push leaves the asic fields at their sentinels");

    for (int pollq = 0; pollq < 2; pollq++) {
        for (int pushq = 0; pushq < 2; pushq++) {
            g_at             = pollq ? 3000 * 1000u : 0;
            g_push_at        = pushq ? 3000 * 1000u : 0;
            g_push.asic_live = 1;
            t                = get_at(3005);
            CHECK(t.poll_fresh == pollq,
                    "the two sources are independent: poll_fresh == poll(%d), "
                    "whatever push(%d) did",
                    pollq, pushq);
            CHECK(t.asic_live == pushq,
                    "asic_live == push(%d) alone — no combined flag can make a "
                    "live local poll speak for the asic heartbeat",
                    pushq);
        }
    }
    for (int pollq = 0; pollq < 2; pollq++) {
        for (int pushq = 0; pushq < 2; pushq++) {
            g_at             = pollq ? 3000 * 1000u : 2000 * 1000u;
            g_push_at        = pushq ? 3000 * 1000u : 2000 * 1000u;
            g_push.asic_live = 1;
            t                = get_at(3005);
            CHECK(t.poll_fresh == pollq && t.asic_live == pushq,
                    "stale-not-absent: poll_fresh == poll(%d), asic_live == "
                    "push(%d), still independent",
                    pollq, pushq);
        }
    }
    g_at = g_push_at = 0;
    memset(&g_tel, 0, sizeof g_tel);
    memset(&g_push, 0, sizeof g_push);

    tst_pw_rc = -1;
    poll_at(4000);
    t = get_at(4000);
    CHECK(t.poll_fresh == 1, "a failed rail read does not fail the whole poll");
    CHECK(t.pout_w == MM_TELEM_POUT_NONE, "a failed rail read publishes the sentinel, not 0 W");
    tst_pw_rc = 0;

    tst_mcu_rc = -1;
    poll_at(4001);
    t = get_at(4001);
    CHECK(t.hb_outlet[0] == ASIC_TEMP_NONE && t.hb_inlet[0] == ASIC_TEMP_NONE,
            "a failed MCU read publishes the -273 sentinel (unchanged)");
    tst_mcu_rc = 0;

    {
        static uint8_t  raw[sizeof(asic_chip_telem) + 8];
        asic_chip_telem seed;
        memset(&seed, 0, sizeof seed);
        seed.tm.in_work    = 1;
        seed.boards        = 1;
        seed.chips[0]      = 7;
        seed.pvt_lapped[0] = 1;
        for (int c = 0; c < 7 && c < MINER_MAX_CHIPS; c++)
            seed.pvt_t[0][c] = 55;
        memcpy(raw + 1, &seed, sizeof seed);
        tst_now = 5000;
        mm_telem_ingest(raw + 1, (int)sizeof seed, 0);
        t = get_at(5000);
        CHECK(t.asic_chips == 7 && t.tmax == 55,
                "an unaligned receive buffer is copied before it is read");
    }

    tst_now = 5005;
    mm_telem_ingest(NULL, (int)sizeof(asic_chip_telem), 0);
    {
        static asic_chip_telem ct;
        memset(&ct, 0, sizeof ct);
        ct.boards   = 1;
        ct.chips[0] = 3;
        mm_telem_ingest(&ct, (int)sizeof ct - 1, 0);
    }
    {
        static uint8_t  big[sizeof(asic_chip_telem) + 8];
        asic_chip_telem ct;
        memset(&ct, 0, sizeof ct);
        ct.boards   = 1;
        ct.chips[0] = 3;
        memcpy(big, &ct, sizeof ct);
        mm_telem_ingest(big, (int)sizeof big, 0);
    }
    t = get_at(5005);
    CHECK(t.asic_chips == 7, "a NULL, short or long push is dropped, snapshot untouched");

    g_at = g_push_at = 0;
    memset(&g_tel, 0, sizeof g_tel);
    memset(&g_push, 0, sizeof g_push);

    core_push_at(6000);
    t = get_at(6000);
    CHECK(t.core_live == 1, "core_live goes true when MSG_CORE_STATS is seen");
    CHECK(t.job_gap_s == 125, "the pool job gap rides MSG_CORE_STATS (%u)", t.job_gap_s);
    CHECK(!t.poll_fresh && !t.asic_live, "core_live is not folded into poll_fresh/asic_live");

    t = get_at(6000 + TELEM_STALE_MS / 1000);
    CHECK(t.core_live == 1, "core_live holds at exactly TELEM_STALE_MS");
    t = get_at(6000 + TELEM_STALE_MS / 1000 + 1);
    CHECK(t.core_live == 0,
            "core_live drops one second after the last MSG_CORE_STATS, the same "
            "staleness rule asic_live uses for MSG_ASIC_TELEM");

    push_at(6000 + TELEM_STALE_MS / 1000 + 1, 12, 70);
    poll_at(6000 + TELEM_STALE_MS / 1000 + 1);
    t = get_at(6000 + TELEM_STALE_MS / 1000 + 1);
    CHECK(t.asic_live == 1 && t.core_live == 0,
            "a live asic push/poll does not make a stale core_miner read live");

    core_push_at(6000 + TELEM_STALE_MS / 1000 + 2);
    t = get_at(6000 + TELEM_STALE_MS / 1000 + 2);
    CHECK(t.core_live == 1, "core_live recovers once traffic resumes");

    g_at = g_push_at = 0;
    memset(&g_tel, 0, sizeof g_tel);
    memset(&g_push, 0, sizeof g_push);

    {
        static asic_chip_telem ct;
        memset(&ct, 0, sizeof ct);
        ct.tm.in_work  = 1;
        ct.boards      = 1;
        ct.chips[0]    = 4;
        ct.asic_health = ASIC_UNHEALTHY_MAIN_STALL;
        tst_now        = 7000;
        mm_telem_ingest(&ct, (int)sizeof ct, 0);
    }
    t = get_at(7000);
    CHECK(t.asic_live == 0, "fresh push with asic_health unhealthy: asic_live=0");

    {
        static asic_chip_telem ct;
        memset(&ct, 0, sizeof ct);
        ct.tm.in_work = 1;
        ct.boards     = 1;
        ct.chips[0]   = 4;
        tst_now       = 7001;
        mm_telem_ingest(&ct, (int)sizeof ct, 0);
    }
    t = get_at(7001);
    CHECK(t.asic_live == 1, "asic_live recovers once asic_health reports healthy again");

    {
        static asic_chip_telem ct;
        memset(&ct, 0, sizeof ct);
        ct.tm.in_work  = 1;
        ct.tm.diag     = 0xff;
        ct.boards      = 1;
        ct.chips[0]    = 4;
        ct.asic_health = ASIC_HEALTH_OK;
        tst_now        = 7002;
        mm_telem_ingest(&ct, (int)sizeof ct, 0);
    }
    t = get_at(7002);
    CHECK(t.asic_live == 1,
            "tm.diag set with asic_health OK leaves asic_live TRUE: diag is "
            "never a health bit. asic_live is an EQUALITY test against "
            "ASIC_HEALTH_OK, so folding diag into asic_health — or widening "
            "that test into a mask that swallows it — starves the watchdog "
            "feed gate and resets the board");

    {
        static asic_chip_telem ct;
        memset(&ct, 0, sizeof ct);
        ct.tm.in_work  = 1;
        ct.tm.diag     = 0xff;
        ct.boards      = 1;
        ct.chips[0]    = 4;
        ct.asic_health = ASIC_UNHEALTHY_MAIN_STALL;
        tst_now        = 7003;
        mm_telem_ingest(&ct, (int)sizeof ct, 0);
    }
    t = get_at(7003);
    CHECK(t.asic_live == 0,
            "...and a REAL asic_health bit still drops asic_live with the same "
            "diag bit set: the diag carve-out exempts diag, not the health test");

    g_at = g_push_at = 0;
    memset(&g_tel, 0, sizeof g_tel);
    memset(&g_push, 0, sizeof g_push);

    {
        mc_core_stats cs = { .ghs_avg_mhs = 1234, .core_health = CORE_UNHEALTHY_MAIN_STALL };
        tst_now          = 8000;
        mm_telem_ingest_core(&cs, (int)sizeof cs);
    }
    t = get_at(8000);
    CHECK(t.core_live == 0, "fresh MSG_CORE_STATS with core_health unhealthy: core_live=0");

    {
        mc_core_stats cs = { .ghs_avg_mhs = 1234 };
        tst_now          = 8001;
        mm_telem_ingest_core(&cs, (int)sizeof cs);
    }
    t = get_at(8001);
    CHECK(t.core_live == 1, "core_live recovers once core_health reports healthy again");

    poll_at(9000);
    core_push_at(9000);
    push_at(9000, 12, 70);
    t = get_at(9001);
    CHECK(t.asic_live && t.in_work && t.asic_chips == 12,
            "a fresh push from the running asic_miner reads live and in work");
    mm_telem_asic_reset(2);
    t = get_at(9001);
    CHECK(!t.asic_live && !t.in_work && t.asic_chips == 0 && t.tmax == ASIC_TEMP_NONE,
            "after the reset a spawn makes, the previous asic_miner's fresh push "
            "is withheld: live %d, in_work %d, %d chip(s), tmax %d",
            t.asic_live, t.in_work, t.asic_chips, t.tmax);
    CHECK(t.poll_fresh && t.pout_w == 143 && t.core_live,
            "...while the local poll and core_miner sources are untouched");
    tst_conn = 1;
    push_at(9002, 12, 71);
    t = get_at(9002);
    CHECK(!t.asic_live && !t.in_work && t.asic_chips == 0 && t.tmax == ASIC_TEMP_NONE,
            "a push still queued on the previous asic_miner's connection and "
            "read only after the spawn is dropped: live %d, in_work %d, %d "
            "chip(s), tmax %d",
            t.asic_live, t.in_work, t.asic_chips, t.tmax);
    tst_conn = 2;
    push_at(9003, 12, 71);
    t = get_at(9003);
    CHECK(t.asic_live && t.asic_chips == 12 && t.tmax != ASIC_TEMP_NONE,
            "the new asic_miner's first push is published as usual");

    mm_telem_asic_reset(UINT_MAX + 1u);
    tst_conn = UINT_MAX;
    push_at(9004, 12, 72);
    t = get_at(9004);
    CHECK(!t.asic_live && t.asic_chips == 0,
            "across the connection sequence wrap, the previous connection's "
            "push is still dropped");
    tst_conn = 0;
    push_at(9005, 12, 72);
    t = get_at(9005);
    CHECK(t.asic_live && t.asic_chips == 12,
            "...and the first connection after the wrap is published");

    printf(fails ? "FAILED (%d)\n" : "PASSED (%d failures)\n", fails);
    return fails ? 1 : 0;
}
