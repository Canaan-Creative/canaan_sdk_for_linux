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

#define _GNU_SOURCE
#include <sys/time.h>
#include "harness_todo.h"
#include "harness_fixture.h"
#include "status.h"
#include "webauth.h"
#include "mc_api_server.h"
#include "mc_api_cmds.h"
#include "mc_api_setter.h"
#include "boardconf.h"
#include "board_tables.h"
#include "ctrl_cmds.h"
#include "core_board.h"

#define ASCSET_ROW(e, n, p, c) { CTRL_OP_##e, n },

int tst_settimeofday(const struct timeval *tv, const struct timezone *tz);
/* routes settimeofday in mc_api_time.c to the no-op stub */
#define settimeofday(tv, tz) tst_settimeofday((tv), (tz))
#include "mc_api_time.c"
#undef settimeofday

int tst_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv;
    (void)tz;
    return 0;
}

static const char *fx_elem(const char *json)
{
    const char *b = strchr(json, '[');
    return b ? b : json;
}

static int fx_field(const char *json, const char *key, char *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    size_t n = 0;
    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (!e)
            return -1;
        n = (size_t)(e - p);
    } else {
        while (p[n] == '-' || (p[n] >= '0' && p[n] <= '9'))
            n++;
        if (!n)
            return -1;
    }
    if (n >= cap)
        return -1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static void ascset(const char *param, char *buf, size_t cap)
{
    char         req[160];
    mc_api_stats s;
    memset(&s, 0, sizeof s);
    snprintf(req, sizeof req, "{\"command\":\"ascset\",\"parameter\":\"%s\"}", param);
    mc_api_dispatch(req, "ascset", &s, buf, cap);
}

static void expect_like(const char *fx, const char *ours, const char *what)
{
    char st[8], code[8], msg[256], needle[320];
    if (fx_field(fx_elem(fx), "STATUS", st, sizeof st) != 0 ||
            fx_field(fx_elem(fx), "Code", code, sizeof code) != 0 ||
            fx_field(fx_elem(fx), "Msg", msg, sizeof msg) != 0) {
        HF_CHECK(0, "%s: capture not parseable", what);
        return;
    }
    snprintf(needle, sizeof needle, "\"STATUS\":\"%s\"", st);
    HF_CHECK(strstr(ours, needle) != NULL, "%s: STATUS %s as captured", what, st);
    snprintf(needle, sizeof needle, "\"Code\":%s", code);
    HF_CHECK(strstr(ours, needle) != NULL, "%s: Code %s as captured", what, code);
    snprintf(needle, sizeof needle, "\"Msg\":\"%s\"", msg);
    HF_CHECK(strstr(ours, needle) != NULL, "%s: Msg \"%s\" as captured", what, msg);
}

static mc_set_result stub_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t cap, void *ctx)
{
    (void)op;
    (void)s;
    const char *name = (const char *)ctx;
    if (args && *args) {
        snprintf(msg, cap, "%s set", name);
        return MC_SET_OK;
    }
    snprintf(msg, cap, "missing %s setting", name);
    return MC_SET_ERR_ARG;
}

int main(int argc, char **argv)
{
    const char *id = argc > 1 ? argv[1] : "";

    if (!strcmp(id, "api_ascset_workmode_err")) {
        hf_fails                         = 0;
        static const char *const wm[][2] = {
            { "workmode", "api/ascset_workmode_err.json" },
            { "worklevel", "api/ascset_worklevel_err.json" },
            { "work_mode_lvl", "api/ascset_work_mode_lvl_err.json" },
        };
        for (size_t i = 0; i < sizeof wm / sizeof wm[0]; i++) {
            char *r = hf_read(wm[i][1]);
            HF_CHECK(r != NULL, "%s present", wm[i][1]);
            if (r) {
                char ours[1024], param[48];
                HF_CHECK(mc_api_setter_register(wm[i][0], 0, stub_setter, (void *)wm[i][0]) == 0,
                        "%s handler registered", wm[i][0]);
                snprintf(param, sizeof param, "0,%s", wm[i][0]);
                ascset(param, ours, sizeof ours);
                expect_like(r, ours, wm[i][0]);
            }
            free(r);
        }
        HF_DONE("api_ascset_workmode_err");
    }
    if (!strcmp(id, "api_pool_commands")) {
        hf_fails  = 0;
        char *en  = hf_read("api/enablepool_0.json");
        char *dis = hf_read("api/disablepool_badidx.json");
        char *pb  = hf_read("api/pools_before.json");
        HF_CHECK(en && dis && pb, "pool-command fixtures present");
        if (en)
            HF_CHECK(strstr(en, "already enabled") != NULL, "enablepool 0: already enabled");
        if (dis)
            HF_CHECK(strstr(dis, "Invalid pool id 9 - range is 0 - 2") != NULL,
                    "disablepool 9: out-of-range error");
        if (pb)
            HF_CHECK(strstr(pb, "\"Msg\":\"3 Pool(s)\"") != NULL, "pools list = 3 pools");

        char ours[512];
        {
            char msg[220];
            snprintf(msg, sizeof msg, "Pool %d:'%s' already %s", 0,
                    "stratum+tcp://btc.f2pool.com:3333", "enabled");
            mc_api_status_reply(1, ours, sizeof ours, 0, 'I', 49, msg);
            if (en)
                expect_like(en, ours, "enablepool 0 already-enabled");
        }
        {
            char msg[64];
            snprintf(msg, sizeof msg, "Invalid pool id %d - range is 0 - %d", 9, 2);
            mc_api_status_reply(1, ours, sizeof ours, 0, 'E', 26, msg);
            if (dis)
                expect_like(dis, ours, "disablepool 9 out-of-range");
        }
        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 47, "Enabling pool 0:'u'");
        HF_CHECK(strstr(ours, "\"STATUS\":\"S\"") && strstr(ours, "\"Code\":47") &&
                        strstr(ours, "\"Msg\":\"Enabling pool 0:'u'\"") != NULL,
                "enablepool ok -> S/47 Enabling pool N:'url'");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 48, "Disabling pool 0:'u'");
        HF_CHECK(strstr(ours, "\"STATUS\":\"S\"") && strstr(ours, "\"Code\":48") &&
                        strstr(ours, "\"Msg\":\"Disabling pool 0:'u'\"") != NULL,
                "disablepool ok -> S/48 Disabling pool N:'url'");

        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 55,
                "Added pool 2:'stratum+tcp://h:3333'");
        HF_CHECK(strstr(ours, "\"Code\":55") &&
                        strstr(ours, "\"Msg\":\"Added pool 2:'stratum+tcp://h:3333'\"") != NULL,
                "addpool ok -> S/55 Added pool N");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'E', 52, "Missing addpool details");
        HF_CHECK(strstr(ours, "\"STATUS\":\"E\"") && strstr(ours, "\"Code\":52") != NULL,
                "addpool missing -> E/52");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'E', 53, "Invalid addpool details");
        HF_CHECK(strstr(ours, "\"Code\":53") != NULL, "addpool invalid/full -> E/53");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 68, "Removed pool 1:'u'");
        HF_CHECK(strstr(ours, "\"Code\":68") != NULL, "removepool ok -> S/68 Removed pool N");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'E', 66, "Cannot remove last pool 0:'u'");
        HF_CHECK(strstr(ours, "\"Code\":66") != NULL, "removepool last -> E/66");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'E', 67, "Cannot remove active pool 0:'u'");
        HF_CHECK(strstr(ours, "\"Code\":67") != NULL, "removepool active -> E/67");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 27, "Switching to pool 1:'u'");
        HF_CHECK(strstr(ours, "\"Code\":27") != NULL, "switchpool ok -> S/27 Switching to pool N");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 73, "Changed pool priorities");
        HF_CHECK(strstr(ours, "\"Code\":73") &&
                        strstr(ours, "\"Msg\":\"Changed pool priorities\"") != NULL,
                "poolpriority ok -> S/73");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'E', 74, "Duplicate pool specified 0");
        HF_CHECK(strstr(ours, "\"Code\":74") != NULL, "poolpriority dup -> E/74");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 97, "Zeroed all stats");
        HF_CHECK(strstr(ours, "\"Code\":97") &&
                        strstr(ours, "\"Msg\":\"Zeroed all stats\"") != NULL,
                "zero all -> S/97 Zeroed all stats");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'S', 96, "Zeroed all stats with summary");
        HF_CHECK(strstr(ours, "\"Code\":96") != NULL, "zero all,true -> S/96 with summary");
        mc_api_status_reply(1, ours, sizeof ours, 0, 'E', 95, "Invalid zero parameter 'foo'");
        HF_CHECK(strstr(ours, "\"Code\":95") != NULL, "zero bad param -> E/95");
        free(en);
        free(dis);
        free(pb);

        mc_api_stats ps;
        memset(&ps, 0, sizeof ps);
        ps.when    = 100;
        ps.npools  = 2;
        ps.pool[0] = (mc_api_pool){ .id = 0,
            .url                        = "stratum+tcp://btc.f2pool.com:3333",
            .status                     = "Alive",
            .user                       = "poolworker.001",
            .accepted                   = 37,
            .rejected                   = 3,
            .diff_accepted              = 671744.0,
            .diff_rejected              = 24576.0,
            .work_diff                  = 131072.0,
            .last_share_diff            = 131072.0,
            .last_share_time            = 1787128137,
            .best_share                 = 9295948727197.0,
            .diff1_shares               = 9408720896.0,
            .getworks                   = 268727,
            .bad_work                   = 10739,
            .block_version              = 536870912,
            .works                      = 2297075,
            .get_failures               = 46,
            .remote_failures            = 4,
            .block_height               = 956842,
            .stratum_active             = 1 };
        ps.pool[1] = (mc_api_pool){ .id = 1,
            .url                        = "stratum+tcp://btc.f2pool.com:3333",
            .status                     = "Dead",
            .user                       = "poolworker.001",
            .accepted                   = 0,
            .rejected                   = 0,
            .work_diff                  = 524288.0,
            .stratum_active             = 0 };
        char pbuf[8192];
        mc_api_pools(&ps, pbuf, sizeof pbuf);
        static const char *F[] = {
            "\"POOL\":",
            "\"URL\":",
            "\"Status\":",
            "\"Priority\":",
            "\"Quota\":",
            "\"Long Poll\":",
            "\"Getworks\":",
            "\"Accepted\":",
            "\"Rejected\":",
            "\"Works\":",
            "\"Discarded\":",
            "\"Stale\":",
            "\"Get Failures\":",
            "\"Remote Failures\":",
            "\"User\":",
            "\"Last Share Time\":",
            "\"Diff1 Shares\":",
            "\"Proxy Type\":",
            "\"Proxy\":",
            "\"Difficulty Accepted\":",
            "\"Difficulty Rejected\":",
            "\"Difficulty Stale\":",
            "\"Last Share Difficulty\":",
            "\"Work Difficulty\":",
            "\"Has Stratum\":",
            "\"Stratum Active\":",
            "\"Stratum URL\":",
            "\"Stratum Difficulty\":",
            "\"Has Vmask\":",
            "\"Has GBT\":",
            "\"Best Share\":",
            "\"Pool Rejected%\":",
            "\"Pool Stale%\":",
            "\"Bad Work\":",
            "\"Current Block Height\":",
            "\"Current Block Version\":",
        };
        int         nF      = (int)(sizeof F / sizeof F[0]);
        const char *prev    = pbuf;
        int         ordered = 1;
        for (int k = 0; k < nF; k++) {
            const char *at = strstr(prev, F[k]);
            if (!at) {
                ordered = 0;
                break;
            }
            prev = at + 1;
        }
        HF_CHECK(nF == 36, "pool field set = 36 API fields");
        HF_CHECK(ordered, "mc_api_pools emits all fields in API order");
        HF_CHECK(strstr(pbuf, "\"Password\":") == NULL, "JSON pools omits Password");
        HF_CHECK(strstr(pbuf, "\"Difficulty Accepted\":671744.00000000") != NULL,
                "JSON per-pool Difficulty Accepted = pool0 diff_accepted");
        HF_CHECK(strstr(pbuf, "\"Difficulty Rejected\":24576.00000000") != NULL,
                "JSON per-pool Difficulty Rejected = pool0 diff_rejected");
        {
            mc_api_stats ds    = ps;
            ds.accepted        = 37;
            ds.rejected        = 3;
            ds.diff_accepted   = 671744.0;
            ds.diff_rejected   = 24576.0;
            ds.best_share      = 296878;
            ds.last_valid_work = 1787128137;
            char sbuf[4096];
            mc_api_summary(&ds, sbuf, sizeof sbuf);
            HF_CHECK(strstr(sbuf,
                             "\"Difficulty Accepted\":671744.00000000,"
                             "\"Difficulty Rejected\":24576.00000000,"
                             "\"Difficulty Stale\":0.00000000,"
                             "\"Best Share\":296878") != NULL,
                    "summary Difficulty Rejected carries the real figure in place");
            mc_api_devs(&ds, sbuf, sizeof sbuf);
            HF_CHECK(strstr(sbuf,
                             "\"Difficulty Accepted\":671744.00000000,"
                             "\"Difficulty Rejected\":24576.00000000,"
                             "\"Last Share Difficulty\":131072.00000000,"
                             "\"Last Valid Work\":1787128137,"
                             "\"Device Hardware%\":0.0000") != NULL,
                    "devs Difficulty Rejected carries the real figure in place");
            ds.diff_rejected = 0.0;
            mc_api_summary(&ds, sbuf, sizeof sbuf);
            HF_CHECK(strstr(sbuf,
                             "\"Difficulty Rejected\":0.00000000,"
                             "\"Difficulty Stale\":0.00000000,"
                             "\"Best Share\":296878") != NULL,
                    "summary zero-reject form unchanged");
            mc_api_devs(&ds, sbuf, sizeof sbuf);
            HF_CHECK(strstr(sbuf,
                             "\"Difficulty Rejected\":0.00000000,"
                             "\"Last Share Difficulty\":131072.00000000,"
                             "\"Last Valid Work\":1787128137,"
                             "\"Device Hardware%\":0.0000") != NULL,
                    "devs zero-reject form unchanged");
        }
        {
            mc_api_stats us         = ps;
            us.accepted             = 37;
            us.rejected             = 3;
            us.elapsed              = 565;
            us.dev_elapsed          = 539.0;
            us.mhs_av               = 5693980.85;
            us.diff_accepted        = 671744.0;
            us.best_share           = 296878;
            us.diff1                = 9408724992.0;
            us.pool[1].diff1_shares = 4096.0;
            char ubuf[4096];
            mc_api_summary(&us, ubuf, sizeof ubuf);
            HF_CHECK(strstr(ubuf,
                             "\"Hardware Errors\":0,\"Utility\":3.93,"
                             "\"Discarded\":0") != NULL,
                    "summary Utility = accepted shares/min, in its API slot");
            HF_CHECK(strstr(ubuf, "\"Utility\":3.929") == NULL &&
                            strstr(ubuf, "\"Utility\":3.9,") == NULL,
                    "summary Utility keeps the 2-decimal API format");
            HF_CHECK(strstr(ubuf,
                             "\"Network Blocks\":0,"
                             "\"Total MH\":3217099180.2500,"
                             "\"Work Utility\":") != NULL,
                    "summary Total MH = MHS av * Elapsed, in its API slot");
            HF_CHECK(strstr(ubuf, "\"Total MH\":3217099180.25,") == NULL,
                    "summary Total MH keeps the 4-decimal API format");
            mc_api_devs(&us, ubuf, sizeof ubuf);
            HF_CHECK(strstr(ubuf,
                             "\"Hardware Errors\":0,\"Utility\":4.12,"
                             "\"Last Share Pool\":0") != NULL,
                    "devs Utility in its API slot, on the device clock");
            HF_CHECK(strstr(ubuf,
                             "\"Total MH\":3217099180.2500,"
                             "\"Diff1 Work\":9408724992,"
                             "\"Difficulty Accepted\":671744.00000000") != NULL,
                    "devs Diff1 Work in its API slot");
            HF_CHECK(strstr(ubuf, "\"Diff1 Work\":9408724992.0") == NULL &&
                            strstr(ubuf, "\"Diff1 Work\":9408720896,") == NULL,
                    "devs Diff1 Work is integer-shaped, and is not pool 0 alone");
            HF_CHECK(strstr(pbuf, "\"Diff1 Shares\":9408720896,") != NULL,
                    "... and pools still carries its own per-pool column");
            mc_api_summary_text(&us, ubuf, sizeof ubuf);
            HF_CHECK(strstr(ubuf, "Hardware Errors=0,Utility=3.93,Discarded=0") != NULL &&
                            strstr(ubuf,
                                    "Network Blocks=0,Total MH=3217099180.2500,"
                                    "Work Utility=") != NULL,
                    "text summary Utility + Total MH in their API slots");
            mc_api_devs_text(&us, ubuf, sizeof ubuf);
            HF_CHECK(strstr(ubuf,
                             "Hardware Errors=0,Utility=4.12,"
                             "Last Share Pool=0") != NULL &&
                            strstr(ubuf,
                                    "Total MH=3217099180.2500,Diff1 Work=9408724992,"
                                    "Difficulty Accepted=") != NULL,
                    "text devs Utility + Total MH + Diff1 Work in their API slots");
        }
        HF_CHECK(strstr(pbuf, "\"Work Difficulty\":131072.00000000") != NULL,
                "JSON Work Difficulty = pool0 sdiff");
        HF_CHECK(strstr(pbuf, "\"Stratum Difficulty\":131072.00000000") != NULL,
                "JSON active-pool Stratum Difficulty = sdiff");
        HF_CHECK(strstr(pbuf, "\"Work Difficulty\":524288.00000000") != NULL,
                "JSON inactive pool keeps its Work Difficulty (sdiff)");
        HF_CHECK(strstr(pbuf,
                         "\"Stratum Active\":false,\"Stratum URL\":\"\","
                         "\"Stratum Difficulty\":0.00000000") != NULL,
                "JSON inactive pool Stratum Difficulty = 0");
        HF_CHECK(strstr(pbuf, "\"Last Share Time\":1787128137") != NULL,
                "JSON Last Share Time = pool0 last accept");
        HF_CHECK(strstr(pbuf, "\"Last Share Difficulty\":131072.00000000") != NULL,
                "JSON Last Share Difficulty = pool0 last share");
        HF_CHECK(strstr(pbuf, "\"Best Share\":9295948727197") != NULL,
                "JSON Best Share = pool0 best");
        HF_CHECK(strstr(pbuf, "\"Diff1 Shares\":9408720896") != NULL,
                "JSON Diff1 Shares = pool0 cumulative");
        HF_CHECK(strstr(pbuf, "\"Getworks\":268727") != NULL, "JSON Getworks = pool0 jobs");
        HF_CHECK(strstr(pbuf, "\"Bad Work\":10739") != NULL, "JSON Bad Work = pool0 HW");
        HF_CHECK(strstr(pbuf, "\"Current Block Version\":536870912") != NULL,
                "JSON Current Block Version = pool0 job version");
        HF_CHECK(strstr(pbuf, "\"Works\":2297075") != NULL, "JSON Works = pool0 dispatched");
        HF_CHECK(strstr(pbuf, "\"Get Failures\":46") != NULL, "JSON Get Failures = pool0");
        HF_CHECK(strstr(pbuf, "\"Remote Failures\":4") != NULL, "JSON Remote Failures = pool0");
        HF_CHECK(strstr(pbuf, "\"Current Block Height\":956842") != NULL,
                "JSON Current Block Height = pool0 (BIP34)");
        HF_CHECK(strstr(pbuf, "\"Status\":\"Dead\"") != NULL, "JSON pool1 Status = Dead");
        HF_CHECK(strstr(pbuf, "\"Difficulty Accepted\":0.00000000") != NULL,
                "diff fields use 8-decimal format (pool1/2 zero)");
        HF_CHECK(strstr(pbuf, "\"Pool Rejected%\":0.0000") != NULL,
                "percent fields use 4-decimal format (idle pools)");
        HF_CHECK(strstr(pbuf, "\"Pool Rejected%\":3.5294") != NULL,
                "Pool Rejected pct is difficulty-based");
        HF_CHECK(strstr(pbuf, "\"Pool Rejected%\":7.5000") == NULL,
                "Pool Rejected pct is NOT count-based");
        HF_CHECK(strstr(pbuf, "\"Stratum URL\":\"btc.f2pool.com\"") != NULL,
                "active pool Stratum URL = bare host");
        HF_CHECK(strstr(pbuf, "\"Stratum URL\":\"\"") != NULL, "inactive pool Stratum URL blank");

        char tbuf[8192];
        mc_api_dispatch_text("pools", "pools", &ps, tbuf, sizeof tbuf);
        HF_CHECK(strstr(tbuf, "unsupported") == NULL, "text pools is supported");
        HF_CHECK(strstr(tbuf, "Msg=2 Pool(s)") != NULL, "text pools STATUS = 2 Pool(s)");
        HF_CHECK(strstr(tbuf, "POOL=0,URL=stratum+tcp://btc.f2pool.com:3333") != NULL,
                "text pools emits POOL record");
        HF_CHECK(strstr(tbuf, "Stratum URL=btc.f2pool.com,") != NULL,
                "text pools Stratum URL = bare host");
        HF_CHECK(strstr(tbuf, "Current Block Version=0|") != NULL,
                "text pools carries the full field set");
        HF_CHECK(strstr(tbuf, "Difficulty Accepted=671744.00000000,") != NULL,
                "text per-pool Difficulty Accepted = pool0 diff_accepted");
        HF_CHECK(strstr(tbuf, "Difficulty Rejected=24576.00000000,") != NULL,
                "text per-pool Difficulty Rejected = pool0 diff_rejected");
        HF_CHECK(strstr(tbuf, "Stratum Difficulty=131072.00000000,") != NULL,
                "text active-pool Stratum Difficulty = sdiff");
        HF_CHECK(strstr(tbuf, "Last Share Time=1787128137,") != NULL,
                "text Last Share Time = pool0 last accept");
        HF_CHECK(strstr(tbuf, ",Password=") == NULL &&
                        strstr(tbuf, "User=poolworker.001,Last Share Time=") != NULL,
                "text pools omits Password, User adjoins Last Share Time");
        HF_DONE("api_pool_commands");
    }
    if (!strcmp(id, "api_ascset_unknown_option")) {
        hf_fails  = 0;
        char *unk = hf_read("api/softoff.json");
        HF_CHECK(unk != NULL, "unknown-option fixture present");
        if (unk) {
            char ours[1024];
            ascset("0,softoff", ours, sizeof ours);
            expect_like(unk, ours, "unregistered subcommand");
        }
        free(unk);
        HF_DONE("api_ascset_unknown_option");
    }
    if (!strcmp(id, "api_ascset_ip_absent")) {
        hf_fails  = 0;
        char *unk = hf_read("api/softoff.json");
        HF_CHECK(unk != NULL, "unknown-option fixture present");
        int on_board = 0;
        for (size_t i = 0; i < board_ctrl_op_count; i++)
            if (board_ctrl_ops[i] == CTRL_OP_RESERVED_5)
                on_board = 1;
        HF_CHECK(!on_board, "CTRL_OP_RESERVED_5 absent from this board's BOARD_CTRL_OPS");
        {
            static const struct {
                uint16_t    op;
                const char *name;
            } ascset_ops[] = { CTRL_CMD_ASCSET(ASCSET_ROW) };
            int  nreg      = 0;
            char help[1024];
            for (size_t k = 0; k < sizeof ascset_ops / sizeof ascset_ops[0]; k++)
                for (size_t i = 0; i < board_ctrl_op_count; i++)
                    if (board_ctrl_ops[i] == ascset_ops[k].op &&
                            mc_api_setter_register(ascset_ops[k].name, ascset_ops[k].op,
                                    stub_setter, (void *)ascset_ops[k].name) == 0)
                        nreg++;
            ascset("0,help", help, sizeof help);
            HF_CHECK(nreg > 0 && strstr(help, "\"Msg\":\"ASC 0 set info: help|") &&
                            strstr(help, "|ip|") == NULL,
                    "this board's ascset help list carries no ip subcommand (%d registered, "
                    "got %.300s)",
                    nreg, help);
        }
        if (unk) {
            char msg[256], needle[320], ours[1024];
            HF_CHECK(fx_field(fx_elem(unk), "Msg", msg, sizeof msg) == 0,
                    "unknown-option capture parseable");
            char *tail = strstr(msg, "softoff");
            if (tail)
                memcpy(tail, "ip\0", 3);
            ascset("0,ip,dhcp", ours, sizeof ours);
            HF_CHECK(strstr(ours, "\"STATUS\":\"E\"") && strstr(ours, "\"Code\":120"),
                    "ascset 0,ip,dhcp -> E/120 on a board without ip");
            snprintf(needle, sizeof needle, "\"Msg\":\"%s\"", msg);
            HF_CHECK(strstr(ours, needle) != NULL, "reply carries %s", needle);
            ascset("0,ip", ours, sizeof ours);
            HF_CHECK(strstr(ours, needle) != NULL, "bare ascset 0,ip answers the same");
        }
        free(unk);
        HF_DONE("api_ascset_ip_absent");
    }
    if (!strcmp(id, "api_time_set")) {
        hf_fails = 0;
        char         ours[1024];
        mc_api_stats st;
        memset(&st, 0, sizeof st);
        mc_api_dispatch_text("time|set,t:1787102966,America/Los_Angeles", "time", &st, ours,
                sizeof ours);
        HF_CHECK(strstr(ours, "STATUS=I") && strstr(ours, "Code=131"),
                "text time set -> I/131 (nano3s Time set info)");
        mc_api_dispatch("{\"command\":\"time\",\"parameter\":\"set,t:1787102966,UTC\"}", "time",
                &st, ours, sizeof ours);
        HF_CHECK(strstr(ours, "\"STATUS\":\"I\"") && strstr(ours, "\"Code\":131"),
                "json time set -> I/131");
        mc_api_dispatch_text("time|set,t:bad", "time", &st, ours, sizeof ours);
        HF_CHECK(strstr(ours, "STATUS=I") && strstr(ours, "Code=131") &&
                        strstr(ours, "Msg=Time set info: time unknown argument"),
                "malformed time -> I/131 time unknown argument");
        HF_DONE("api_time_set");
    }
    if (!strcmp(id, "api_dashboard_telemetry")) {
        hf_fails = 0;
        char *d  = hf_read("web/get_dashboard.json");
        HF_CHECK(d != NULL, "get_dashboard fixture present");
        if (d) {
            HF_CHECK(strncmp(d, "dashboardCallback(", 18) == 0,
                    "JSONP dashboardCallback(...) wrapper");
            static const char *const keys[] = {
                "\"hwtype\":",
                "\"sys_status\":",
                "\"elapsed\":",
                "\"workingmode\":",
                "\"workingstatus\":",
                "\"power\":",
                "\"realtime_hash\":",
                "\"average_hash\":",
                "\"accepted\":",
                "\"reject\":",
                "\"rejected_percentage\":",
                "\"fan_status\":",
                "\"fanr\":",
                "\"asic_status\":",
                "\"ping\":",
                "\"power_status\":",
                "\"pool_status\":",
                "\"current_pool\":",
                "\"address\":",
                "\"worker\":",
                "\"mac\":",
                "\"version\":",
                "\"pool1\":",
                "\"pool2\":",
                "\"pool3\":",
            };
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
                HF_CHECK(strstr(d, keys[i]) != NULL, "telemetry field %s present", keys[i]);

            mc_dashboard dv = { .hwtype = "Avalon Nano3s",
                .sys_status             = "1",
                .elapsed                = "560",
                .workingmode            = "2",
                .workingstatus          = "1",
                .power                  = "132",
                .realtime_hash          = "5.89",
                .average_hash           = "5.66",
                .accepted               = "36",
                .reject                 = "0",
                .rejected_percentage    = "0",
                .fan_status             = "1",
                .fanr                   = "4200",
                .asic_status            = "1",
                .ping                   = "1",
                .power_status           = "1",
                .pool_status            = "1",
                .current_pool           = "0",
                .address                = "192.168.1.50",
                .worker                 = "w.001",
                .mac                    = "aa:bb:cc:dd:ee:ff",
                .version                = "0.2.0",
                .pool[0] = { .url = "stratum+tcp://x:3333", .worker = "w.001", .passwd = "x" } };
            char         asm_buf[2048];
            size_t       alen = mc_status_dashboard_json(&dv, asm_buf, sizeof asm_buf);
            HF_CHECK(alen < sizeof asm_buf && asm_buf[0] == '{' && asm_buf[alen - 1] == '}',
                    "miner_core assembler emits a plain JSON object");
            int all_asm = 1;
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
                if (!strstr(asm_buf, keys[i]))
                    all_asm = 0;
            HF_CHECK(all_asm, "assembler emits every documented telemetry field");
        }
        free(d);
        HF_DONE("api_dashboard_telemetry");
    }
    if (!strcmp(id, "api_web_auth")) {
        hf_fails = 0;
        mc_webcred cred;
        int        rc = mc_api_load_creds(&cred, HARNESS_FIXTURES_DIR "/config/systemcfg.ini");
        HF_CHECK(rc == 0 && cred.loaded, "web creds loaded from systemcfg.ini");
        if (cred.loaded) {
            HF_CHECK(mc_api_check(&cred, "admin",
                             "0000000000000000000000000000000000000000000000000000000000000000") ==
                            1,
                    "valid login accepted");
            HF_CHECK(mc_api_check(&cred, "admin", "wrong") == 0, "bad pass rejected");
        }
        HF_DONE("api_web_auth");
    }

    HARNESS_UNKNOWN(id);
}
