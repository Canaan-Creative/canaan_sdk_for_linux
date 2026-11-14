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
#include "core_ctrl.h"
#include "ctrl_cmds.h"
#include "miner_ipc.h"
#include "mc_api_cmds.h"
#include "mc_api_setter.h"

#include "../../tests/include/tst_check.h"

#define N_WANT_SETTERS ((int)(sizeof WANT_SETTERS / sizeof WANT_SETTERS[0]))
#define N_WANT_CMDS    ((int)(sizeof WANT_CMDS / sizeof WANT_CMDS[0]))

/* Every setter name a board can register */
static const char *const WANT_SETTERS[] = {
    "workmode",
    "worklevel",
    "work_mode_lvl",
    "ledmode",
    "ledset",
    "nightlamp",
    "qr_auth",
    "password",
    "reboot",
    "reboot_web",
    "activate",
    "filter-clean",
    "loop",
    "lcd",
};

/* Setters the x3 descriptor does not register */
static const char *const ABSENT_X3[] = {
    "ledmode",
    "ledset",
    "nightlamp",
    "filter-clean",
    "lcd",
    "activate",
};

static const char *const *g_absent; /* Setters absent on the descriptor under test */
static int                g_absent_n;
static int                g_ctrl_ops;

/* Commands main() registers on every board */
static const char *const WANT_CMDS[] = {
    "setpool",
    "enablepool",
    "disablepool",
    "addpool",
    "removepool",
    "switchpool",
    "poolpriority",
    "zero",
    "softon",
    "softoff",
};

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
    if (type == MSG_CTRL_CMD && data && len >= (int)sizeof(mc_ctrl_cmd)) {
        mc_ctrl_ack a = { .result = MC_SET_OK, .seq = ((const mc_ctrl_cmd *)data)->seq };
        core_ctrl_ack(&a, (int)sizeof a);
    }
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

static int want_present(const char *name)
{
    for (int i = 0; i < g_absent_n; i++)
        if (!strcmp(name, g_absent[i]))
            return 0;
    return 1;
}

int main(void)
{
    char *buf = malloc(MC_API_REPLY_CAP);
    if (!buf) {
        printf("FAILED (oom)\n");
        return 1;
    }
    if (!strcmp(BOARDCONF_SECTION, "NANO3S")) {
        g_absent   = NULL;
        g_absent_n = 0;
        g_ctrl_ops = 1;
    } else if (!strcmp(BOARDCONF_SECTION, "X3")) {
        g_absent   = ABSENT_X3;
        g_absent_n = (int)(sizeof ABSENT_X3 / sizeof ABSENT_X3[0]);
        g_ctrl_ops = 0;
    } else {
        printf("FAILED: no expected setter set for descriptor [%s] — list it by "
               "hand off that board's own CAP_* and BOARD_CTRL_OPS\n",
                BOARDCONF_SECTION);
        free(buf);
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

    CHECK(mc_api_setter_lookup("ledmode", NULL) == NULL,
            "before core_setters_register the table is empty");
    mc_api_dispatch_text("ascset|0,ledmode,1", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "Msg=ASC 0 set failed: Unknown option: ledmode") != NULL,
            "...and a real device command is refused as an unknown option");
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=120"),
            "...with the ordinary rejected-setter letter/code, not a distinct one");

    int want_n = 0;
    for (int i = 0; i < N_WANT_SETTERS; i++)
        want_n += want_present(WANT_SETTERS[i]);
    int n = core_setters_register("/nonexistent/systemcfg.ini");
    CHECK(n == want_n,
            "core_setters_register registers the %d of %d ascset subcommands this "
            "descriptor declares (got %d)",
            want_n, N_WANT_SETTERS, n);
    {
        int found = 0, absent = 0, wrong = 0;
        for (int i = 0; i < N_WANT_SETTERS; i++) {
            int want = want_present(WANT_SETTERS[i]);
            int have = mc_api_setter_lookup(WANT_SETTERS[i], NULL) != NULL;
            if (want && have)
                found++;
            else if (!want && !have)
                absent++;
            else {
                wrong++;
                fprintf(stderr, "    (%s setter: %s)\n", want ? "missing" : "unexpected",
                        WANT_SETTERS[i]);
            }
        }
        CHECK(found == want_n && wrong == 0,
                "every ascset subcommand this descriptor declares resolves (%d/%d)", found, want_n);
        CHECK(absent == N_WANT_SETTERS - want_n,
                "...and the %d whose capability it does NOT declare are absent, so "
                "the app is refused rather than answered for absent hardware",
                N_WANT_SETTERS - want_n);
    }
    mc_api_dispatch_text("ascset|0,nosuchsetter,1", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "Unknown option: nosuchsetter") != NULL,
            "...and a name that is NOT in the table still refuses, so the "
            "assertion above is about these names and not about any name");
    mc_api_dispatch_text("ascset|0,no-such-setting,1", "ascset", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=120") &&
                    strstr(buf, "Msg=ASC 0 set failed: Unknown option: no-such-setting,"),
            "without an API plugin `ascset|0,no-such-setting,1` answers E/120 Unknown option");

    CHECK(core_setters_register("/nonexistent/systemcfg.ini") == 0,
            "a second core_setters_register registers nothing (all duplicates)");

    mc_api_command_reset();
    CHECK(core_pool_setter_register("/nonexistent/pools.conf", "/nonexistent/systemcfg.ini") == 0,
            "shipped order 1/3: setpool registers");
    CHECK(core_pool_cmds_register("/nonexistent/pools.conf") == 0,
            "shipped order 2/3: the seven pool commands register");
    int nsoft = core_soft_setter_register();
    CHECK(nsoft == 2 * g_ctrl_ops,
            "shipped order 3/3: the soft-on/off commands the descriptor declares register "
            "(got %d)",
            nsoft);
    int nwp = core_wallpaper_register();
    CHECK(nwp == g_ctrl_ops, "wallpaper registers only on a board that declares it (got %d)", nwp);
    mc_api_dispatch_text("ascset|0,wallpaper,bmp", "ascset", &s, buf, MC_API_REPLY_CAP);
    if (g_ctrl_ops)
        CHECK(strstr(buf, "Msg=ASC 0 set failed: input param error") != NULL,
                "...and reaches the wallpaper setter");
    else
        CHECK(strstr(buf, "Msg=ASC 0 set failed: Unknown option: wallpaper") != NULL,
                "...and is refused as an unknown option on a board that does not declare it");

    {
        const char *want = NULL;
        if (!strcmp(BOARDCONF_SECTION, "NANO3S"))
            want = "\"Msg\":\"ASC 0 set info: help|ledmode|ledset|lcd|nightlamp|wallpaper|"
                   "workmode|worklevel|work_mode_lvl|reboot|filter-clean|activate|loop|"
                   "password|qr_auth|\"";
        else if (!strcmp(BOARDCONF_SECTION, "X3"))
            want = "\"Msg\":\"ASC 0 set info: help|workmode|worklevel|work_mode_lvl|reboot|loop|"
                   "password|qr_auth|\"";
        if (want) {
            mc_api_dispatch("{\"command\":\"ascset\",\"parameter\":\"0,help\"}", "ascset", &s, buf,
                    MC_API_REPLY_CAP);
            CHECK(strstr(buf, want) != NULL, "help lists the %s names in board order (got %.400s)",
                    BOARDCONF_SECTION, buf);
        }
    }

    {
        int want_cmds = N_WANT_CMDS - 2 * !g_ctrl_ops;
        int found = 0, missing = 0;
        for (int i = 0; i < want_cmds; i++) {
            if (mc_api_command_lookup(WANT_CMDS[i], NULL))
                found++;
            else {
                missing++;
                fprintf(stderr, "    (absent command: %s)\n", WANT_CMDS[i]);
            }
        }
        CHECK(found == want_cmds && missing == 0, "all %d commands main() asks for are reachable",
                want_cmds);
        CHECK((mc_api_command_lookup("softon", NULL) != NULL) == g_ctrl_ops &&
                        (mc_api_command_lookup("softoff", NULL) != NULL) == g_ctrl_ops,
                "...softon and softoff only where the descriptor declares them");
    }

    mc_api_dispatch("{\"command\":\"softoff\"}", "softoff", &s, buf, MC_API_REPLY_CAP);
    if (g_ctrl_ops) {
        CHECK(strstr(buf, "unknown/unsupported command") == NULL,
                "json `softoff` is not answered as a nonexistent command");
        CHECK(strstr(buf, "\"Msg\":\"success softoff now\"") != NULL,
                "...it reaches soft_handler, which answers the immediate softoff");
    } else {
        CHECK(strstr(buf, "\"Code\":14,\"Msg\":\"Invalid command\"") != NULL,
                "json `softoff` is answered as a nonexistent command on this descriptor");
    }
    mc_api_dispatch_text("softon", "softon", &s, buf, MC_API_REPLY_CAP);
    if (g_ctrl_ops) {
        CHECK(strstr(buf, "Msg=unsupported,") == NULL, "text `softon` likewise — not unsupported");
        CHECK(strstr(buf, "Msg=success softon now,") != NULL,
                "...and answers softon, not softoff, on the softon slot");
    } else {
        CHECK(strstr(buf, "Code=14,Msg=Invalid command") != NULL,
                "text `softon` likewise — Invalid command");
    }

    mc_api_dispatch("{\"command\":\"zero\"}", "zero", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "unknown/unsupported command") == NULL &&
                    strstr(buf, "Missing zero parameters") != NULL,
            "the last pool slot (`zero`) IS reachable and answers its own handler");
    mc_api_dispatch("{\"command\":\"zero\",\"parameter\":\"all\"}", "zero", &s, buf,
            MC_API_REPLY_CAP);
    CHECK(strstr(buf, "Zeroed all stats") != NULL, "json `zero all` zeroes the statistics");
    mc_api_dispatch_text("zero|all,false", "zero", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "STATUS=S") && strstr(buf, "Code=97,") && strstr(buf, "Zeroed all stats,"),
            "text `zero|all,false` zeroes the statistics without the summary");
    mc_api_dispatch_text("zero|bestshare,true", "zero", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "Code=96,") && strstr(buf, "Zeroed bestshare stats with summary"),
            "text `zero|bestshare,true` zeroes the best share with the summary");
    mc_api_dispatch_text("zero|nosuch,false", "zero", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Invalid zero parameter 'nosuch'"),
            "an unknown counter name is refused");
    mc_api_dispatch_text("zero|nosuch", "zero", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Invalid zero parameter 'nosuch'"),
            "...also without the summary flag");
    mc_api_dispatch("{\"command\":\"setpool\"}", "setpool", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "unknown/unsupported command") == NULL,
            "the 1st slot (`setpool`) is reachable too");

    struct {
        const char *text, *name;
    } idonly[] = {
        { "enablepool|0", "enablepool" },
        { "disablepool|0", "disablepool" },
        { "switchpool|0", "switchpool" },
        { "poolpriority|0", "poolpriority" },
    };
    for (int i = 0; i < (int)(sizeof idonly / sizeof idonly[0]); i++) {
        mc_api_dispatch_text(idonly[i].text, idonly[i].name, &s, buf, MC_API_REPLY_CAP);
        CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=8,") && strstr(buf, "No pools"),
                "...and reports No pools (Code 8) on an empty table");
    }
    mc_api_dispatch_text("addpool|", "addpool", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "unknown/unsupported command") == NULL, "`addpool` reaches its parser");
    mc_api_dispatch_text("removepool|0", "removepool", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "unknown/unsupported command") == NULL, "`removepool` reaches its parser");
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=8,") && strstr(buf, "No pools"),
            "...and reports No pools (Code 8) on an empty table");
    mc_api_dispatch_text("removepool", "removepool", &s, buf, MC_API_REPLY_CAP);
    CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=8,") && strstr(buf, "No pools"),
            "`removepool` with no id on an empty table reports No pools (Code 8) "
            "ahead of the missing id");

    pthread_mutex_lock(&g_pools_lock);
    g_pool_pub_n = 1;
    pthread_mutex_unlock(&g_pools_lock);
    static const char *const noid[] = { "enablepool", "disablepool", "switchpool", "poolpriority" };
    for (int i = 0; i < (int)(sizeof noid / sizeof noid[0]); i++) {
        mc_api_dispatch_text(noid[i], noid[i], &s, buf, MC_API_REPLY_CAP);
        CHECK(strstr(buf, "STATUS=E") && strstr(buf, "Code=25,") &&
                        strstr(buf, "Msg=Missing pool id parameter,"),
                "`%s` with no id on a non-empty table reports "
                "Missing pool id parameter (Code 25)",
                noid[i]);
    }
    pthread_mutex_lock(&g_pools_lock);
    g_pool_pub_n = 0;
    pthread_mutex_unlock(&g_pools_lock);

    free(buf);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
