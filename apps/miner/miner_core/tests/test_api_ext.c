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
#include "ctrl_cmds.h"
#include "miner_ipc.h"
#include "mc_api_cmds.h"
#include "mc_api_setter.h"

#include "../../tests/include/tst_check.h"

#define PLUGIN_SYSTEMCFG "/nonexistent/systemcfg.ini"

static int         g_sent_n;    /* MSG_CTRL_CMD sends captured */
static mc_ctrl_cmd g_sent;      /* last MSG_CTRL_CMD payload sent */
static char        g_log[4096]; /* log lines since the last reset, newline-separated */

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
    if (type == MSG_CTRL_CMD && data && len >= (int)sizeof g_sent) {
        g_sent = *(const mc_ctrl_cmd *)data;
        g_sent_n++;
    }
    return OSIF_OK;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    size_t  n = strlen(g_log);
    va_list ap;

    (void)lvl;
    if (n + 1 >= sizeof g_log)
        return;
    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
    va_end(ap);
    n = strlen(g_log);
    if (n + 1 < sizeof g_log)
        strcpy(g_log + n, "\n");
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

#define main core_miner_main_unused
#include "core_miner_tu.h"
#undef main

static mc_set_result log_reply_stub(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)op;
    (void)args;
    (void)s;
    (void)ctx;
    snprintf(msg, msgcap, "Item-1 ID:SECRET42");
    mc_api_setter_log_reply("Item-1 ID:xxx");
    return MC_SET_INFO;
}

int main(void)
{
    char *buf = malloc(MC_API_REPLY_CAP);
    if (!buf) {
        printf("FAILED (oom)\n");
        return 1;
    }
    mc_api_stats s = { 0 };
    s.prod         = "p";
    s.model        = "m";
    s.hwtype       = "h";
    s.swtype       = "sw";
    s.version      = "v";
    s.dna          = "d";
    s.mac          = "mac";

    int       n  = core_setters_register(PLUGIN_SYSTEMCFG);
    const int wp = ctrl_cmd_available(CTRL_OP_WP_RELOAD);
    CHECK(n > 0, "core_setters_register registers the built-in and plugin setters (%d)", n);
    CHECK(mc_api_setter_lookup("plugin-stub", NULL) != NULL,
            "an API_PLUGIN_SETTERS entry with CTRL_OP_NONE registers on [%s]", BOARDCONF_SECTION);
    CHECK((mc_api_setter_lookup("plugin-stub-wp", NULL) != NULL) == wp,
            "an entry gated on CTRL_OP_WP_RELOAD registers only where the board offers it "
            "([%s] offers it: %d)",
            BOARDCONF_SECTION, wp);

    CHECK(!mc_api_cmd_writes("ascset|0,loop", "ascset") &&
                    !mc_api_cmd_writes("ascset|0,plugin-stub", "ascset") &&
                    mc_api_cmd_writes("ascset|0,plugin-stub,1", "ascset") &&
                    mc_api_cmd_writes("ascset|0,plugin-stub-hidden", "ascset"),
            "loop and an argument-less plugin-stub only read, plugin-stub with an argument and a "
            "plugin setting declared without a read check write");

    mc_api_dispatch_text("ascset|0,plugin-stub", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "STATUS=I") && strstr(buf, "Code=118") &&
                    strstr(buf, "Msg=ASC 0 set info: plugin-stub ctx " PLUGIN_SYSTEMCFG ","),
            "`ascset 0,plugin-stub` reaches the plugin setter with the systemcfg path as ctx "
            "(got %.200s)",
            buf);

    g_sent_n = 0;
    mc_api_dispatch_text("ascset|0,plugin-stub,1", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "STATUS=S") && strstr(buf, "Code=119") && g_sent_n == 1 &&
                    g_sent.op == CTRL_OP_WP_RELOAD,
            "core_ascset_forward parses and forwards the plugin's control op to mm "
            "(got %.200s, %d send(s))",
            buf, g_sent_n);

    mc_api_dispatch_text("ascset|0,help", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "?plugin-stub?") != NULL,
            "help lists the plugin setter, '|' sent as '?' in the text form (got %.400s)", buf);
    const char *next = ctrl_cmd_available(CTRL_OP_LEDMODE) ? "?plugin-stub?ledmode?" :
            ctrl_cmd_available(CTRL_OP_WORKMODE)           ? "?plugin-stub?workmode?" :
                                                             "?plugin-stub?loop?";
    CHECK(strstr(buf, next) != NULL,
            "help lists a plugin setter declared before ledmode right before the next "
            "registered setter (got %.400s)",
            buf);
    CHECK(mc_api_setter_lookup("plugin-stub-hidden", NULL) != NULL &&
                    strstr(buf, "plugin-stub-hidden") == NULL,
            "help leaves out a registered plugin setter declared hidden (got %.400s)", buf);

    g_log[0] = 0;
    mc_api_dispatch_text("ascset|0,plugin-stub-hidden,hunter2", "ascset", &s, buf,
            MC_API_REPLY_CAP);
    CHECK(strstr(g_log, "API setting plugin-stub-hidden for unit 0, arguments hidden (7 bytes)") &&
                    !strstr(g_log, "hunter2"),
            "a plugin setter declared secret logs its arguments hidden (log %.400s)", g_log);

    CHECK(mc_api_setter_register("log-reply-stub", CTRL_OP_NONE, log_reply_stub, NULL) == 0,
            "a setter that sets its logged reply registers");

    char hex[WEBPASS_HEX_LEN + 1];
    core_webpass_sha256_hex("hunter3", 7, hex);
    core_webpass_cache_set(hex);
    g_log[0] = 0;
    mc_api_dispatch_text("ascset|0,log-reply-stub,hunter3,1", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(g_log, "API setting log-reply-stub for unit 0, arguments (9 bytes): xxx,1") &&
                    !strstr(g_log, "hunter3"),
            "the web password among the arguments of a setting is logged as xxx (log %.400s)",
            g_log);

    g_log[0] = 0;
    mc_api_dispatch_text("ascset|0,log-reply-stub,hunter3x,1", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(g_log, "API setting log-reply-stub for unit 0, arguments (10 bytes): hunter3x,1"),
            "arguments without the web password are logged as received (log %.400s)", g_log);

    g_log[0] = 0;
    mc_api_dispatch_text("ascset|0,no-such-setting,hunter3x,1", "ascset", &s, buf,
            MC_API_REPLY_CAP);
    CHECK(strstr(buf, "Unknown option: no-such-setting") &&
                    strstr(g_log,
                            "API setting no-such-setting for unit 0, unknown, arguments "
                            "hidden (10 bytes)") &&
                    !strstr(g_log, "hunter3x"),
            "an unknown setting logs its name and argument length only (log %.400s)", g_log);

    g_log[0] = 0;
    mc_api_dispatch_text("ascset|0,no-such-write,AB12CD34EF567890", "ascset", &s, buf,
            MC_API_REPLY_CAP);
    CHECK(strstr(g_log,
                  "API setting no-such-write for unit 0, unknown, arguments hidden "
                  "(16 bytes)") &&
                    !strstr(g_log, "AB12CD34EF567890"),
            "an unknown setting carrying a serial number leaves it out of the log (log %.400s)",
            g_log);

    g_log[0] = 0;
    mc_api_dispatch_text("ascset|0,log-reply-stub,1", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "Msg=ASC 0 set info: Item-1 ID:SECRET42,") != NULL,
            "the reply carries the text the setter wrote (got %.200s)", buf);
    CHECK(strstr(g_log, "log-reply-stub accepted (code 118: ASC 0 set info: Item-1 ID:xxx)") &&
                    !strstr(g_log, "SECRET42"),
            "the operation log carries the text given to mc_api_setter_log_reply (log %.400s)",
            g_log);
    g_log[0] = 0;
    mc_api_dispatch_text("ascset|0,plugin-stub", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(g_log, "plugin-stub accepted (code 118: ASC 0 set info: plugin-stub ctx"),
            "the next setting logs its own reply (log %.400s)", g_log);

    free(buf);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
