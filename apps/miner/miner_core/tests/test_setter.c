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
#include "mc_api_setter.h"
#include <stdio.h>
#include <string.h>

#include "../../tests/include/tst_check.h"

#define OP_LEDSET 1 /* Setter op registered as ledset */
#define OP_FAIL   2 /* Setter op whose handler fails with MC_SET_ERR_HW */
#define OP_ECHO   3 /* Setter op whose handler echoes its argument */
#define OP_QUOTE  4 /* Setter op whose handler replies with a quoted string */
#define OP_DUP    5 /* Setter op registered as dup */

/* Op, call count and argument of the last fake_set() call */
static uint16_t seen_op;
static int      seen_n;
static char     seen_args[72];

/* Raw tail and ASC index of the last fake_raw() call */
static char raw_tail_seen[3000];
static int  raw_asc_seen;

static mc_set_result fake_set(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t cap, void *ctx)
{
    (void)s;
    (void)ctx;
    seen_op = op;
    seen_n++;
    snprintf(seen_args, sizeof seen_args, "%s", args);
    if (op == OP_FAIL) {
        snprintf(msg, cap, "hw refused");
        return MC_SET_ERR_HW;
    }
    snprintf(msg, cap, "op %u set", op);
    return MC_SET_OK;
}

static mc_set_result echo_set(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t cap, void *ctx)
{
    (void)s;
    (void)ctx;
    if (op == OP_QUOTE)
        snprintf(msg, cap, "he said \"hi\"");
    else
        snprintf(msg, cap, "set to %s", args);
    return MC_SET_INFO;
}
static mc_set_result fake_raw(const char *raw_tail, int asc, const mc_api_stats *s, char *msg,
        size_t cap, void *ctx)
{
    (void)s;
    (void)ctx;
    snprintf(raw_tail_seen, sizeof raw_tail_seen, "%s", raw_tail);
    raw_asc_seen = asc;
    snprintf(msg, cap, "raw ok %zu", strlen(raw_tail));
    return MC_SET_INFO;
}

int main(void)
{
    char         buf[2048];
    mc_api_stats s = { 0 };
    s.prod         = "Avalon Nano3s";
    s.version      = "v";
    s.dna          = "d";
    s.mac          = "m";

    mc_api_setter_reset();
    CHECK(mc_api_setter_register("ledset", OP_LEDSET, fake_set, NULL) == 0, "register ledset");
    CHECK(mc_api_setter_register("fail", OP_FAIL, fake_set, NULL) == 0, "register fail");
    CHECK(mc_api_setter_register("ledset", OP_LEDSET, fake_set, NULL) == -1, "duplicate rejected");
    CHECK(mc_api_setter_register(NULL, OP_LEDSET, fake_set, NULL) == -1, "null subcmd rejected");
    CHECK(mc_api_setter_register("LEDSET", OP_LEDSET, fake_set, NULL) == -1,
            "a case variant is the same name to the registry -> duplicate");

    char sub[24], args[72];
    int  asc = -1;
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":\"0,ledset,80\"}", sub,
                  sizeof sub, args, sizeof args, &asc) == 0 &&
                    !strcmp(sub, "ledset") && !strcmp(args, "80") && asc == 0,
            "parse json ascset");
    CHECK(mc_api_parse_ascset("ascset|0,ledset,80", sub, sizeof sub, args, sizeof args, NULL) ==
                            0 &&
                    !strcmp(sub, "ledset") && !strcmp(args, "80"),
            "parse text ascset");
    CHECK(mc_api_parse_ascset("ascset|0,led,3,7,on", sub, sizeof sub, args, sizeof args, NULL) ==
                            0 &&
                    !strcmp(sub, "led") && !strcmp(args, "3,7,on"),
            "parse args keep commas");
    CHECK(mc_api_parse_ascset("ascset|0", sub, sizeof sub, args, sizeof args, NULL) != 0,
            "parse index-only -> fail");

    CHECK(mc_api_parse_ascset("ascset|3,ledset,80", sub, sizeof sub, args, sizeof args, &asc) ==
                            0 &&
                    asc == 3,
            "asc index parsed");
    CHECK(mc_api_parse_ascset("ascset|x,ledset,80", sub, sizeof sub, args, sizeof args, &asc) ==
                            0 &&
                    asc == 0,
            "non-numeric asc index -> 0");
    CHECK(mc_api_parse_ascset("ascset|99999,ledset,80", sub, sizeof sub, args, sizeof args, &asc) ==
                            0 &&
                    asc == 0,
            "absurd asc index -> 0");

    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":\"0,ledset,80\nFAKE LOG\"}",
                  sub, sizeof sub, args, sizeof args, NULL) == 0 &&
                    !strcmp(sub, "ledset") && !strcmp(args, "80\nFAKE LOG"),
            "json args are the decoded string, a raw control char included");
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":\"0,ledset,80\\nFAKE\"}", sub,
                  sizeof sub, args, sizeof args, NULL) == 0 &&
                    !strcmp(args, "80\nFAKE"),
            "json escapes are decoded");
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":"
                              "\"0,ledset,a\\\\b\\\"c\\/d\\u00e9\\u4e2d\\ud83d\\ude00\"}",
                  sub, sizeof sub, args, sizeof args, NULL) == 0 &&
                    !strcmp(args, "a\\b\"c/d\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80"),
            "json backslash, quote, slash and unicode escapes are decoded to the raw bytes");
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":\"0,ledset,80\\u0000x\"}", sub,
                  sizeof sub, args, sizeof args, NULL) != 0,
            "json with an escaped NUL is refused");
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":\"0,ledset,80\"", sub,
                  sizeof sub, args, sizeof args, NULL) != 0,
            "malformed json is refused");
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":7}", sub, sizeof sub, args,
                  sizeof args, NULL) != 0,
            "a parameter that is not a json string is refused");
    CHECK(mc_api_parse_ascset("ascset|0,ledset,a\\b\"c", sub, sizeof sub, args, sizeof args,
                  NULL) == 0 &&
                    !strcmp(args, "a\\b\"c"),
            "text args keep backslash and quote as they are");
    CHECK(mc_api_parse_ascset("ascset|0,ledset,80\nFAKE LOG", sub, sizeof sub, args, sizeof args,
                  NULL) == 0 &&
                    !strcmp(args, "80"),
            "text args stop at control char");
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":\"0,led\\nset,80\"}", sub,
                  sizeof sub, args, sizeof args, NULL) != 0,
            "a subcommand with a control char is refused");

    char longreq[1024];
    snprintf(longreq, sizeof longreq, "ascset|0,ledset,%0*d", 90, 7);
    CHECK(mc_api_parse_ascset(longreq, sub, sizeof sub, args, sizeof args, NULL) == -2,
            "args over the caller cap -> -2, not a silent cut");
    snprintf(longreq, sizeof longreq, "ascset|0,%0*d,80", 40, 7);
    CHECK(mc_api_parse_ascset(longreq, sub, sizeof sub, args, sizeof args, NULL) == -2,
            "subcmd over the caller cap -> -2");
    char bigargs[520];
    snprintf(longreq, sizeof longreq, "ascset|0,password,%0*d,%0*d", 256, 1, 256, 2);
    CHECK(mc_api_parse_ascset(longreq, sub, sizeof sub, bigargs, sizeof bigargs, NULL) == 0 &&
                    strlen(bigargs) == 513,
            "513-char password args parse whole");

    snprintf(longreq, sizeof longreq, "ascset|0,ledset,%0*d", 600, 7);
    CHECK(mc_api_setter_run(longreq, &s, buf, sizeof buf) == MC_SET_OK,
            "a 600-byte arg applies, not refused as parameter too long");

    {
        char hugereq[6000];
        snprintf(hugereq, sizeof hugereq, "ascset|0,ledset,%0*d", 5000, 7);
        CHECK(mc_api_setter_run(hugereq, &s, buf, sizeof buf) == MC_SET_ERR_ARG &&
                        strstr(buf, "ASC 0 set failed: parameter too long"),
                "an arg far beyond any real request is still refused, not truncated");
    }

    {
        char toolong[64];
        memset(toolong, 'x', sizeof toolong - 1);
        toolong[sizeof toolong - 1] = 0;
        char req2[128];
        snprintf(req2, sizeof req2, "ascset|0,%s,1", toolong);
        CHECK(mc_api_setter_run(req2, &s, buf, sizeof buf) == MC_SET_ERR_UNSUP &&
                        strstr(buf, "Unknown option: xxxx"),
                "an over-long subcommand answers Unknown option, not parameter too long");
    }

    char tiny[32];
    memset(tiny, 'x', sizeof tiny);
    CHECK(mc_api_setter_run("ascset|0", &s, tiny, 8) == MC_SET_ERR_ARG && strlen(tiny) < 8,
            "short msgcap stays NUL-terminated");

    CHECK(mc_set_letter(MC_SET_OK) == 'S' && mc_set_code(MC_SET_OK) == 119, "applied -> S/119");
    CHECK(mc_set_letter(MC_SET_INFO) == 'I' && mc_set_code(MC_SET_INFO) == 118, "info -> I/118");
    CHECK(mc_set_letter(MC_SET_ERR_ARG) == 'E' && mc_set_code(MC_SET_ERR_ARG) == 120,
            "bad arg -> E/120");
    CHECK(mc_set_letter(MC_SET_ERR_UNSUP) == 'E' && mc_set_code(MC_SET_ERR_UNSUP) == 120,
            "unsupported -> E/120");
    CHECK(mc_set_letter(MC_SET_ERR_HW) == 'E' && mc_set_code(MC_SET_ERR_HW) == 120,
            "hw failure -> E/120");

    seen_op      = 0;
    seen_n       = 0;
    seen_args[0] = 0;
    mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,ledset,80\"}", "ascset", &s, buf,
            sizeof buf);
    CHECK(seen_op == OP_LEDSET && !strcmp(seen_args, "80"), "json routed to handler by op");
    CHECK(strstr(buf, "\"STATUS\":\"S\"") && strstr(buf, "\"Code\":119") &&
                    strstr(buf, "\"Msg\":\"ASC 0 set OK\""),
            "json applied -> S/119 + bare set-OK envelope");
    CHECK(!strstr(buf, "op 1 set"), "handler reason dropped on an applied write");

    mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"2,ledset,80\"}", "ascset", &s, buf,
            sizeof buf);
    CHECK(strstr(buf, "\"Msg\":\"ASC 2 set OK\""), "asc index echoed in the reply");

    mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,fail,1\"}", "ascset", &s, buf,
            sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":\"E\"") && strstr(buf, "\"Code\":120") &&
                    strstr(buf, "\"Msg\":\"ASC 0 set failed: hw refused\""),
            "json handler HW-fail -> E/120 + set-failed envelope");

    mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,softoff\"}", "ascset", &s, buf,
            sizeof buf);
    CHECK(strstr(buf, "\"Msg\":\"ASC 0 set failed: Unknown option: softoff\"") &&
                    strstr(buf, "\"Code\":120"),
            "json unknown subcmd -> API Unknown option");

    mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0\"}", "ascset", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":\"E\""), "json missing subcmd -> STATUS E");

    seen_op      = 0;
    seen_n       = 0;
    seen_args[0] = 0;
    mc_api_dispatch_text("ascset|0,ledset,80", "ascset", &s, buf, sizeof buf);
    CHECK(seen_op == OP_LEDSET && !strcmp(seen_args, "80"), "text routed to handler by op");
    CHECK(strstr(buf, "STATUS=S,When=0,Code=119,Msg=ASC 0 set OK,"),
            "text applied -> same letter/code/envelope as JSON");

    {
        char jlo[2048], jup[2048], tlo[2048], tup[2048];
        seen_op = 0;
        seen_n  = 0;
        mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,ledset,80\"}", "ascset", &s, jlo,
                sizeof jlo);
        uint16_t op_lo = seen_op;
        mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,LEDSET,80\"}", "ascset", &s, jup,
                sizeof jup);
        CHECK(seen_n == 2 && seen_op == OP_LEDSET && op_lo == OP_LEDSET,
                "json: LEDSET reaches the same handler with the same op as ledset");
        CHECK(!strcmp(jlo, jup), "json: the reply is byte-identical for either case");
        mc_api_dispatch_text("ascset|0,ledset,80", "ascset", &s, tlo, sizeof tlo);
        mc_api_dispatch_text("ascset|0,LedSet,80", "ascset", &s, tup, sizeof tup);
        CHECK(seen_n == 4 && seen_op == OP_LEDSET,
                "text: a mixed-case spelling reaches the same handler and op");
        CHECK(!strcmp(tlo, tup), "text: the reply is byte-identical for either case");
        mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,LEDSETX,80\"}", "ascset", &s,
                jup, sizeof jup);
        mc_api_dispatch_text("ascset|0,LEDSETX,80", "ascset", &s, tup, sizeof tup);
        CHECK(strstr(jup, "Unknown option: LEDSETX") && strstr(jup, "\"Code\":120") &&
                        strstr(tup, "Unknown option: LEDSETX") && strstr(tup, "Code=120"),
                "both ladders: case folding is not prefix/suffix tolerance");
    }

    {
        char hmsg[256];
        CHECK(mc_api_setter_run("ascset|0,HELP", &s, hmsg, sizeof hmsg) == MC_SET_INFO &&
                        strstr(hmsg, "ASC 0 set info: help|"),
                "help is matched case-insensitively too");
    }

    mc_api_dispatch_text("ascset|0,fail,1", "ascset", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=120"), "text handler HW-fail -> E/120");

    mc_api_dispatch_text("bogus", "bogus", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=E"), "text unsupported -> STATUS=E");

    CHECK(mc_api_setter_register("echo", OP_ECHO, echo_set, NULL) == 0, "register echo");
    CHECK(mc_api_setter_register("quote", OP_QUOTE, echo_set, NULL) == 0, "register quote");

    mc_api_dispatch_text("ascset|0,echo,3,7", "ascset", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Msg=ASC 0 set info: set to 3?7"),
            "comma in args cannot split the text record");

    mc_api_dispatch_text("ascset|0,echo,80\"X\\Y", "ascset", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Msg=ASC 0 set info: set to 80?X?Y"), "quote/backslash in args neutralized");

    mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,quote,x\"}", "ascset", &s, buf,
            sizeof buf);
    CHECK(strstr(buf, "\"Msg\":\"ASC 0 set info: he said ?hi?\"") &&
                    strstr(buf, "\"STATUS\":\"I\"") && strstr(buf, "\"Code\":118"),
            "handler-written quotes cannot break the JSON reply");

    mc_api_dispatch_text("ascset|0,no\"pe", "ascset", &s, buf, sizeof buf);
    CHECK(strstr(buf, "Msg=ASC 0 set failed: Unknown option: no?pe"),
            "unknown-subcmd reason is escaped");

    {
        char          hmsg[256];
        mc_set_result hr = mc_api_setter_run("{\"command\":\"ascset\",\"parameter\":\"0,help\"}",
                &s, hmsg, sizeof hmsg);
        CHECK(hr == MC_SET_INFO, "help returns MC_SET_INFO");
        CHECK(strstr(hmsg, "ASC 0 set info: help|"), "help msg has prefix");
        CHECK(strstr(hmsg, "ledset|"), "help lists ledset");
        CHECK(strstr(hmsg, "echo|"), "help lists echo");
        CHECK(strstr(hmsg, "quote|"), "help lists quote");
        CHECK(strstr(hmsg, "fail|"), "help lists fail");
    }
    mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,help\"}", "ascset", &s, buf,
            sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":\"I\"") && strstr(buf, "\"Code\":118"), "help json -> I/118");
    CHECK(strstr(buf, "ledset|"), "help json lists ledset with pipe");

    mc_api_dispatch_text("ascset|0,help", "ascset", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=I") && strstr(buf, "Code=118"), "help text -> I/118");
    CHECK(strstr(buf, "ledset?"), "help text pipes scrubbed to ? in text mode");

    CHECK(mc_api_setter_register_raw("blob", fake_raw, NULL) == 0, "register raw blob");
    CHECK(mc_api_setter_register_raw("blob", fake_raw, NULL) == -1, "duplicate raw rejected");
    {
        char hmsg[256];
        mc_api_setter_run("ascset|0,help", &s, hmsg, sizeof hmsg);
        CHECK(strstr(hmsg, "|blob|"), "help lists a raw setter too (nano3s lists wallpaper)");
    }

    {
        char tail[1600];
        int  p = snprintf(tail, sizeof tail, "png,");
        memset(tail + p, 'a', sizeof tail - 1 - p);
        tail[sizeof tail - 1] = 0;
        char rreq[1800];
        snprintf(rreq, sizeof rreq, "ascset|3,blob,%s", tail);
        raw_tail_seen[0] = 0;
        raw_asc_seen     = -1;
        mc_set_result rr = mc_api_setter_run(rreq, &s, buf, sizeof buf);
        CHECK(rr == MC_SET_INFO, "raw handler invoked, not -2 parameter too long");
        CHECK(!strcmp(raw_tail_seen, tail), "raw tail delivered whole, commas kept");
        CHECK(raw_asc_seen == 3, "raw asc index extracted");
        CHECK(strstr(buf, "ASC 3 set info: raw ok"), "raw reply enveloped with the asc index");
    }

    {
        char   tail[2200];
        size_t L = 2199;
        int    p = snprintf(tail, sizeof tail, "png,");
        memset(tail + p, 'a', L - (size_t)p);
        tail[L] = 0;
        char rreq[2400];
        snprintf(rreq, sizeof rreq, "ascset|5,blob,%s", tail);
        raw_tail_seen[0] = 0;
        raw_asc_seen     = -1;
        mc_set_result rr = mc_api_setter_run(rreq, &s, buf, sizeof buf);
        CHECK(rr == MC_SET_INFO,
                "a 2199-byte tail (the raw cap's limit) still reaches the raw handler");
        CHECK(!strcmp(raw_tail_seen, tail), "boundary-size tail delivered whole");
    }
    {
        char   tail[2300];
        size_t L = 2200;
        int    p = snprintf(tail, sizeof tail, "png,");
        memset(tail + p, 'a', L - (size_t)p);
        tail[L] = 0;
        char rreq[2500];
        snprintf(rreq, sizeof rreq, "ascset|4,blob,%s", tail);
        raw_tail_seen[0] = 0;
        raw_asc_seen     = -1;
        mc_set_result rr = mc_api_setter_run(rreq, &s, buf, sizeof buf);
        CHECK(rr == MC_SET_ERR_ARG && strstr(buf, "ASC 4 set failed: parameter too long"),
                "a 2200-byte tail (one over the raw cap) is refused, not silently truncated");
        CHECK(raw_tail_seen[0] == 0, "raw handler never invoked for the over-cap tail");
    }

    {
        char hex[2105];
        memset(hex, 'a', sizeof hex - 1);
        hex[sizeof hex - 1] = 0;
        char tail[2200];
        snprintf(tail, sizeof tail, "png,%s", hex);
        char rreq[2400];
        snprintf(rreq, sizeof rreq, "ascset|6,blob,%s", tail);
        raw_tail_seen[0] = 0;
        raw_asc_seen     = -1;
        mc_set_result rr = mc_api_setter_run(rreq, &s, buf, sizeof buf);
        CHECK(rr == MC_SET_INFO && !strcmp(raw_tail_seen, tail),
                "a real wallpaper-sized tail (WP_PKG_LEN=2104 hex + mode) reaches the raw handler "
                "intact");
    }

    raw_tail_seen[0] = 0;
    mc_api_setter_run("{\"command\":\"ascset\",\"parameter\":\"0,blob,png,DEADBEEF\"}", &s, buf,
            sizeof buf);
    CHECK(!strcmp(raw_tail_seen, "png,DEADBEEF"), "raw tail from the JSON form");

    snprintf(longreq, sizeof longreq, "ascset|0,ledset,%0*d", 600, 7);
    CHECK(mc_api_setter_run(longreq, &s, buf, sizeof buf) == MC_SET_OK,
            "a 600-digit normal setter succeeds with a raw handler present");

    CHECK(mc_api_setter_register("dup", OP_DUP, fake_set, NULL) == 0, "register normal dup");
    CHECK(mc_api_setter_register_raw("dup", fake_raw, NULL) == 0, "register raw dup");
    raw_tail_seen[0] = 0;
    seen_n           = 0;
    mc_api_setter_run("ascset|0,dup,hello,world", &s, buf, sizeof buf);
    CHECK(!strcmp(raw_tail_seen, "hello,world") && seen_n == 0,
            "raw path wins over a same-named normal setter");

    CHECK(mc_api_setter_register_raw("BLOB", fake_raw, NULL) == -1,
            "a case variant is a duplicate in the raw table as well");
    raw_tail_seen[0] = 0;
    mc_api_setter_run("ascset|0,BLOB,png,DEADBEEF", &s, buf, sizeof buf);
    CHECK(!strcmp(raw_tail_seen, "png,DEADBEEF"),
            "a case variant of a raw subcommand reaches the raw handler");
    CHECK(!strstr(buf, "Unknown option"), "...and is not answered as unknown");

    {
        char name23[24];
        memset(name23, 'q', 23);
        name23[23] = 0;
        CHECK(mc_api_setter_register_raw(name23, fake_raw, NULL) == 0,
                "register a raw name at the 23-char boundary");
        char longname[40];
        snprintf(longname, sizeof longname, "%sJUNKJUNKJUNKJUNK", name23);
        char req3[128];
        snprintf(req3, sizeof req3, "ascset|7,%s,1", longname);
        raw_tail_seen[0] = 0;
        mc_set_result rr = mc_api_setter_run(req3, &s, buf, sizeof buf);
        CHECK(rr == MC_SET_ERR_UNSUP && strstr(buf, longname),
                "an over-long raw subcommand falls through and answers Unknown option,"
                " not a truncated match");
        CHECK(raw_tail_seen[0] == 0, "the 23-char raw handler was never invoked");
    }

    mc_api_setter_reset();
    CHECK(mc_api_setter_declare("plug-hidden", MC_API_SETTER_HIDDEN, NULL, NULL) == 0 &&
                    mc_api_setter_declare("plug-first", 0, "ledset", NULL) == 0,
            "declare a hidden name and a name listed before ledset");
    CHECK(mc_api_setter_register("qr_auth", OP_ECHO, echo_set, NULL) == 0 &&
                    mc_api_setter_register("reboot_web", OP_ECHO, echo_set, NULL) == 0 &&
                    mc_api_setter_register("ip", OP_ECHO, echo_set, NULL) == 0 &&
                    mc_api_setter_register("plug-hidden", OP_ECHO, echo_set, NULL) == 0 &&
                    mc_api_setter_register("ledset", OP_LEDSET, fake_set, NULL) == 0 &&
                    mc_api_setter_register("plug-first", OP_ECHO, echo_set, NULL) == 0 &&
                    mc_api_setter_register_raw("wallpaper", fake_raw, NULL) == 0,
            "register a subset of names out of order");
    {
        char hmsg[256];
        mc_api_setter_run("ascset|0,help", &s, hmsg, sizeof hmsg);
        CHECK(!strcmp(hmsg, "ASC 0 set info: help|plug-first|ledset|wallpaper|qr_auth|ip|"),
                "help: nano3s order first, other names after, reboot_web/plug-hidden not listed "
                "(got \"%s\")",
                hmsg);
    }

    CHECK(mc_api_setter_declare("plug-first", MC_API_SETTER_HIDDEN, NULL, NULL) == 0 &&
                    mc_api_setter_declare("plug-loop", 0, "loop", NULL) == 0 &&
                    mc_api_setter_register("plug-loop", OP_ECHO, echo_set, NULL) == 0 &&
                    mc_api_setter_register("password", OP_ECHO, echo_set, NULL) == 0,
            "declare plug-first again and a name listed before the unregistered loop");
    {
        char hmsg[256];
        mc_api_setter_run("ascset|0,help", &s, hmsg, sizeof hmsg);
        CHECK(!strcmp(hmsg,
                      "ASC 0 set info: help|plug-first|ledset|wallpaper|plug-loop|"
                      "password|qr_auth|ip|"),
                "help: a repeated declaration is ignored, a name goes where its anchor "
                "would be listed (got \"%s\")",
                hmsg);
    }
    {
        char dname[16];
        int  ok = 1;
        for (int k = 0; k < 13; k++) {
            snprintf(dname, sizeof dname, "plug-d%d", k);
            ok = ok && mc_api_setter_declare(dname, 0, NULL, NULL) == 0;
        }
        CHECK(ok && mc_api_setter_declare("plug-d13", 0, NULL, NULL) == -1,
                "sixteen declarations fit, a seventeenth is refused");
    }
    mc_api_setter_reset();
    CHECK(mc_api_setter_register("plug-first", OP_ECHO, echo_set, NULL) == 0 &&
                    mc_api_setter_register("ledset", OP_LEDSET, fake_set, NULL) == 0,
            "register plug-first and ledset after a reset");
    {
        char hmsg[256];
        mc_api_setter_run("ascset|0,help", &s, hmsg, sizeof hmsg);
        CHECK(!strcmp(hmsg, "ASC 0 set info: help|ledset|plug-first|"),
                "help: a reset clears the declarations (got \"%s\")", hmsg);
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
