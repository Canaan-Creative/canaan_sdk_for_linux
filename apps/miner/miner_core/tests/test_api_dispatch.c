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

#include "mc_api_server.h"
#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "mc_api_command.h"
#include "mc_api_setter.h"
#include "mc_api_qrlogin.h"
#include "tst_stats_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tests/include/tst_check.h"

#define NBUF 6 /* Reply buffers allocated by main() */

#define ROUTE_JSON(name, call) \
    do { \
        jreq(req, MC_API_REPLY_CAP, name); \
        mc_api_dispatch(req, name, &s, got, MC_API_REPLY_CAP); \
        call; \
        CHECK(!strcmp(got, want), "json `%s` reaches its builder", name); \
    } while (0)

#define ROUTE_TEXT(name, call) \
    do { \
        mc_api_dispatch_text(name, name, &s, got, MC_API_REPLY_CAP); \
        call; \
        CHECK(!strcmp(got, want), "text `%s` reaches its builder", name); \
    } while (0)

static char *B[NBUF]; /* Reply buffers of MC_API_REPLY_CAP bytes each */

static int stub_runs;

static mc_api_stats fixed_stats(void)
{
    mc_api_stats s           = { 0 };
    s.prod                   = "Avalon Nano3s";
    s.model                  = "Nano3s";
    s.hwtype                 = "N_MM1v1_X1";
    s.swtype                 = "MM319";
    s.version                = "26070301_649070d";
    s.dna                    = "0201000012345678";
    s.mac                    = "020000000001";
    s.when                   = 4133982044u;
    s.elapsed                = 565;
    s.accepted               = 37;
    s.diff_accepted          = 671744.0;
    s.best_share             = 296878;
    s.mhs_av                 = 5693980.85;
    s.asc_count              = 1;
    s.pool_count             = 2;
    s.npools                 = 2;
    s.pool[0]                = (mc_api_pool){ .id = 0,
                       .url                       = "stratum+tcp://btc.f2pool.com:3333",
                       .status                    = "Alive",
                       .user                      = "poolworker.001",
                       .accepted                  = 37,
                       .stratum_active            = 1,
                       .work_diff                 = 65536.0 };
    s.pool[1]                = (mc_api_pool){ .id = 1,
                       .url                       = "stratum+tcp://backup.example:3333",
                       .status                    = "Alive",
                       .user                      = "poolworker.002",
                       .work_diff                 = 8192.0 };
    s.telem                  = (mc_api_telem){ .valid = 1,
                         .in_work                     = 1,
                         .freq_mhz                    = 416,
                         .ghs_mm                      = 6067.10,
                         .temp_max                    = 92,
                         .temp_avg                    = 89,
                         .board_temp                  = 68 };
    s.telem.board_n          = 1;
    s.telem.chip_n[0]        = 2;
    s.telem.chip_pvt_t[0][0] = 90;
    s.telem.chip_pvt_t[0][1] = 88;
    s.block_hash             = "00000000000000000001cb4150b955d47fd97be3d968826bb45111a859f51935";
    s.net_difficulty         = 133869853540305.39;
    return s;
}

static void jreq(char *out, size_t cap, const char *cmd)
{
    snprintf(out, cap, "{\"command\":\"%s\"}", cmd);
}

static size_t stub_cmd(const char *req, const mc_api_stats *s, int is_json, char *buf, size_t cap,
        void *ctx)
{
    (void)req;
    (void)s;
    (void)is_json;
    (void)ctx;
    return (size_t)snprintf(buf, cap, "STUB-HANDLER-RAN");
}

static size_t count_cmd(const char *req, const mc_api_stats *s, int is_json, char *buf, size_t cap,
        void *ctx)
{
    stub_runs++;
    return stub_cmd(req, s, is_json, buf, cap, ctx);
}

int main(void)
{
    for (int i = 0; i < NBUF; i++) {
        B[i] = malloc(MC_API_REPLY_CAP);
        if (!B[i]) {
            printf("FAILED (oom)\n");
            return 1;
        }
    }
    char        *got = B[0], *want = B[1], *alt = B[2], *req = B[3], *t1 = B[4], *t2 = B[5];
    mc_api_stats s = fixed_stats();

    mc_api_command_reset();
    mc_api_setter_reset();

    ROUTE_JSON("version", mc_api_version(&s, want, MC_API_REPLY_CAP));
    ROUTE_JSON("summary", mc_api_summary(&s, want, MC_API_REPLY_CAP));
    ROUTE_JSON("devs", mc_api_devs(&s, want, MC_API_REPLY_CAP));
    ROUTE_JSON("edevs", mc_api_devs(&s, want, MC_API_REPLY_CAP));
    ROUTE_JSON("pools", mc_api_pools(&s, want, MC_API_REPLY_CAP));
    ROUTE_JSON("coin", mc_api_coin(&s, want, MC_API_REPLY_CAP));
    ROUTE_JSON("config", mc_api_config(&s, want, MC_API_REPLY_CAP));
    ROUTE_JSON("stats", mc_api_stats_(&s, want, MC_API_REPLY_CAP, 0, 1));
    ROUTE_JSON("estats", mc_api_stats_(&s, want, MC_API_REPLY_CAP, 0, 0));
    ROUTE_JSON("litestats", mc_api_litestats_(&s, want, MC_API_REPLY_CAP));

    ROUTE_TEXT("version", mc_api_version_text(&s, want, MC_API_REPLY_CAP));
    ROUTE_TEXT("summary", mc_api_summary_text(&s, want, MC_API_REPLY_CAP));
    ROUTE_TEXT("devs", mc_api_devs_text(&s, want, MC_API_REPLY_CAP));
    ROUTE_TEXT("edevs", mc_api_devs_text(&s, want, MC_API_REPLY_CAP));
    ROUTE_TEXT("pools", mc_api_pools_text(&s, want, MC_API_REPLY_CAP));
    ROUTE_TEXT("coin", mc_api_coin_text(&s, want, MC_API_REPLY_CAP));
    ROUTE_TEXT("config", mc_api_config_text(&s, want, MC_API_REPLY_CAP));
    ROUTE_TEXT("stats", mc_api_stats_text(&s, want, MC_API_REPLY_CAP, 0, 1));
    ROUTE_TEXT("estats", mc_api_stats_text(&s, want, MC_API_REPLY_CAP, 0, 0));
    ROUTE_TEXT("litestats", mc_api_litestats_text(&s, want, MC_API_REPLY_CAP));

    jreq(req, MC_API_REPLY_CAP, "stats");
    mc_api_dispatch(req, "stats", &s, got, MC_API_REPLY_CAP);
    jreq(req, MC_API_REPLY_CAP, "estats");
    mc_api_dispatch(req, "estats", &s, alt, MC_API_REPLY_CAP);
    CHECK(strstr(got, "\"ID\":\"POOL0\"") && strstr(got, "\"ID\":\"POOL1\""),
            "json `stats` appends one section per pool");
    CHECK(!strstr(alt, "\"ID\":\"POOL0\""),
            "json `estats` appends none, so the two are not the same reply");
    mc_api_dispatch_text("stats", "stats", &s, got, MC_API_REPLY_CAP);
    mc_api_dispatch_text("estats", "estats", &s, alt, MC_API_REPLY_CAP);
    CHECK(strstr(got, "ID=POOL0,") && strstr(got, "ID=POOL1,"),
            "text `stats` appends one section per pool");
    CHECK(!strstr(alt, "ID=POOL0,"), "text `estats` appends none");

    mc_api_dispatch_text("estats|all", "estats", &s, got, MC_API_REPLY_CAP);
    mc_api_dispatch_text("estats", "estats", &s, alt, MC_API_REPLY_CAP);
    CHECK(strstr(got, "CF0_0[") && !strstr(alt, "CF0_0["),
            "text `estats|all` selects the extended per-chip block");

    mc_api_dispatch("summary", "summary", &s, alt, MC_API_REPLY_CAP);
    mc_api_dispatch("version", "version", &s, req, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP, "{\"summary\":%s,\"version\":%s,\"id\":1}", alt, req);
    mc_api_dispatch("summary+version", "summary+version", &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "json `+` batches sub-replies under their names");
    mc_api_dispatch("pools", "pools", &s, req, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP, "{\"summary\":%s,\"pools\":%s,\"id\":1}", alt, req);
    mc_api_dispatch("{\"command\":\"summary+pools\"}", "summary+pools", &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "json `summary+pools` is the two replies under their names");
    snprintf(want, MC_API_REPLY_CAP,
            "{\"summary\":%s,\"ascset\":{\"STATUS\":[{\"STATUS\":\"E\",\"When\":4133982044,"
            "\"Code\":45,\"Msg\":\"Access denied to 'ascset' command\","
            "\"Description\":\"AvalonMiner " AM_VER "\"}],\"id\":1},\"id\":1}",
            alt);
    mc_api_dispatch("{\"command\":\"summary+ascset\",\"parameter\":\"0,softoff,1\"}",
            "summary+ascset", &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "json `summary+ascset` denies ascset and still answers summary");
    CHECK(mc_api_command_register("zero", count_cmd, NULL) == 0, "register a counted `zero`");
    stub_runs = 0;
    mc_api_dispatch("{\"command\":\"zero+summary\"}", "zero+summary", &s, got, MC_API_REPLY_CAP);
    CHECK(stub_runs == 0 && strstr(got, "\"Msg\":\"Access denied to 'zero' command\"") &&
                    strstr(got, "\"SUMMARY\":["),
            "a registered command in a batch is denied, not run");
    mc_api_dispatch("{\"command\":\"time+check+version\"}", "time+check+version", &s, got,
            MC_API_REPLY_CAP);
    CHECK(strstr(got, "'time' command") && strstr(got, "'check' command") &&
                    strstr(got, "\"VERSION\":["),
            "`time` and `check` are denied in a batch too");
    mc_api_dispatch("{\"command\":\"summary+nosuchcmd\"}", "summary+nosuchcmd", &s, got,
            MC_API_REPLY_CAP);
    CHECK(strstr(got,
                  "\"nosuchcmd\":{\"STATUS\":[{\"STATUS\":\"E\",\"When\":4133982044,"
                  "\"Code\":14,\"Msg\":\"Invalid command\"}],\"id\":1}") != NULL,
            "an unknown command in a batch is the Invalid command record");
    stub_runs = 0;
    mc_api_dispatch_join("zero+zero", 1, &s, got, MC_API_REPLY_CAP);
    CHECK(stub_runs == 0 &&
                    !strcmp(got,
                            "{\"zero\":{\"STATUS\":[{\"STATUS\":\"E\",\"When\":4133982044,"
                            "\"Code\":45,"
                            "\"Msg\":\"Access denied to 'zero' command\","
                            "\"Description\":\"AvalonMiner " AM_VER "\"}],\"id\":1},\"id\":1}"),
            "a repeated denied command is answered once");
    mc_api_command_reset();
    snprintf(want, MC_API_REPLY_CAP, "{\"summary\":%s,\"pools\":%s,\"id\":1}", alt, req);
    mc_api_dispatch_join("summary+pools+summary+pools", 1, &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "json repeats of a known command are skipped");
    mc_api_dispatch("{\"command\":\"nosuch\"}", "nosuch", &s, t1, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP, "{\"nosuch\":%s,\"nosuch\":%s,\"id\":1}", t1, t1);
    mc_api_dispatch_join("nosuch+nosuch", 1, &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "repeats of an unknown command are each answered");
    mc_api_dispatch_join("+", 1, &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, ",\"id\":1}"), "json batch of no command is the bare tail");
    mc_api_dispatch_join("+", 0, &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, ""), "text batch of no command is empty");
    mc_api_dispatch_join("a\\b\"c+version", 1, &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "{\"a\\\\b\\\"c\":{\"STATUS\":") == got,
            "json batch escapes \\ and \" in a command name");

    mc_api_dispatch_text("summary", "summary", &s, t1, MC_API_REPLY_CAP);
    mc_api_dispatch_text("pools", "pools", &s, t2, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP, "CMD=summary|%sCMD=pools|%s", t1, t2);
    mc_api_dispatch_join("summary+pools+summary", 0, &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "text `summary+pools` is CMD=<name>| before each reply");
    snprintf(want, MC_API_REPLY_CAP,
            "CMD=summary|%sCMD=ascset|STATUS=E,When=4133982044,Code=45,"
            "Msg=Access denied to 'ascset' command,Description=AvalonMiner " AM_VER "|",
            t1);
    mc_api_dispatch_join("summary+ascset", 0, &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "text `summary+ascset` denies ascset");
    snprintf(want, MC_API_REPLY_CAP,
            "CMD=a\\,b\\=c\\\\d|STATUS=E,When=4133982044,Code=14,Msg=Invalid command|"
            "CMD=summary|%s",
            t1);
    mc_api_dispatch_join("a,b=c\\d+summary", 0, &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "text batch escapes , = and \\ in a command name");
    mc_api_dispatch("{\"command\":\"estats\",\"parameter\":\"all\"}", "estats", &s, got,
            MC_API_REPLY_CAP);
    CHECK(sj_ok(got) && sj_key(got, "CF0_0"),
            "json estats \"parameter\":\"all\" selects the same block");
    jreq(req, MC_API_REPLY_CAP, "estats");
    mc_api_dispatch(req, "estats", &s, alt, MC_API_REPLY_CAP);
    CHECK(sj_ok(alt) && !sj_key(alt, "CF0_0"), "...which plain json `estats` omits");

    jreq(req, MC_API_REPLY_CAP, "lockstats");
    mc_api_dispatch(req, "lockstats", &s, got, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP,
            "{\"STATUS\":[{\"STATUS\":\"W\",\"When\":4133982044,\"Code\":124,"
            "\"Msg\":\"Lock stats not enabled\","
            "\"Description\":\"AvalonMiner " AM_VER "\"}],\"id\":1}");
    CHECK(!strcmp(got, want), "json `lockstats` is the whole WARN/124 stub record");
    mc_api_dispatch_text("lockstats", "lockstats", &s, got, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP,
            "STATUS=W,When=4133982044,Code=124,Msg=Lock stats not enabled,"
            "Description=AvalonMiner " AM_VER "|");
    CHECK(!strcmp(got, want), "text `lockstats` is the whole WARN/124 stub record");
    mc_api_dispatch_text("check|lockstats", "check", &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "Exists=Y,Access=Y") != NULL,
            "text `check|lockstats` reports the command exists");

    jreq(req, MC_API_REPLY_CAP, "nosuchcmd");
    mc_api_dispatch(req, "nosuchcmd", &s, got, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP,
            "{\"STATUS\":[{\"STATUS\":\"E\",\"When\":4133982044,\"Code\":14,"
            "\"Msg\":\"Invalid command\"}],\"id\":1}");
    CHECK(!strcmp(got, want), "json refusal is the whole unknown-command record");
    mc_api_dispatch_text("nosuchcmd", "nosuchcmd", &s, got, MC_API_REPLY_CAP);
    snprintf(want, MC_API_REPLY_CAP, "STATUS=E,When=4133982044,Code=14,Msg=Invalid command|");
    CHECK(!strcmp(got, want), "text refusal is the whole Invalid command record");
    CHECK(strstr(got, "Invalid command") != NULL, "...and the two modes agree on the wording");

    {
        static const char *const misses[] = {
            "",
            "ver",
            "versions",
            "summar",
            "summarys",
            "dev",
            "devss",
            "pool",
            "poolss",
            "coins",
            "configs",
            "stat",
            "statss",
            "estat",
            "estatss",
            "litestat",
            "asc",
            "ascsets",
            "times",
        };
        int refused_json = 0, refused_text = 0;
        for (size_t i = 0; i < sizeof misses / sizeof misses[0]; i++) {
            jreq(req, MC_API_REPLY_CAP, misses[i]);
            mc_api_dispatch(req, misses[i], &s, got, MC_API_REPLY_CAP);
            if (strstr(got, "Invalid command"))
                refused_json++;
            mc_api_dispatch_text(misses[i], misses[i], &s, got, MC_API_REPLY_CAP);
            if (strstr(got, "Msg=Invalid command"))
                refused_text++;
        }
        CHECK(refused_json == (int)(sizeof misses / sizeof misses[0]),
                "every near-miss name is refused in json (%d/%d)", refused_json,
                (int)(sizeof misses / sizeof misses[0]));
        CHECK(refused_text == (int)(sizeof misses / sizeof misses[0]),
                "every near-miss name is refused in text (%d/%d)", refused_text,
                (int)(sizeof misses / sizeof misses[0]));
    }

    mc_api_dispatch("{\"command\":\"ascset\"}", "ascset", &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "\"Code\":120") &&
                    strstr(got, "\"Msg\":\"ASC 0 set failed: missing subcommand\""),
            "json `ascset` reaches the setter runner, not the refusal");
    mc_api_dispatch_text("ascset", "ascset", &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "Code=120") && strstr(got, "Msg=ASC 0 set failed: missing subcommand"),
            "text `ascset` reaches the setter runner too");
    mc_api_dispatch("{\"command\":\"time\"}", "time", &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "\"Code\":130") && strstr(got, "\"Msg\":\"Time parameter is not available\""),
            "json `time` reaches the time runner, not the refusal");
    mc_api_dispatch_text("time", "time", &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "Code=130") && strstr(got, "Msg=Time parameter is not available"),
            "text `time` reaches the time runner too");

    {
        char cmd[32];
        mc_api_parse_cmd("VERSION", cmd, sizeof cmd);
        CHECK(!strcmp(cmd, "version"), "a text command name is folded to lower case");
        mc_api_parse_cmd("{\"command\":\"SuMMaRy\"}", cmd, sizeof cmd);
        CHECK(!strcmp(cmd, "summary"), "a json command name is folded too");
        mc_api_parse_cmd("stats|0", cmd, sizeof cmd);
        CHECK(!strcmp(cmd, "stats"), "the |parameter tail is not part of the name");
        CHECK(mc_api_estats_all("estats|ALL") == 0,
                "the `all` PARAMETER is case-SENSITIVE — |ALL is not the extended set");
        CHECK(mc_api_estats_all("estats|all") == 1, "...while |all is");
    }

    {
        char        cmd[MC_API_CMD_CAP], big[MC_API_CMD_CAP + 64];
        const char *all = "version+config+devs+edevs+pools+summary+devdetails+stats+estats"
                          "+litestats+coin+lcd+lockstats";
        snprintf(req, MC_API_REPLY_CAP, "{\"command\":\"%s\"}", all);
        CHECK(mc_api_parse_cmd(req, cmd, sizeof cmd) == strlen(all) && !strcmp(cmd, all),
                "json: every joinable command joined once parses whole");
        CHECK(mc_api_parse_cmd(all, cmd, sizeof cmd) == strlen(all) && !strcmp(cmd, all),
                "text: every joinable command joined once parses whole");
        jreq(req, MC_API_REPLY_CAP, "summary+pools+devs+stats+estats");
        mc_api_parse_cmd(req, cmd, sizeof cmd);
        mc_api_dispatch(req, cmd, &s, got, MC_API_REPLY_CAP);
        CHECK(strstr(got, "{\"summary\":{") == got && strstr(got, ",\"pools\":{") &&
                        strstr(got, ",\"devs\":{") && strstr(got, ",\"stats\":{") &&
                        strstr(got, ",\"estats\":{\"STATUS\":[{\"STATUS\":\"S\"") &&
                        !strcmp(got + strlen(got) - 9, "},\"id\":1}"),
                "json `summary+pools+devs+stats+estats` answers all five");
        mc_api_parse_cmd("summary+pools+devs+stats+estats", cmd, sizeof cmd);
        mc_api_dispatch_join(cmd, 0, &s, got, MC_API_REPLY_CAP);
        CHECK(strstr(got, "CMD=summary|") == got && strstr(got, "|CMD=estats|STATUS=S,"),
                "text `summary+pools+devs+stats+estats` answers all five");
        size_t n = 0;
        while (n + 8 < sizeof big) {
            memcpy(big + n, "summary+", 8);
            n += 8;
        }
        memcpy(big + n, "pools", 6);
        memset(cmd, 'x', sizeof cmd);
        CHECK(mc_api_parse_cmd(big, cmd, sizeof cmd) == 0 && !cmd[0],
                "text: a command string over the cap parses to no command");
        mc_api_dispatch_text(big, cmd, &s, got, MC_API_REPLY_CAP);
        CHECK(!strcmp(got, "STATUS=E,When=4133982044,Code=14,Msg=Invalid command|"),
                "...and is refused, not answered as a cut batch");
        snprintf(req, MC_API_REPLY_CAP, "{\"command\":\"%s\"}", big);
        CHECK(mc_api_parse_cmd(req, cmd, sizeof cmd) == 0 && !cmd[0],
                "json: a command string over the cap parses to no command");
        mc_api_dispatch(req, cmd, &s, got, MC_API_REPLY_CAP);
        CHECK(!strcmp(got,
                      "{\"STATUS\":[{\"STATUS\":\"E\",\"When\":4133982044,\"Code\":14,"
                      "\"Msg\":\"Invalid command\"}],\"id\":1}"),
                "...and is refused, not answered as a cut batch");
    }

    {
        jreq(req, MC_API_REPLY_CAP, "VeRsIoN");
        mc_api_dispatch(req, "VeRsIoN", &s, got, MC_API_REPLY_CAP);
        mc_api_version(&s, want, MC_API_REPLY_CAP);
        CHECK(!strcmp(got, want), "json `VeRsIoN` reaches the version builder");
        mc_api_dispatch_text("VeRsIoN", "VeRsIoN", &s, got, MC_API_REPLY_CAP);
        mc_api_version_text(&s, want, MC_API_REPLY_CAP);
        CHECK(!strcmp(got, want), "text `VeRsIoN` reaches the version builder");
        mc_api_dispatch("{\"command\":\"check\",\"parameter\":\"VERSION\"}", "check", &s, got,
                MC_API_REPLY_CAP);
        CHECK(strstr(got, "\"Exists\":\"Y\"") != NULL,
                "json `check|VERSION` reports the command exists");
        mc_api_dispatch_text("check|VERSION", "check", &s, got, MC_API_REPLY_CAP);
        CHECK(strstr(got, "Exists=Y,Access=Y") != NULL,
                "text `check|VERSION` reports the command exists");
        mc_api_dispatch("{\"command\":\"check\",\"parameter\":\"NoSuchCmd\"}", "check", &s, got,
                MC_API_REPLY_CAP);
        CHECK(strstr(got, "\"Exists\":\"N\"") != NULL,
                "json `check|NoSuchCmd` still reports absent");
        mc_api_dispatch_text("check|NoSuchCmd", "check", &s, got, MC_API_REPLY_CAP);
        CHECK(strstr(got, "Exists=N,Access=N") != NULL,
                "text `check|NoSuchCmd` still reports absent");
    }

    mc_api_command_reset();
    CHECK(mc_api_command_register("version", stub_cmd, NULL) == 0,
            "a handler may be registered under a builtin's name");
    CHECK(mc_api_command_register("ascset", stub_cmd, NULL) == 0, "...including ascset");
    jreq(req, MC_API_REPLY_CAP, "version");
    mc_api_dispatch(req, "version", &s, got, MC_API_REPLY_CAP);
    mc_api_version(&s, want, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "json `version` is still the builtin, not the handler");
    mc_api_dispatch_text("version", "version", &s, got, MC_API_REPLY_CAP);
    mc_api_version_text(&s, want, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, want), "text `version` is still the builtin");
    mc_api_dispatch("{\"command\":\"ascset\"}", "ascset", &s, got, MC_API_REPLY_CAP);
    CHECK(strcmp(got, "STUB-HANDLER-RAN") != 0 && strstr(got, "ASC 0 set failed:") != NULL,
            "json `ascset` is still the setter seam, not the handler");
    mc_api_dispatch_text("ascset", "ascset", &s, got, MC_API_REPLY_CAP);
    CHECK(strcmp(got, "STUB-HANDLER-RAN") != 0 && strstr(got, "ASC 0 set failed:") != NULL,
            "text `ascset` is still the setter seam");
    CHECK(mc_api_command_register("stubcmd", stub_cmd, NULL) == 0, "register an unshadowed name");
    mc_api_dispatch("{\"command\":\"stubcmd\"}", "stubcmd", &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, "STUB-HANDLER-RAN"), "an unshadowed name does reach the handler (json)");
    mc_api_dispatch_text("stubcmd", "stubcmd", &s, got, MC_API_REPLY_CAP);
    CHECK(!strcmp(got, "STUB-HANDLER-RAN"), "an unshadowed name does reach the handler (text)");

    mc_api_command_reset();
    CHECK(mc_api_command_register("BoardCmd", stub_cmd, NULL) == 0,
            "a board may register a name that is not all lower case");
    CHECK(mc_api_command_register("BOARDCMD", stub_cmd, NULL) == -1,
            "...and a second spelling of it is refused, not silently shadowed");
    {
        static const char *const spellings[] = { "BoardCmd", "boardcmd", "BOARDCMD" };
        for (size_t i = 0; i < sizeof spellings / sizeof spellings[0]; i++) {
            jreq(req, MC_API_REPLY_CAP, spellings[i]);
            mc_api_dispatch(req, spellings[i], &s, got, MC_API_REPLY_CAP);
            CHECK(!strcmp(got, "STUB-HANDLER-RAN"), "json `%s` reaches the registered handler",
                    spellings[i]);
            mc_api_dispatch_text(spellings[i], spellings[i], &s, got, MC_API_REPLY_CAP);
            CHECK(!strcmp(got, "STUB-HANDLER-RAN"), "text `%s` reaches the registered handler",
                    spellings[i]);
            snprintf(req, MC_API_REPLY_CAP, "{\"command\":\"check\",\"parameter\":\"%s\"}",
                    spellings[i]);
            mc_api_dispatch(req, "check", &s, got, MC_API_REPLY_CAP);
            CHECK(strstr(got, "\"Exists\":\"Y\"") != NULL, "json `check|%s` reports it exists",
                    spellings[i]);
            snprintf(req, MC_API_REPLY_CAP, "check|%s", spellings[i]);
            mc_api_dispatch_text(req, "check", &s, got, MC_API_REPLY_CAP);
            CHECK(strstr(got, "Exists=Y,Access=N") != NULL,
                    "text `check|%s` reports it exists, write access off the trusted channel",
                    spellings[i]);
            mc_api_set_peer_trusted(1);
            mc_api_dispatch_text(req, "check", &s, got, MC_API_REPLY_CAP);
            CHECK(strstr(got, "Exists=Y,Access=Y") != NULL,
                    "text `check|%s` grants access on the trusted channel", spellings[i]);
            mc_api_set_peer_trusted(0);
        }
    }

    mc_api_command_reset();
    mc_api_qr_login_begin("12345678abcdef0123456789abcdef01");
    mc_api_qr_login_approve("12345678");
    mc_api_set_peer_trusted(1);
    mc_api_dispatch_text("qrlogin", "qrlogin", &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "Msg=Invalid command") != NULL,
            "text `qrlogin` is refused even on the trusted channel (no text branch)");
    mc_api_dispatch(
            "{\"command\":\"qrlogin\",\"parameter\":\"claim,12345678abcdef0123456789abcdef01\"}",
            "qrlogin", &s, got, MC_API_REPLY_CAP);
    CHECK(strstr(got, "\"Approved\":1") != NULL,
            "...and the refused text attempt did not consume the approval");

    mc_api_command_reset();
    {
        int  ok = 0;
        char nm[20][8];
        for (int i = 0; i < 20; i++) {
            snprintf(nm[i], sizeof nm[i], "cap%d", i);
            if (mc_api_command_register(nm[i], stub_cmd, NULL) == 0)
                ok++;
        }
        CHECK(ok == 20, "the top-level registry takes exactly 20 commands (got %d)", ok);
        CHECK(mc_api_command_register("cap20", stub_cmd, NULL) == -1,
                "a 21st top-level command is REFUSED, not silently swapped in");
        CHECK(mc_api_command_lookup("cap20", NULL) == NULL, "...so it is absent from the registry");
        mc_api_dispatch("{\"command\":\"cap20\"}", "cap20", &s, got, MC_API_REPLY_CAP);
        CHECK(strstr(got, "Invalid command") != NULL,
                "...and the LAN sees it as a command that does not exist");
        CHECK(mc_api_command_lookup("cap19", NULL) != NULL, "the 20 that fitted are unaffected");
    }
    mc_api_command_reset();
    CHECK(mc_api_command_register("dup", stub_cmd, NULL) == 0, "register `dup` once");
    CHECK(mc_api_command_register("dup", stub_cmd, NULL) == -1,
            "a duplicate name is refused on an otherwise empty table");
    CHECK(mc_api_command_register("thisnameistwentyfourch__", stub_cmd, NULL) == -1,
            "a 24-byte name does not fit the 24-byte field, and is refused");
    CHECK(mc_api_command_register("thisnameis23charslong__", stub_cmd, NULL) == 0,
            "...while 23 bytes does fit");

    mc_api_command_reset();
    for (int i = 0; i < NBUF; i++)
        free(B[i]);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
