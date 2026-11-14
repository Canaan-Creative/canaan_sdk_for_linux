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
#include "mc_api_command.h"
#include "mc_api_qrlogin.h"
#include "mc_api_setter.h"
#include "tst_stats_json.h"
#include <stdio.h>
#include <string.h>
#include "../../tests/include/tst_check.h"

static size_t stub_cmd(const char *req, const mc_api_stats *s, int is_json, char *buf, size_t cap,
        void *ctx)
{
    (void)s;
    return (size_t)snprintf(buf, cap, "%s:%s:%s", is_json ? "json" : "text", (const char *)ctx,
            req);
}

static int stub_reads(const char *args)
{
    return !*args || !strcmp(args, "q");
}

int main(void)
{
    char         cmd[32], buf[2048], stats[2048], estats[2048];
    mc_api_stats s       = { 0 };
    s.prod               = "Avalon Nano3s";
    s.model              = "Nano3s";
    s.hwtype             = "H";
    s.swtype             = "S";
    s.version            = "v";
    s.dna                = "d";
    s.mac                = "m";
    s.asc_count          = 1;
    s.pool_count         = 0;
    s.telem.ghs_spd_chip = 5892.96;

    CHECK(mc_api_parse_cmd("{\"command\":\"version\"}", cmd, sizeof cmd) && !strcmp(cmd, "version"),
            "parse json");
    CHECK(mc_api_parse_cmd("summary", cmd, sizeof cmd) && !strcmp(cmd, "summary"), "parse legacy");
    CHECK(mc_api_parse_cmd("stats|0", cmd, sizeof cmd) && !strcmp(cmd, "stats"),
            "parse legacy+param");

    mc_api_dispatch("{\"command\":\"version\"}", "version", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"PROD\":\"Avalon Nano3s\""), "dispatch version");
    mc_api_dispatch_text("version", "version", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=S") && strstr(buf, "|VERSION,"), "text version behavior preserved");

    mc_api_dispatch("{\"command\":\"stats\"}", "stats", &s, stats, sizeof stats);
    mc_api_dispatch("{\"command\":\"estats\"}", "estats", &s, estats, sizeof estats);
    mc_api_dispatch("{\"command\":\"litestats\"}", "litestats", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":[{\"STATUS\":\"S\""), "json litestats -> STATUS S");
    CHECK(strstr(buf, "\"STATS\":[") && sj_ok(buf),
            "json litestats carries STATS with the MMID0Summary object");
    CHECK(sj_has(buf, "GHSspd[5892.960000]"), "json litestats GHSspd at 6dp");
    CHECK(strcmp(buf, estats) != 0 && !strstr(buf, "SYSTEMSTATU"),
            "json litestats is the compact branch, not estats");

    mc_api_dispatch_text("stats", "stats", &s, stats, sizeof stats);
    mc_api_dispatch_text("estats", "estats", &s, estats, sizeof estats);
    mc_api_dispatch_text("litestats", "litestats", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=S") && strstr(buf, "GHSspd[5892.960000]"),
            "text litestats -> STATUS S with GHSspd at 6dp");
    CHECK(strcmp(buf, estats) != 0 && !strstr(buf, "SYSTEMSTATU"),
            "text litestats is the compact branch, not estats");

    snprintf(s.ssid, sizeof s.ssid, "%s", "a,b|c]d");
    mc_api_dispatch_text("litestats", "litestats", &s, buf, sizeof buf);
    {
        int bars = 0;
        for (const char *q = buf; *q; q++)
            bars += *q == '|';
        CHECK(strstr(buf, "SSID[a?b?c]d] RSSI[") && bars == 2,
                "text litestats SSID has ',' and '|' replaced, ']' kept");
    }
    mc_api_dispatch("{\"command\":\"litestats\"}", "litestats", &s, buf, sizeof buf);
    CHECK(sj_has(buf, "SSID[a,b|c]d]"), "json litestats SSID is unchanged");
    s.ssid[0] = '\0';

    mc_api_dispatch("ascset", "ascset", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":\"E\""), "dispatch setter (no param) -> error");
    mc_api_dispatch("bogus", "bogus", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":\"E\""), "dispatch unknown -> error");
    mc_api_dispatch_text("bogus", "bogus", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=14") && strstr(buf, "Msg=Invalid command"),
            "text unknown -> STATUS E / Code 14 / Invalid command");

    mc_api_set_peer_trusted(1);
    mc_api_dispatch(
            "{\"command\":\"qrlogin\",\"parameter\":\"begin,12345678abcdef0123456789abcdef01\"}",
            "qrlogin", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Started\":1"), "the trusted channel registers the browser session");
    mc_api_qr_login_approve("12345678");
    mc_api_set_peer_trusted(0);
    char unknown_reply[512];
    mc_api_dispatch("bogus", "bogus", &s, unknown_reply, sizeof unknown_reply);
    mc_api_dispatch(
            "{\"command\":\"qrlogin\",\"parameter\":\"claim,12345678abcdef0123456789abcdef01\"}",
            "qrlogin", &s, buf, sizeof buf);
    CHECK(!strcmp(buf, unknown_reply), "remote qrlogin is indistinguishable from unknown command");
    CHECK(strstr(buf, "\"STATUS\":\"E\"") && !strstr(buf, "QRLOGIN"),
            "qrlogin off the trusted channel -> unknown command, no hint it exists");

    mc_api_set_peer_trusted(1);
    mc_api_dispatch(
            "{\"command\":\"qrlogin\",\"parameter\":\"claim,12345678abcdef0123456789abcdef01\"}",
            "qrlogin", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Approved\":1"), "... and the approval survived the refusal");
    mc_api_dispatch(
            "{\"command\":\"qrlogin\",\"parameter\":\"claim,12345678abcdef0123456789abcdef01\"}",
            "qrlogin", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"Approved\":0"), "qrlogin is read-and-clear");
    mc_api_set_peer_trusted(0);

    mc_api_command_reset();
    CHECK(mc_api_command_register("stubcmd", stub_cmd, (void *)"C") == 0,
            "top-level command registered");
    mc_api_dispatch_text("stubcmd|x", "stubcmd", &s, buf, sizeof buf);
    CHECK(!strcmp(buf, "text:C:stubcmd|x"), "text dispatch -> registry, is_json=0");
    mc_api_dispatch("{\"command\":\"stubcmd\"}", "stubcmd", &s, buf, sizeof buf);
    CHECK(!strcmp(buf, "json:C:{\"command\":\"stubcmd\"}"),
            "json dispatch -> the same handler, is_json=1");

    {
        static const struct {
            const char *req, *cmd;
        } reads[] = {
            { "version", "version" },
            { "summary", "summary" },
            { "{\"command\":\"pools\"}", "pools" },
            { "estats|all", "estats" },
            { "check|stubcmd", "check" },
            { "ascset|0,help", "ascset" },
            { "{\"command\":\"ascset\",\"parameter\":\"0,help\"}", "ascset" },
            { "ascset|0,plug-loop", "ascset" },
            { "ascset|0,plug-loop,1", "ascset" },
            { "ascset|0,plug-read", "ascset" },
            { "ascset|0,plug-read,", "ascset" },
            { "{\"command\":\"ascset\",\"parameter\":\"0,plug-read\"}", "ascset" },
            { "ascset|0,plug-read,q", "ascset" },
            { "{\"command\":\"ascset\",\"parameter\":\"0,PLUG-READ,q\"}", "ascset" },
            { "time|get", "time" },
            { "{\"command\":\"time\",\"parameter\":\"get\"}", "time" },
            { "time", "time" },
            { "{\"command\":\"time\"}", "time" },
            { "time|getx", "time" },
            { "time|sync,t:1787102966,UTC", "time" },
            { "nosuch", "nosuch" },
        };
        static const struct {
            const char *req, *cmd;
        } writes[] = {
            { "stubcmd|x", "stubcmd" },
            { "{\"command\":\"StubCmd\"}", "StubCmd" },
            { "{\"command\":\"qrlogin\",\"parameter\":\"claim,x\"}", "qrlogin" },
            { "ascset", "ascset" },
            { "ascset|0", "ascset" },
            { "ascset|0,reboot,0", "ascset" },
            { "{\"command\":\"ascset\",\"parameter\":\"0,reboot_web\"}", "ascset" },
            { "ascset|0,plug-read,1200", "ascset" },
            { "ascset|0,plug-read,q,x", "ascset" },
            { "ascset|0,plug-write", "ascset" },
            { "ascset|0,undeclared", "ascset" },
            { "ascset|0,password,a,b", "ascset" },
            { "ascset|0,wallpaper,none", "ascset" },
            { "ascset|0,qr_auth,12345678,ff", "ascset" },
            { "{\"command\":\"ascset\",\"parameter\":\"0,workmode,1\"}", "ascset" },
            { "time|set,t:1787102966,UTC", "time" },
            { "{\"command\":\"time\",\"parameter\":\"set,t:1787102966,UTC\"}", "time" },
            { "time|set", "time" },
        };
        CHECK(mc_api_setter_declare("plug-loop", 0, NULL, mc_api_setter_always_reads) == 0 &&
                        mc_api_setter_declare("plug-read", 0, NULL, stub_reads) == 0 &&
                        mc_api_setter_declare("plug-write", 0, NULL, NULL) == 0,
                "declare a setting that always reads, one that reads by its arguments and one "
                "that always writes");
        for (size_t i = 0; i < sizeof reads / sizeof reads[0]; i++)
            CHECK(!mc_api_cmd_writes(reads[i].req, reads[i].cmd), "`%s` only reads", reads[i].req);
        for (size_t i = 0; i < sizeof writes / sizeof writes[0]; i++)
            CHECK(mc_api_cmd_writes(writes[i].req, writes[i].cmd), "`%s` writes", writes[i].req);

        mc_api_dispatch_text("check|stubcmd", "check", &s, buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=N"),
                "text check of a write command off the trusted channel -> Access=N");
        mc_api_dispatch("{\"command\":\"check\",\"parameter\":\"stubcmd\"}", "check", &s, buf,
                sizeof buf);
        CHECK(strstr(buf, "\"Exists\":\"Y\",\"Access\":\"N\""),
                "json check of a write command off the trusted channel -> Access N");
        mc_api_dispatch_text("check|version", "check", &s, buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=Y"), "check of a read command -> Access=Y");
        mc_api_dispatch_text("check|ascset", "check", &s, buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=Y"), "ascset keeps its read settings -> Access=Y");
        mc_api_dispatch_text("check|qrlogin", "check", &s, buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=N"),
                "check of qrlogin off the trusted channel -> Exists=Y, Access=N");
        mc_api_set_peer_trusted(1);
        mc_api_dispatch_text("check|qrlogin", "check", &s, buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=Y"),
                "check of qrlogin on the trusted channel -> Exists=Y, Access=Y");
        mc_api_dispatch_text("check|stubcmd", "check", &s, buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=Y"), "check on the trusted channel -> Access=Y");
        mc_api_dispatch("{\"command\":\"check\",\"parameter\":\"stubcmd\"}", "check", &s, buf,
                sizeof buf);
        CHECK(strstr(buf, "\"Exists\":\"Y\",\"Access\":\"Y\""),
                "json check on the trusted channel -> Access Y");
        mc_api_set_peer_trusted(0);
    }

    {
        static const char *const http[] = {
            "POST / HTTP/1.1\r\nHost: 10.0.0.2:4028\r\nContent-Type: text/plain\r\n\r\n"
            "\"command\":\"stubcmd|0,reboot,0",
            "POST / HTTP/1.1\r\n\r\n{\"command\":\"stubcmd\"}",
            "GET /?{\"command\":\"stubcmd\"} HTTP/1.1\r\n\r\n",
            "  OPTIONS / HTTP/1.1\r\n\r\n\"command\":\"stubcmd\"",
        };
        for (size_t i = 0; i < sizeof http / sizeof http[0]; i++) {
            memset(cmd, 'x', sizeof cmd);
            CHECK(mc_api_parse_cmd(http[i], cmd, sizeof cmd) == 0 && !cmd[0],
                    "HTTP-wrapped request %zu parses to no command", i);
            mc_api_dispatch_text(http[i], cmd, &s, buf, sizeof buf);
            CHECK(strstr(buf, "Msg=Invalid command") && !strstr(buf, "text:C:"),
                    "HTTP-wrapped request %zu is refused and the handler does not run", i);
        }
    }
    CHECK(mc_api_parse_cmd("bogus \"command\":\"stubcmd\"", cmd, sizeof cmd) &&
                    !strcmp(cmd, "bogus "),
            "text request: \"command\" past the first byte is not a command");
    CHECK(mc_api_parse_cmd(" \t\r\n{\"command\":\"stubcmd\"}", cmd, sizeof cmd) &&
                    !strcmp(cmd, "stubcmd"),
            "JSON after leading whitespace parses");
    CHECK(mc_api_parse_cmd("{\"parameter\":\"0,x\",\"command\":\"ascset\"}", cmd, sizeof cmd) &&
                    !strcmp(cmd, "ascset"),
            "JSON with \"command\" after another key parses");
    CHECK(mc_api_parse_cmd("ascset|0,ledmode,1", cmd, sizeof cmd) && !strcmp(cmd, "ascset"),
            "text ascset parses");
    CHECK(mc_api_parse_cmd(" summary", cmd, sizeof cmd) && !strcmp(cmd, "summary"),
            "text command after a leading space parses");
    CHECK(mc_api_parse_cmd("\nsummary", cmd, sizeof cmd) && !strcmp(cmd, "summary"),
            "text command after a leading newline parses");
    CHECK(mc_api_parse_cmd(" \t\r\nascset|0,ledmode,1", cmd, sizeof cmd) && !strcmp(cmd, "ascset"),
            "text ascset after leading whitespace parses");
    mc_api_parse_cmd(" summary", cmd, sizeof cmd);
    mc_api_dispatch_text(" summary", cmd, &s, buf, sizeof buf);
    CHECK(!strstr(buf, "Invalid command"), "text command after a leading space is accepted");
    CHECK(mc_api_parse_cmd("posting", cmd, sizeof cmd) && !strcmp(cmd, "posting"),
            "a text command that begins like a method name is not an HTTP request");
    mc_api_dispatch("{\"command\":\"nosuch\"}", "nosuch", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Invalid command") != NULL,
            "an unregistered json command still gets the unknown-command reply");

    mc_api_status_reply(0, buf, sizeof buf, 7, 'E', 120, "nope");
    CHECK(!strcmp(buf, "STATUS=E,When=7,Code=120,Msg=nope,Description=AvalonMiner " AM_VER "|"),
            "mc_api_status_reply(0) == the text STATUS record");
    {
        const char *p    = strstr(buf, ",Description=AvalonMiner ");
        int         dots = 0, digits = 0, bad = 0;
        if (p) {
            p += sizeof ",Description=AvalonMiner " - 1;
            for (; *p && *p != '|'; p++) {
                if (*p >= '0' && *p <= '9')
                    digits++;
                else if (*p == '.' && digits) {
                    dots++;
                    digits = 0;
                } else
                    bad = 1;
            }
        }
        CHECK(p && !bad && dots == 2 && digits > 0 && *p == '|',
                "Description carries `AvalonMiner <major>.<minor>.<patch>` — asserted "
                "by SHAPE, with AM_VER interpolated nowhere. Every other Description "
                "assertion in this suite pastes the macro into its expectation, so "
                "redefining it moves expectation and reality together and changes "
                "nothing; this one fails when the field stops being a bare semver");
    }
    mc_api_status_reply(1, buf, sizeof buf, 7, 'S', 119, "yep");
    CHECK(!strcmp(buf,
                  "{\"STATUS\":[{\"STATUS\":\"S\",\"When\":7,\"Code\":119,"
                  "\"Msg\":\"yep\",\"Description\":\"AvalonMiner " AM_VER "\"}],\"id\":1}"),
            "mc_api_status_reply(1) == the JSON STATUS envelope");

    mc_api_status_reply(1, buf, sizeof buf, 7, 'E', 120, "say \"hi\"\tnow\\then");
    CHECK(!strcmp(buf,
                  "{\"STATUS\":[{\"STATUS\":\"E\",\"When\":7,\"Code\":120,"
                  "\"Msg\":\"say \\\"hi\\\"\\u0009now\\\\then\","
                  "\"Description\":\"AvalonMiner " AM_VER "\"}],\"id\":1}"),
            "json Msg escapes quote, backslash and control chars");

    {
        enum { PAIRS = (MC_API_MSG_CAP - 1) / 2, UESC = (MC_API_MSG_CAP - 1) / 6 };
        char   many[MC_API_MSG_CAP + 1], want[2 * MC_API_MSG_CAP];
        size_t w;
        memset(many, '"', MC_API_MSG_CAP);
        many[MC_API_MSG_CAP] = 0;
        w                    = (size_t)snprintf(want, sizeof want, "\"Msg\":\"");
        for (int i = 0; i < PAIRS; i++)
            w += (size_t)snprintf(want + w, sizeof want - w, "\\\"");
        snprintf(want + w, sizeof want - w, "\",\"Description\"");
        mc_api_status_reply(1, buf, sizeof buf, 7, 'E', 120, many);
        CHECK(strstr(buf, want) != NULL, "json Msg cut between two-char escapes");

        memset(many, '\t', UESC + 10);
        many[UESC + 10] = 0;
        w               = (size_t)snprintf(want, sizeof want, "\"Msg\":\"");
        for (int i = 0; i < UESC; i++)
            w += (size_t)snprintf(want + w, sizeof want - w, "\\u0009");
        snprintf(want + w, sizeof want - w, "\",\"Description\"");
        mc_api_status_reply(1, buf, sizeof buf, 7, 'E', 120, many);
        CHECK(strstr(buf, want) != NULL, "json Msg cut between \\u escapes");
    }

    {
        static const char *const live_msgs[] = {
            "Invalid command",
            "ASC 0 set OK",
            "ASC 0 set info: led set ok",
            "ASC 0 set failed: Unknown option: nosuch",
            "Time parameter is not available",
            "Time set info: time unknown argument",
            "Time set failed: time out of range",
            "Time set failed: invalid timezone",
            "Time set failed: timezone not confirmed",
            "Time set failed: system clock not set",
            "Time set info: time t:1787102966 America/Los_Angeles",
            "we need 1 or 4 arguments",
            "invalid pool index",
            "invalid pool url",
            "The pool url length is too long",
            "The pool user length is too long",
            "The pool pass length is too long",
            "unknown error",
            "config write error",
            "success set to pool 0",
            "invalid softon setting",
            "invalid softoff setting",
            "control channel down",
            "success softon now",
            "success softoff at 1787102966",
            "success softon cancelled",
        };
        for (size_t i = 0; i < sizeof live_msgs / sizeof live_msgs[0]; i++) {
            char want[256];
            snprintf(want, sizeof want,
                    "STATUS=E,When=7,Code=120,Msg=%s,Description=AvalonMiner " AM_VER "|",
                    live_msgs[i]);
            mc_api_text_status(buf, sizeof buf, 7, 'E', 120, live_msgs[i]);
            CHECK(!strcmp(buf, want), "text Msg unchanged: \"%s\"", live_msgs[i]);
        }
        mc_api_text_status(buf, sizeof buf, 7, 'E', 120, "a,b|c\td\"e\\f");
        CHECK(!strcmp(buf,
                      "STATUS=E,When=7,Code=120,Msg=a?b?c?d?e?f,"
                      "Description=AvalonMiner " AM_VER "|"),
                "text Msg scrubs comma, pipe, control char, quote and backslash");
    }

    {
        char devs[2048];
        mc_api_dispatch_text("summary", "summary", &s, buf, sizeof buf);
        CHECK(strstr(buf, "STATUS=S,When=0,Code=11,Msg=Summary") &&
                        strstr(buf, "|SUMMARY,Elapsed=0,"),
                "text summary answers with the SUMMARY section");
        mc_api_dispatch_text("devs", "devs", &s, devs, sizeof devs);
        CHECK(strstr(devs, "STATUS=S,When=0,Code=9,Msg=1 ASC(s)") &&
                        strstr(devs, "|ASC=0,Name=AVALON,"),
                "text devs answers with the ASC-indexed section");
        mc_api_dispatch_text("edevs", "edevs", &s, buf, sizeof buf);
        CHECK(!strcmp(buf, devs) && strstr(buf, "|ASC=0,Name=AVALON,"),
                "text edevs shares the devs reply, and it IS a devs reply");
        CHECK(strstr(buf,
                      "Difficulty Rejected=0.00000000,"
                      "Last Share Difficulty=0.00000000,Last Valid Work=0,"
                      "Device Hardware%=0.0000"),
                "text edevs carries Last Share Difficulty + Last Valid Work in place");
        mc_api_dispatch_text("summary", "summary", &s, buf, sizeof buf);
        CHECK(strstr(buf,
                      "Total MH=0.0000,Work Utility=0.00,"
                      "Difficulty Accepted=0.00000000,"
                      "Difficulty Rejected=0.00000000,Difficulty Stale=0.00000000,"
                      "Best Share=0"),
                "text summary carries Work Utility + Difficulty Stale in place");
        mc_api_dispatch_text("coin", "coin", &s, buf, sizeof buf);
        CHECK(strstr(buf, "STATUS=S,When=0,Code=78,Msg=AvalonMiner coin") &&
                        strstr(buf, "|COIN,Hash Method=sha256,"),
                "text coin answers with the COIN section");
        mc_api_dispatch_text("config", "config", &s, buf, sizeof buf);
        CHECK(strstr(buf, "STATUS=S,When=0,Code=33,Msg=AvalonMiner config") &&
                        strstr(buf, "|CONFIG,ASC Count=1,"),
                "text config answers with the CONFIG section");
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
