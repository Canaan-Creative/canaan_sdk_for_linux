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
#include "mc_api_server.h"
#include "mc_api_cmds.h"
#include "mc_api_time.h"
#include <stdio.h>
#include <string.h>

#include "../../tests/include/tst_check.h"

int tst_settimeofday(const struct timeval *tv, const struct timezone *tz);
#define settimeofday(tv, tz) tst_settimeofday((tv), (tz))
#include "mc_api_time.c"
#undef settimeofday

#define REQ_TEXT "time|set,t:1787102966,America/Los_Angeles" /* Text-form time set request */
/* JSON-form time set request */
#define REQ_JSON "{\"command\":\"time\",\"parameter\":\"set,t:1787102966,America/Los_Angeles\"}"
/* Reply message to a valid time set request */
#define SET_INFO "Time set info: time t:1787102966 America/Los_Angeles"
/* Reply message to an unrecognised time argument */
#define UNKNOWN "Time set info: time unknown argument"

static int g_stod_calls;        /* settimeofday() calls */
static int g_stod_rc;           /* Return of tst_settimeofday() */
static int g_fwd_calls_at_stod; /* g_fwd_calls at the last settimeofday() */
static int g_fwd_calls;         /* capture_fwd() calls */

static char          g_fwd[64]; /* Time zone passed to the last capture_fwd() call */
static mc_set_result g_fwd_rc;  /* Return of capture_fwd() */

int tst_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv;
    (void)tz;
    g_stod_calls++;
    g_fwd_calls_at_stod = g_fwd_calls;
    return g_stod_rc;
}
static mc_set_result capture_fwd(const char *tz)
{
    g_fwd_calls++;
    snprintf(g_fwd, sizeof g_fwd, "%s", tz ? tz : "");
    return g_fwd_rc;
}

static int read_tz(char *out, size_t cap)
{
    snprintf(out, cap, "Asia/Shanghai");
    return 0;
}

static int run(const char *req, char *msg, size_t cap, int *code)
{
    *code = -1;
    return mc_api_time_run(req, msg, cap, code);
}

int main(void)
{
    char         buf[2048], msg[128];
    mc_api_stats s = { 0 };
    int          code;

    CHECK(run(REQ_TEXT, msg, sizeof msg, &code) == MC_SET_INFO && code == 131 &&
                    !strcmp(msg, SET_INFO),
            "captured text request -> I/131 nano3s set info (%s)", msg);
    CHECK(run(REQ_JSON, msg, sizeof msg, &code) == MC_SET_INFO && code == 131,
            "captured json request -> I/131");

    CHECK(run("time|get", msg, sizeof msg, &code) == MC_SET_INFO && code == 131 &&
                    !strcmp(msg, "Time set info: time t:"),
            "get with no zone reader -> I/131 empty zone (%s)", msg);
    mc_api_time_set_tz_reader(read_tz);
    CHECK(run("time|get", msg, sizeof msg, &code) == MC_SET_INFO && code == 131 &&
                    !strcmp(msg, "Time set info: time t:Asia/Shanghai"),
            "get -> I/131 time t:<zone> (%s)", msg);
    CHECK(run("time|get,x", msg, sizeof msg, &code) == MC_SET_INFO &&
                    !strcmp(msg, "Time set info: time t:Asia/Shanghai"),
            "get with a trailing field still reads (nano3s sscanf count 1)");
    mc_api_time_set_tz_reader(NULL);

    static const char *const unknown[] = {
        "time|get,t:1787102966,UTC",
        "time|set",
        "time|set,1787102966,UTC",
        "time|set,t:notanumber,UTC",
        "time|set,t:1787102966x,UTC",
        "time|sett,t:1787102966,UTC",
        "time|set,t:1787102966,",
        "time|set,t:1787102966",
        "time|set,t:0",
    };
    for (size_t i = 0; i < sizeof unknown / sizeof unknown[0]; i++)
        CHECK(run(unknown[i], msg, sizeof msg, &code) == MC_SET_INFO && code == 131 &&
                        !strcmp(msg, UNKNOWN),
                "%s -> I/131 time unknown argument (%s)", unknown[i], msg);

    CHECK(run("time|set,t:1787102966, UTC extra", msg, sizeof msg, &code) == MC_SET_INFO &&
                    code == 131 && !strcmp(msg, "Time set info: time t:1787102966 UTC"),
            "zone is the first whitespace-delimited token (nano3s %%64s) (%s)", msg);
    CHECK(run("time|set,t:1787102966,"
              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABBBB",
                  msg, sizeof msg, &code) == MC_SET_INFO &&
                    code == 131 &&
                    !strcmp(msg,
                            "Time set info: time t:1787102966 "
                            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
            "zone longer than 64 chars is cut to 64 (nano3s %%64s) (%s)", msg);

    CHECK(run("time", msg, sizeof msg, &code) == MC_SET_INFO && code == 130 &&
                    !strcmp(msg, "Time parameter is not available"),
            "no payload -> I/130");
    CHECK(run("time|set,t:4133980258,UTC", msg, sizeof msg, &code) == MC_SET_ERR_ARG &&
                    code == 120 && !strcmp(msg, "Time set failed: time out of range"),
            "2101 epoch out of range -> E/120 with the app's failure word (%s)", msg);
    CHECK(run("time|set,t:0,UTC", msg, sizeof msg, &code) == MC_SET_ERR_ARG && code == 120,
            "epoch 0 out of range -> E/120");

    mc_api_time_set_forwarder(capture_fwd);

    g_fwd_calls         = 0;
    g_fwd[0]            = 0;
    g_fwd_rc            = 0;
    g_stod_calls        = 0;
    g_fwd_calls_at_stod = 0;
    run(REQ_TEXT, msg, sizeof msg, &code);
    CHECK(g_fwd_calls == 1 && !strcmp(g_fwd, "America/Los_Angeles"),
            "set with tz -> forwarder called with the parsed zone");
    CHECK(g_stod_calls == 1 && g_fwd_calls_at_stod == 1,
            "set with tz -> zone forwarded before the system time is set (nano3s order)");

    g_fwd_calls = 0;
    run("time|set,t:1787102966", msg, sizeof msg, &code);
    CHECK(g_fwd_calls == 0, "set without tz (nano3s sscanf count 2) -> forwarder NOT called");

    g_fwd_calls = 0;
    run("time|set,t:0,America/Los_Angeles", msg, sizeof msg, &code);
    CHECK(g_fwd_calls == 0, "rejected set -> forwarder NOT called");

    g_fwd_calls  = 0;
    g_fwd_rc     = MC_SET_ERR_ARG;
    g_stod_calls = 0;
    CHECK(run("time|set,t:1787102966,Mars/Olympus", msg, sizeof msg, &code) == MC_SET_ERR_ARG &&
                    code == 120 && !strcmp(msg, "Time set failed: invalid timezone") &&
                    g_fwd_calls == 1,
            "a zone mm refuses -> E/120 invalid timezone, not a success (%s)", msg);
    CHECK(g_stod_calls == 0, "a zone mm refuses -> system time NOT set");

    g_fwd_calls  = 0;
    g_fwd_rc     = MC_SET_ERR_HW;
    g_stod_calls = 0;
    CHECK(run(REQ_TEXT, msg, sizeof msg, &code) == MC_SET_ERR_HW && code == 120 &&
                    !strcmp(msg, "Time set failed: timezone not confirmed") && g_fwd_calls == 1,
            "an unconfirmed tz forward -> E/120, not a success (%s)", msg);
    CHECK(g_stod_calls == 0, "an unconfirmed tz forward -> system time NOT set");
    g_fwd_rc = MC_SET_OK;

    g_stod_rc = -1;
    CHECK(run(REQ_TEXT, msg, sizeof msg, &code) == MC_SET_ERR_HW && code == 120 &&
                    !strcmp(msg, "Time set failed: system clock not set"),
            "a failed settimeofday -> E/120 (%s)", msg);
    g_stod_rc = 0;

    mc_api_time_set_forwarder(NULL);

    mc_api_dispatch_text(REQ_TEXT, "time", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=I,When=0,Code=131,Msg=" SET_INFO ","),
            "text time applied -> I/131 nano3s envelope");

    mc_api_dispatch_text("time|get,t:1", "time", &s, buf, sizeof buf);
    CHECK(strstr(buf, "STATUS=I,When=0,Code=131,Msg=" UNKNOWN ","),
            "text malformed time -> I/131 time unknown argument");

    mc_api_dispatch(REQ_JSON, "time", &s, buf, sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":\"I\"") && strstr(buf, "\"Code\":131") &&
                    strstr(buf, "\"Msg\":\"" SET_INFO "\""),
            "json time applied -> I/131 envelope");

    mc_api_dispatch("{\"command\":\"time\",\"parameter\":\"set,t:4133980258,UTC\"}", "time", &s,
            buf, sizeof buf);
    CHECK(strstr(buf, "\"STATUS\":\"E\"") && strstr(buf, "\"Code\":120"),
            "json out-of-range time -> E/120");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
