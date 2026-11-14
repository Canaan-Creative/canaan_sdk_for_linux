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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osif.h"
#include "miner_ipc.h"
#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "tst_stats_json.h"

#include "../../tests/include/tst_check.h"

#define TEMP_NONE (-273) /* Chip temperature that marks no reading, degrees C */

#define NCHIP 4 /* Chips per board in the published telemetry */

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    return OSIF_OK;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)type;
    (void)data;
    (void)len;
    return OSIF_OK;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

#define main core_miner_main_unused
#include "core_miner_tu.h"
#undef main

static void chips_blank(asic_chip_telem *ct)
{
    memset(ct, 0, sizeof *ct);
    ct->boards = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        ct->chips[b]      = NCHIP;
        ct->pvt_lapped[b] = 1;
        for (int c = 0; c < NCHIP; c++)
            ct->pvt_t[b][c] = TEMP_NONE;
    }
}

static void publish(asic_chip_telem *ct, mc_api_stats *s)
{
    nonce_cb(MSG_ASIC_TELEM, ct, (int)sizeof *ct);
    api_snapshot(s, NULL);
}

int main(void)
{
    printf("descriptor %s: %d board(s)\n", BOARDCONF_SECTION, (int)HBOARD_COUNT);

    mc_api_stats    s;
    asic_chip_telem ct;

    api_snapshot(&s, NULL);
    CHECK(s.telem.temp_max == TEMP_NONE && s.telem.temp_avg == TEMP_NONE,
            "before any MSG_ASIC_TELEM both temps are the sentinel (got %d/%d)", s.telem.temp_max,
            s.telem.temp_avg);

    chips_blank(&ct);
    ct.pvt_lapped[0] = 0;
    ct.pvt_t[0][0]   = 40;
    ct.pvt_t[0][1]   = 70;
    publish(&ct, &s);
    CHECK(s.telem.temp_max == 70, "a partial lap still publishes the MAX (got %d, want 70)",
            s.telem.temp_max);
    CHECK(s.telem.temp_avg == TEMP_NONE, "...and withholds the AVERAGE (got %d, want %d)",
            s.telem.temp_avg, TEMP_NONE);

    ct.pvt_lapped[0] = 1;
    publish(&ct, &s);
    CHECK(s.telem.temp_avg == 55, "a closed lap publishes the average (got %d, want 55)",
            s.telem.temp_avg);

    CHECK(s.telem.temp_avg == 55,
            "the mean divides by the chips that ANSWERED, not by the chip count");

    chips_blank(&ct);
    ct.pvt_t[0][0] = 40;
    ct.pvt_t[0][1] = 70;
    ct.pvt_t[0][2] = 72;
    publish(&ct, &s);
    CHECK(s.telem.temp_avg == 60,
            "the mean truncates toward zero (got %d, want 60 from 182/3=60.67)", s.telem.temp_avg);
    CHECK(s.telem.temp_max == 72,
            "...and the max is the hottest chip that answered (got %d, want 72)", s.telem.temp_max);

    chips_blank(&ct);
    publish(&ct, &s);
    CHECK(s.telem.temp_max == TEMP_NONE && s.telem.temp_avg == TEMP_NONE,
            "an all-sentinel chain publishes neither (got %d/%d)", s.telem.temp_max,
            s.telem.temp_avg);

    if (HBOARD_COUNT > 1) {
        chips_blank(&ct);
        ct.pvt_t[0][0]                = 50;
        ct.pvt_t[HBOARD_COUNT - 1][0] = 82;
        publish(&ct, &s);
        CHECK(s.telem.temp_max == 82, "the max spans every board, not board 0 (got %d, want 82)",
                s.telem.temp_max);

        chips_blank(&ct);
        ct.pvt_t[0][0]                = 60;
        ct.pvt_t[0][1]                = 60;
        ct.pvt_t[0][2]                = 60;
        ct.pvt_t[HBOARD_COUNT - 1][0] = 20;
        publish(&ct, &s);
        CHECK(s.telem.temp_avg == 50, "the mean weighs chips, not boards (got %d, want 50 not 40)",
                s.telem.temp_avg);

        ct.pvt_lapped[HBOARD_COUNT - 1] = 0;
        publish(&ct, &s);
        CHECK(s.telem.temp_avg == TEMP_NONE,
                "one unlapped board withholds the whole average (got %d)", s.telem.temp_avg);
        CHECK(s.telem.temp_max == 60, "...and the max is unaffected by it (got %d, want 60)",
                s.telem.temp_max);

        chips_blank(&ct);
        ct.chips[HBOARD_COUNT - 1]      = 0;
        ct.pvt_lapped[HBOARD_COUNT - 1] = 0;
        ct.pvt_t[0][0]                  = 44;
        ct.pvt_t[0][1]                  = 46;
        publish(&ct, &s);
        CHECK(s.telem.temp_avg == 45,
                "a board that never enumerated does not withhold the average "
                "(got %d, want 45)",
                s.telem.temp_avg);
        CHECK(s.telem.temp_max == 46,
                "...and the max is the hottest chip that did report (got %d, want 46)",
                s.telem.temp_max);
    } else {
        printf("  (cross-board properties not expressible at HBOARD_COUNT=1)\n");
    }

    chips_blank(&ct);
    ct.pvt_t[0][0] = 66;
    ct.pvt_t[0][1] = 74;
    publish(&ct, &s);

    g_chip_at = now_s() - 7;
    api_snapshot(&s, NULL);
    CHECK(s.telem.temp_max == TEMP_NONE && s.telem.temp_avg == TEMP_NONE,
            "a snapshot older than the 6 s window publishes neither (got %d/%d)", s.telem.temp_max,
            s.telem.temp_avg);

    publish(&ct, &s);
    CHECK(s.telem.temp_max == 74, "a fresh snapshot is published again (got %d, want 74)",
            s.telem.temp_max);

    char *buf = malloc(MC_API_REPLY_CAP);
    if (!buf) {
        printf("FAILED (oom)\n");
        return 1;
    }
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_ok(buf) && sj_has(buf, "TMax[74] TAvg[70]"), "estats publishes TMax[74] TAvg[70]");
#if defined(BOARD_X3_H)
    CHECK(sj_has(buf, "MTmax[74 -273] MTavg[70]"),
            "...and MTmax lists each board's own max, MTavg the machine mean");
#elif defined(BOARD_NANO3S_H)
    CHECK(sj_has(buf, "MTmax[74] MTavg[70]"),
            "...and the same pair again as MTmax/MTavg (one board)");
#else
#error "no MTmax goldens for this board"
#endif
    free(buf);

    return tst_done();
}
