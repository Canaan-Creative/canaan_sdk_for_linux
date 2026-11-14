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

#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "boardconf.h"
#include "tst_stats_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            fails++; \
        } \
    } while (0)

/* Per-board HW error count whose board sum fills a uint32_t */
#define HW_PER_BOARD (4294967295u / (unsigned)HBOARD_COUNT)

static int fails;

static void fill_full_chain(mc_api_stats *s)
{
    static char widest_ver[MINER_VERSION_CAP];
    for (size_t i = 0; i + 1 < sizeof widest_ver; i++)
        widest_ver[i] = (char)('0' + (int)(i % 10));
    widest_ver[sizeof widest_ver - 1] = '\0';

    memset(s, 0, sizeof *s);
    s->prod    = "AVALON";
    s->model   = "MODEL";
    s->hwtype  = "HW";
    s->swtype  = "SW";
    s->version = widest_ver;
    s->dna     = "0123456789abcdef";
    s->mac     = "00:11:22:33:44:55";
    memcpy(s->mm_version, widest_ver, sizeof s->mm_version);
    s->when          = 4294967295u;
    s->elapsed       = 4294967295u;
    s->local_work    = 4294967295u;
    s->mm_local_work = 4294967295u;
    s->dhw           = 4294967295u;
    s->hw_errors     = HW_PER_BOARD * (unsigned)HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        s->hw_board[b]   = HW_PER_BOARD;
        s->hu_errcode[b] = 4294967295u;
    }
    s->mm_errcode    = 4294967295u;
    s->pool_idle     = 1;
    s->mhs_av        = 99999999.0;
    s->mhs_5m        = 99999999.0;
    s->diff_accepted = 99999999.0;
    s->max_pout      = 3500;
    s->target_temp   = 90;
    s->workmode      = 2;
    s->caliall       = 2147483647;
    s->bar           = 2147483647;
    s->adj           = 0;
    s->led_temper    = 4294967295u;
    s->led_rgb       = 0xFFFFFFu;

    mc_api_telem *t = &s->telem;
    t->valid        = 1;
    t->in_work      = 1;
    t->freq_mhz     = 9999;
    t->temp_max = t->temp_avg = t->board_temp = -273;
    for (int i = 0; i < 7; i++)
        t->ps[i] = 999999;
    for (int f = 0; f < FAN_COUNT; f++)
        t->fan_rpm[f] = 99999;
    t->fan_pct      = 100;
    t->ghs_mm       = 99999999.0;
    t->ghs_spd_chip = 99999999.0;
    t->hb_count     = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++)
        t->hb_outlet[b] = t->hb_inlet[b] = -273;

    t->board_n = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        t->chip_n[b] = MINER_MAX_CHIPS;
        for (int c = 0; c < MINER_MAX_CHIPS; c++) {
            t->chip_pvt_t[b][c] = -273;
            t->chip_pvt_v[b][c] = 9999;
            t->chip_pass[b][c]  = 4294967295u;
            t->chip_fail[b][c]  = 4294967295u;
            t->chip_mw[b][c]    = 18446744073709551615ull;
            for (int k = 0; k < ASIC_PLL_POINTS; k++) {
                t->chip_pllcnt[b][c][k] = 255;
                t->chip_cf[b][c][k]     = 9999;
            }
        }
    }
}

static void expect_last_json_field_complete(const char *reply, int show_all, int nc,
        const char *what)
{
    char      key[32];
    const int b = HBOARD_COUNT - 1;
    if (show_all)
        snprintf(key, sizeof key, "GHSmm%02d", b);
    else
        snprintf(key, sizeof key, "MW%d", b);

    CHECK(sj_ok(reply),
            "%s: the reply is not one well-formed JSON document with MMID0Summary an "
            "object — truncated (reply %zu bytes, chain %d chips)",
            what, strlen(reply), (int)MINER_MAX_CHIPS);
    CHECK(sj_array_len(reply, key) == nc,
            "%s: last field '%s' carries %d value(s), expected %d — HB_%d_Info was "
            "truncated (reply %zu bytes)",
            what, key, sj_array_len(reply, key), nc, b, strlen(reply));
}

static void expect_last_field_complete(const char *reply, int show_all, const char *what)
{
    char      label[32];
    const int b = HBOARD_COUNT - 1;
    if (show_all)
        snprintf(label, sizeof label, " GHSmm%02d[", b);
    else
        snprintf(label, sizeof label, " MW%d[", b);

    const char *p = strstr(reply, label);
    if (!p) {
        CHECK(0,
                "%s: last field '%s' missing entirely — MM ID0 was truncated "
                "before it (reply %zu bytes, chain %d chips)",
                what, label, strlen(reply), (int)MINER_MAX_CHIPS);
        return;
    }
    const char *close = strchr(p + strlen(label), ']');
    CHECK(close != NULL,
            "%s: last field '%s' never closes — truncated mid-array "
            "(reply %zu bytes)",
            what, label, strlen(reply));
}

int main(void)
{
    printf("descriptor %s: %d board(s) x %d chip(s), mm cap %u, reply cap %u\n", BOARDCONF_SECTION,
            (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, (unsigned)MC_API_MM_ID0_CAP,
            (unsigned)MC_API_REPLY_CAP);

    CHECK(MINER_MAX_CHIPS == MAX_ASIC_ON_SINGLE_HASH,
            "MINER_MAX_CHIPS %d != descriptor MAX_ASIC_ON_SINGLE_HASH %d", (int)MINER_MAX_CHIPS,
            (int)MAX_ASIC_ON_SINGLE_HASH);

    mc_api_stats s;
    fill_full_chain(&s);

    char *reply = malloc(MC_API_REPLY_CAP);
    if (!reply) {
        printf("FAILED (oom)\n");
        return 1;
    }

    for (int show_all = 0; show_all <= 1; show_all++) {
        size_t n = mc_api_stats_(&s, reply, MC_API_REPLY_CAP, show_all, 0);
        CHECK(n < MC_API_REPLY_CAP,
                "json estats%s: %zu bytes does not fit the "
                "%u-byte reply buffer",
                show_all ? "|all" : "", n, (unsigned)MC_API_REPLY_CAP);
        expect_last_json_field_complete(reply, show_all, MINER_MAX_CHIPS,
                show_all ? "json estats|all" : "json estats");

        n = mc_api_stats_text(&s, reply, MC_API_REPLY_CAP, show_all, 0);
        CHECK(n < MC_API_REPLY_CAP,
                "text estats%s: %zu bytes does not fit the "
                "%u-byte reply buffer",
                show_all ? "|all" : "", n, (unsigned)MC_API_REPLY_CAP);
        expect_last_field_complete(reply, show_all, show_all ? "text estats|all" : "text estats");
    }

    char want[32 + 2 * MINER_VERSION_CAP], tail[128], hw[24];
    snprintf(want, sizeof want, "Ver[%s-%s] LVer[%s] ", s.model, s.version, s.mm_version);
    snprintf(tail, sizeof tail, " WORKMODE[%d] WORKLEVEL[0] MPO[%d] CALIALL[%d] ADJ[%d] BAR[%d]",
            s.workmode, s.max_pout, s.caliall, s.adj, s.bar);
    snprintf(hw, sizeof hw, " HW[%u]", HW_PER_BOARD * (unsigned)HBOARD_COUNT);

    mc_api_stats_(&s, reply, MC_API_REPLY_CAP, 1, 0);
    CHECK(sj_array_len(reply, "PVT_V0") == MINER_MAX_CHIPS,
            "PVT_V0 carries %d value(s), expected the full %d-chip chain",
            sj_array_len(reply, "PVT_V0"), (int)MINER_MAX_CHIPS);

    CHECK(sj_has(reply, want), "Ver[]/LVer[] render whole at the %d-character version cap",
            (int)MINER_VERSION_CAP - 1);

    CHECK(sj_has(reply, tail),
            "the Hashinfo TAIL survives the widest chain (\"%s\"): "
            "expect_last_json_field_complete() above checks the last per-chip "
            "field, and the app parses WORKMODE, MPO, CALIALL, ADJ and BAR by "
            "field name",
            tail);

    {
        char fans[32 + 16 * FAN_COUNT] = "OTemp[-273]";
        for (int f = 0; f < FAN_COUNT; f++)
            snprintf(fans + strlen(fans), sizeof fans - strlen(fans), " Fan%d[99999]", f + 1);
        strcat(fans, " FanR[100]");
        CHECK(sj_has(reply, fans), "every fan field renders whole, in order, before FanR (\"%s\")",
                fans);
    }

    char mh[32 + 11 * HBOARD_COUNT] = " MH[", echu[32 + 11 * HBOARD_COUNT] = " ECHU[";
    for (int b = 0; b < HBOARD_COUNT; b++) {
        snprintf(mh + strlen(mh), sizeof mh - strlen(mh), "%s%u", b ? " " : "", HW_PER_BOARD);
        snprintf(echu + strlen(echu), sizeof echu - strlen(echu), "%s4294967295", b ? " " : "");
    }
    strcat(mh, "] DHW[");
    strcat(echu, "]");
    {
        CHECK(strlen(hw) == 15 && sj_has(reply, hw),
                "HW[..] must render the %d-board total at the full 10 digits "
                "(\"%s\")",
                (int)HBOARD_COUNT, hw);
        CHECK(sj_has(reply, " PING[0] POOLS[1] SoftOFF["),
                "POOLS[..] renders whole between PING[..] and SoftOFF[..]");
        CHECK(sj_has(reply, mh), "%d board(s) of MH[..] render whole at the widest value (\"%s\")",
                (int)HBOARD_COUNT, mh);
        CHECK(sj_has(reply, echu) && sj_has(reply, "ECMM[4294967295]"),
                "%d board(s) of ECHU[..] render whole at the widest value (\"%s\"), and "
                "ECMM too",
                (int)HBOARD_COUNT, echu);
    }

    mc_api_stats_text(&s, reply, MC_API_REPLY_CAP, 1, 0);
    CHECK(strstr(reply, want) != NULL,
            "text: Ver[]/LVer[] render whole at the %d-character version cap",
            (int)MINER_VERSION_CAP - 1);
    CHECK(strstr(reply, tail) != NULL,
            "text: the MM ID0 TAIL survives the widest chain (\"%s\"). "
            "expect_last_field_complete() above only checks the last PER-CHIP "
            "field; build_mm_id0 appends this tail AFTER that block through "
            "ap(), which returns silently once the buffer is full — so a "
            "MC_API_MM_ID0_CAP that is one field too small drops WORKMODE, "
            "MPO, CALIALL, ADJ and BAR with no other symptom, and the app "
            "parses MM ID0 by field name",
            tail);
    CHECK(strstr(reply, hw) != NULL,
            "text: HW[..] must render the %d-board total at the full 10 digits (\"%s\")",
            (int)HBOARD_COUNT, hw);
    CHECK(strstr(reply, " PING[0] POOLS[1] SoftOFF[") != NULL,
            "text: POOLS[..] renders whole between PING[..] and SoftOFF[..]");
    {
        const char *first = strstr(reply, " Fan1[");
        const char *duty  = strstr(reply, " FanR[");
        if (!first || !duty || duty < first) {
            CHECK(0, "text fan fields missing or out of order (Fan1 %s, FanR %s)",
                    first ? "found" : "absent", duty ? "found" : "absent");
        } else {
            CHECK(duty[-1] == ']',
                    "the fan list is cut off before its last ']' (byte before "
                    "FanR is '%c') — MC_API_FAN_FIELD_CAP %u is too small for "
                    "%d fan(s)",
                    duty[-1], (unsigned)MC_API_FAN_FIELD_CAP, (int)FAN_COUNT);
            size_t used   = (size_t)(duty - first);
            size_t budget = (size_t)FAN_COUNT * MC_API_FAN_FIELD_CAP;
            CHECK(used <= budget,
                    "%d fan field(s) render %zu bytes at the widest, over the "
                    "%zu-byte budget MC_API_MM_ID0_CAP reserves (%u B/fan)",
                    (int)FAN_COUNT, used, budget, (unsigned)MC_API_FAN_FIELD_CAP);
        }
    }

    {
        strcat(echu, " ECMM[4294967295]");
        CHECK(strstr(reply, mh) != NULL,
                "text: %d board(s) of MH[..] render whole at the widest value (\"%s\") — "
                "MC_API_MH_CAP %u",
                (int)HBOARD_COUNT, mh, (unsigned)MC_API_MH_CAP);
        CHECK(strstr(reply, echu) != NULL,
                "text: %d board(s) of ECHU[..] render whole at the widest value (\"%s\") — "
                "MC_API_ECHU_CAP %u",
                (int)HBOARD_COUNT, echu, (unsigned)MC_API_ECHU_CAP);

        const char *ms = strstr(reply, " MH["), *mc = ms ? strchr(ms, ']') : NULL;
        const char *es = strstr(reply, " ECHU["), *ec = es ? strchr(es, ']') : NULL;
        if (!ms || !mc || !es || !ec) {
            CHECK(0, "MH[..] or ECHU[..] missing from the reply entirely");
        } else {
            size_t mused = (size_t)(mc - (ms + 4)), eused = (size_t)(ec - (es + 6));
            CHECK(mused <= (size_t)HBOARD_COUNT * MC_API_MH_CAP,
                    "MH[..] renders %zu byte(s), over the %zu the budget reserves "
                    "(%u B/board)",
                    mused, (size_t)HBOARD_COUNT * MC_API_MH_CAP, (unsigned)MC_API_MH_CAP);
            CHECK(eused <= (size_t)HBOARD_COUNT * MC_API_ECHU_CAP,
                    "ECHU[..] renders %zu byte(s), over the %zu the budget reserves "
                    "(%u B/board)",
                    eused, (size_t)HBOARD_COUNT * MC_API_ECHU_CAP, (unsigned)MC_API_ECHU_CAP);
        }
    }

    {
        size_t full[2], none[2], mixed[2];
        for (int show_all = 0; show_all <= 1; show_all++) {
            mc_api_stats w;

            fill_full_chain(&w);
            full[show_all] = mc_api_stats_(&w, reply, MC_API_REPLY_CAP, show_all, 0);

            fill_full_chain(&w);
            w.telem.board_n = 0;
            for (int b = 0; b < HBOARD_COUNT; b++)
                w.telem.chip_n[b] = 0;
            none[show_all] = mc_api_stats_(&w, reply, MC_API_REPLY_CAP, show_all, 0);
            expect_last_json_field_complete(reply, show_all, 0,
                    show_all ? "chipless estats|all" : "chipless estats");

            fill_full_chain(&w);
            for (int b = 1; b < HBOARD_COUNT; b++)
                w.telem.chip_n[b] = 0;
            mixed[show_all] = mc_api_stats_(&w, reply, MC_API_REPLY_CAP, show_all, 0);
            expect_last_json_field_complete(reply, show_all, w.telem.chip_n[HBOARD_COUNT - 1],
                    show_all ? "one-board estats|all" : "one-board estats");

            CHECK(none[show_all] <= full[show_all] && mixed[show_all] <= full[show_all],
                    "estats%s: the per-chip labels are emitted for every one of "
                    "the %d board(s), populated or not, so a chipless board "
                    "costs its fixed labels — that "
                    "must stay no wider than the same board WITH its chips, "
                    "which is the worst case fill_full_chain() sizes "
                    "MC_API_MM_ID0_CAP against (full %zu, chipless %zu, "
                    "one-board %zu)",
                    show_all ? "|all" : "", (int)HBOARD_COUNT, full[show_all], none[show_all],
                    mixed[show_all]);
            CHECK(none[show_all] < MC_API_REPLY_CAP && mixed[show_all] < MC_API_REPLY_CAP,
                    "estats%s: chipless/one-board replies fit the %u-byte buffer",
                    show_all ? "|all" : "", (unsigned)MC_API_REPLY_CAP);
        }
    }

    free(reply);
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
