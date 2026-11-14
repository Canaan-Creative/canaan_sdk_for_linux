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

#include "core_setters_harness.h"
#include "../../tests/include/tst_rsa_sign.h"

#undef CORE_CTRL_ACK_MS
#define CORE_CTRL_ACK_MS 50u
#include "core_ctrl.c"

#include "core_ascset.c"

#define WM_SPAN(lo, hi) (v >= (lo) && v <= (hi)) ||

#define CAP_ROW(e, nm, payload, cap) \
    do { \
        int   fitted = fitted_here(nm); \
        void *cctx   = NULL; \
        CHECK((mc_api_setter_lookup(nm, &cctx) != NULL) == fitted, \
                "%s is %s the ascset registry on this descriptor", nm, fitted ? "IN" : "NOT in"); \
        if (fitted) \
            break; \
        const char *cr = ascset(nm, DAY); \
        CHECK(strstr(cr, "STATUS=E") && strstr(cr, "Code=120") && \
                        strstr(cr, "Msg=ASC 0 set failed: Unknown option: " nm), \
                "...so `ascset 0,%s` answers E/120 Unknown option on the " \
                "unauthenticated :4028 port, not a success for absent hardware", \
                nm); \
        CHECK(sent_n == 0, "...and `%s` forwards nothing to mm", nm); \
    } while (0);

#define CASE_ROW(e, nm, payload, cap) \
    do { \
        char   up[32]; \
        size_t ul = 0; \
        for (const char *cp = nm; *cp && ul + 1 < sizeof up; cp++, ul++) \
            up[ul] = (*cp >= 'a' && *cp <= 'z') ? (char)(*cp - 'a' + 'A') : *cp; \
        up[ul]                = 0; \
        void            *lctx = (void *)1, *uctx = (void *)2; \
        mc_api_setter_fn lfn = mc_api_setter_lookup(nm, &lctx); \
        mc_api_setter_fn ufn = mc_api_setter_lookup(up, &uctx); \
        CHECK(lfn == ufn && (lfn == NULL || lctx == uctx), \
                "\"%s\" and \"%s\" resolve to the same registry entry", nm, up); \
        if (!fitted_here(nm)) \
            break; \
        char lo_reply[2048]; \
        snprintf(lo_reply, sizeof lo_reply, "%s", ascset(nm, DAY)); \
        int         lo_sent  = sent_n; \
        const char *up_reply = ascset(up, DAY); \
        CHECK(!strcmp(lo_reply, up_reply), \
                "...and `ascset 0,%s` answers byte-identically to `ascset 0,%s`, " \
                "so the reply carries the canonical name, not the wire spelling", \
                up, nm); \
        CHECK(lo_sent == 0 && sent_n == 0, "...with neither spelling forwarding to mm (%d / %d)", \
                lo_sent, sent_n); \
    } while (0);

/* ascset options registered on the NANO3S descriptor */
static const char *const FITTED_NANO3S[] = {
    "workmode",
    "worklevel",
    "work_mode_lvl",
    "ledmode",
    "ledset",
    "nightlamp",
    "filter-clean",
    "lcd",
};
/* ascset options registered on the X3 descriptor */
static const char *const FITTED_X3[] = {
    "workmode",
    "worklevel",
    "work_mode_lvl",
};
static const char *const *g_fitted; /* FITTED_* list of this build's descriptor */
static int                g_fitted_n;

static int set_tz_ini(ini_t *ini, void *ctx)
{
    return ini_set(ini, "syscfg", "timezone", (const char *)ctx);
}

static const char *topcmd(const char *cmd, const char *tail)
{
    static char  buf[2048];
    char         req[256];
    mc_api_stats s = { 0 };
    s.prod         = "Avalon Nano3s";
    s.version      = "v";
    s.dna          = g_dna;
    s.mac          = "m";
    if (tail)
        snprintf(req, sizeof req, "%s|%s", cmd, tail);
    else
        snprintf(req, sizeof req, "%s", cmd);
    sent_n = 0;
    memset(&sent_cmd, 0, sizeof sent_cmd);
    mc_api_dispatch_text(req, cmd, &s, buf, sizeof buf);
    return buf;
}

static const char *topcmd_json(const char *cmd, const char *param)
{
    static char  buf[2048];
    char         req[256];
    mc_api_stats s = { 0 };
    s.prod         = "Avalon Nano3s";
    s.version      = "v";
    s.dna          = g_dna;
    s.mac          = "m";
    if (param)
        snprintf(req, sizeof req, "{\"command\":\"%s\",\"parameter\":\"%s\"}", cmd, param);
    else
        snprintf(req, sizeof req, "{\"command\":\"%s\"}", cmd);
    sent_n = 0;
    memset(&sent_cmd, 0, sizeof sent_cmd);
    mc_api_dispatch(req, cmd, &s, buf, sizeof buf);
    return buf;
}

static const char *stored_webpass(const char *path)
{
    static char out[128];
    out[0]     = 0;
    ini_t *ini = ini_load(path);
    if (ini) {
        const char *v = ini_get(ini, "syscfg", "webpass");
        if (v)
            snprintf(out, sizeof out, "%s", v);
        ini_free(ini);
    }
    return out;
}

static int stored_activation(const char *path)
{
    ini_t *ini = ini_load(path);
    if (!ini)
        return 0;
    const char *v  = ini_get(ini, "dev_attr", "activation");
    int         on = v && atoi(v) == 1;
    ini_free(ini);
    return on;
}

static const char *live_prod(void)
{
    static core_hwcfg_buf b;
    core_hwcfg_copy(&b);
    return b.prod;
}

static const char *live_model(void)
{
    static core_hwcfg_buf b;
    core_hwcfg_copy(&b);
    return b.model;
}

static int fitted_here(const char *nm)
{
    for (int i = 0; i < g_fitted_n; i++)
        if (!strcmp(nm, g_fitted[i]))
            return 1;
    return 0;
}

static int wm_allowed(int v)
{
    return BOARD_WORKMODE_RANGES(WM_SPAN) 0;
}

static const char *activation_hex(const char *dna16)
{
    static char out[64 + 2 * TST_RSA_SIG_LEN + 1];
    uint8_t     md[SHA256_DIGEST_SIZE], sig[TST_RSA_SIG_LEN];

    out[0] = 0;
    sha256(dna16, strlen(dna16), md);
    if (tst_rsa_sign(PIC_SIGN_KEY, 1, md, sizeof md, sig) != 0)
        return out;
    memset(out, '0', 64);
    for (size_t i = 0; i < sizeof sig; i++)
        sprintf(out + 64 + 2 * i, "%02x", sig[i]);
    return out;
}

static void loop_checks(void)
{
    const char *r;

    {
        char want[64] = "Msg=ASC 0 set info: LOOP[";
        g_board_n     = HBOARD_COUNT;
        for (int b = 0; b < HBOARD_COUNT; b++) {
            g_chip[b] = 30;
            strcat(want, "30 ");
        }
        strcat(want, "]");
        r = ascset("loop", DAY);
        CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=118") && strstr(r, want),
                "loop -> I/118 + LOOP[<count per board>]");
        CHECK(sent_n == 0, "a loop read is answered core-side, never forwarded");
        r = ascset("loop,anything", DAY);
        CHECK(strstr(r, want), "loop ignores its argument (read-only)");
    }
    {
        char want[64] = "Msg=ASC 0 set info: LOOP[";
        g_board_n     = HBOARD_COUNT;
        for (int b = 0; b < HBOARD_COUNT; b++) {
            g_chip[b] = b % 2 ? 0 : 44 - b;
            snprintf(want + strlen(want), sizeof want - strlen(want), "%d ", g_chip[b]);
        }
        strcat(want, "]");
        r = ascset("loop", DAY);
        CHECK(strstr(r, want), "loop prints each board's own chip count (%s)", want + 20);
    }
    g_board_n = 0;
    memset(g_chip, 0, sizeof g_chip);
    r = ascset("loop", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set info: LOOP[]"), "loop with no telemetry -> empty LOOP[]");

    r = ascset("audio,50", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: Unknown option: audio") && sent_n == 0,
            "audio -> E/120 Unknown option, nothing forwarded");
}

static void workmode_checks(void)
{
    const char *r;

    if (wm_allowed(1) && wm_allowed(2)) {
        r = ascset("workmode,set,1", DAY);
        CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119"), "workmode applied -> S/119");
        CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_WORKMODE && sent_cmd.v.workmode.mode == 1,
                "workmode forwarded as DATA: mode %u, no argument text on the wire",
                sent_cmd.v.workmode.mode);

        r = ascset("workmode,2", DAY);
        CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && sent_n == 1 &&
                        sent_cmd.op == CTRL_OP_WORKMODE && sent_cmd.v.workmode.mode == 2,
                "workmode,2 takes the single-field mode grammar -> op WORKMODE mode 2");
        r = ascset("workmode,1-0", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: error:2 parameter error") && sent_n == 0,
                "...and refuses the two-field form, which is work_mode_lvl's grammar");

        r = ascset("work_mode_lvl,2-0", DAY);
        CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && sent_n == 1 &&
                        sent_cmd.op == CTRL_OP_WORKMODE && sent_cmd.v.workmode.mode == 2 &&
                        sent_cmd.v.workmode.level == 0,
                "work_mode_lvl,2-0 takes the mode+level grammar -> op WORKMODE mode 2 level 0");
        r = ascset("work_mode_lvl,2", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: error:2 parameter error") && sent_n == 0,
                "...and its refusal is nano3s error:2, never worklevel's out-of-range reason");

        g_maxmode = 1;
        r         = ascset("workmode,set,2", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r,
                                "Msg=ASC 0 set failed: error:1 not support set workmode "
                                "max_mode[1],") &&
                        sent_n == 0,
                "workmode above max_mode -> E/120 error:1 max_mode[1], nothing forwarded");
        r = ascset("work_mode_lvl,2-0", DAY);
        CHECK(strstr(r, "Msg=ASC 0 set failed: error:1 not support set workmode max_mode[1],") &&
                        sent_n == 0,
                "work_mode_lvl above max_mode refused the same way");
        r = ascset("workmode,set,1", DAY);
        CHECK(strstr(r, "STATUS=S") && sent_n == 1 && sent_cmd.v.workmode.mode == 1,
                "the ceiling itself is selectable (mode <= maxmode)");
        g_maxmode = 0;
        r         = ascset("workmode,set,1", DAY);
        CHECK(strstr(r, "error:1 not support set workmode max_mode[0],") && sent_n == 0,
                "a mode-0-only admission refuses mode 1");
        {
            char over[32];
            snprintf(over, sizeof over, "workmode,set,%d", CALI_MODE_MAX);
            r = ascset(over, DAY);
        }
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r,
                                "Msg=ASC 0 set failed: error:1 not support set workmode "
                                "max_mode[0],") &&
                        sent_n == 0,
                "workmode beyond every mode -> E/120 error:1 max_mode[0], nothing forwarded");
        r = ascset("workmode,set,-1", DAY);
        CHECK(strstr(r, "Msg=ASC 0 set failed: error:1 not support set workmode max_mode[0],") &&
                        sent_n == 0,
                "a negative workmode -> E/120 error:1, as nano3s' %%hhd wraps it above max_mode");
        r = ascset("workmode,set,x", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: error:2 parameter error") && sent_n == 0,
                "an unparsable workmode -> E/120 error:2 parameter error");
        g_maxmode = cali_mode_top(CALI_MODE_BOARD_MASK);
    }
    for (int m = -1; m <= CALI_MODE_MAX; m++) {
        char refused[96], arg[32];
        snprintf(refused, sizeof refused,
                "Msg=ASC 0 set failed: error:1 not support set workmode max_mode[%d],", g_maxmode);
        snprintf(arg, sizeof arg, "workmode,set,%d", m);
        r = ascset(arg, DAY);
        if (wm_allowed(m))
            CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && sent_n == 1 &&
                            sent_cmd.op == CTRL_OP_WORKMODE && sent_cmd.v.workmode.mode == m,
                    "[%s] workmode %d is in the board range -> S/119, mode forwarded",
                    BOARDCONF_SECTION, m);
        else
            CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") && strstr(r, refused) &&
                            sent_n == 0,
                    "[%s] workmode %d is outside the board range -> E/120 error:1, "
                    "nothing forwarded",
                    BOARDCONF_SECTION, m);
        snprintf(arg, sizeof arg, "work_mode_lvl,%d-0", m);
        r = ascset(arg, DAY);
        if (wm_allowed(m))
            CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && sent_n == 1 &&
                            sent_cmd.op == CTRL_OP_WORKMODE && sent_cmd.v.workmode.mode == m,
                    "[%s] work_mode_lvl %d-0 -> S/119, mode forwarded", BOARDCONF_SECTION, m);
        else
            CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                            strstr(r, "Msg=ASC 0 set failed: error:2 parameter error") &&
                            sent_n == 0,
                    "[%s] work_mode_lvl %d-0 outside the board range -> E/120 error:2",
                    BOARDCONF_SECTION, m);
    }
    static const char *const mode_names[] = { "low", "mid", "high", "medium", "set,high" };
    for (size_t i = 0; i < sizeof mode_names / sizeof mode_names[0]; i++) {
        char arg[32];
        snprintf(arg, sizeof arg, "workmode,%s", mode_names[i]);
        r = ascset(arg, DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: error:2 parameter error") && sent_n == 0,
                "[%s] workmode,%s is not a mode number -> E/120 error:2, nothing forwarded",
                BOARDCONF_SECTION, mode_names[i]);
    }

    g_adj      = 0;
    g_workmode = 0;
    r          = ascset("workmode,set,0", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r,
                            "Msg=ASC 0 set failed: current mode 0 is caling Don't permit "
                            "switch mode,") &&
                    sent_n == 0,
            "workmode while the running mode is not aged -> E/120 with the "
            "API text, nothing forwarded; the running mode itself included");
    r = ascset("work_mode_lvl,0-0", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: current mode 0 is caling. Don't permit switch mode,") &&
                    sent_n == 0,
            "work_mode_lvl refused the same way, with its own API text");
    if (wm_allowed(1) && wm_allowed(2)) {
        g_workmode = 2;
        r          = ascset("workmode,set,1", DAY);
        CHECK(strstr(r, "current mode 2 is caling Don't permit switch mode") && sent_n == 0,
                "the refusal names the running mode, not the requested one");
        g_maxmode = 1;
        r         = ascset("workmode,set,2", DAY);
        CHECK(strstr(r, "error:1 not support set workmode max_mode[1],") && sent_n == 0,
                "a mode above max_mode reports max_mode before the aging refusal");
        g_maxmode  = cali_mode_top(CALI_MODE_BOARD_MASK);
        g_workmode = 0;
    }
    r = ascset("worklevel,0", DAY);
    CHECK(strstr(r, "STATUS=S") && sent_n == 0,
            "worklevel is not gated: level 0 is the only level and changes nothing");
    g_adj = 2;
    r     = ascset("workmode,set,0", DAY);
    CHECK(strstr(r, "STATUS=S") && sent_n == 1 && sent_cmd.v.workmode.mode == 0,
            "a mode that finished aging this run may switch (ADJ 2)");
    g_adj      = 1;
    g_workmode = 0;

    r = ascset("worklevel,0", DAY);
    CHECK(strstr(r, "STATUS=S,When=0,Code=119,Msg=ASC 0 set OK,") && sent_n == 0,
            "worklevel,0 takes the level grammar -> S/119, answered by core, nothing forwarded");
    CHECK(strstr(g_oplog, "API setting worklevel accepted") != NULL,
            "...and its OP log says accepted, not forwarded (got \"%s\")", g_oplog);
    send_fail = 1;
    r         = ascset("worklevel,set,0", DAY);
    send_fail = 0;
    CHECK(strstr(r, "STATUS=S,When=0,Code=119,Msg=ASC 0 set OK,") && sent_n == 0,
            "worklevel,set,0 answers S/119 without the control channel");
    r = ascset("worklevel,1", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: worklevel unknown argument") && sent_n == 0,
            "worklevel,1 -> E/120 worklevel unknown argument (nano3s wording)");

    g_workmode = 1;
    r          = ascset("workmode,get", DAY);
    CHECK(strstr(r, "STATUS=I,When=0,Code=118,Msg=ASC 0 set info: workmode 1,") && sent_n == 0,
            "workmode,get -> I/118 workmode <running mode>, nothing forwarded (%s)", r);
    r = ascset("worklevel,get", DAY);
    CHECK(strstr(r, "STATUS=I,When=0,Code=118,Msg=ASC 0 set info: worklevel 0,") && sent_n == 0,
            "worklevel,get -> I/118 worklevel 0, nothing forwarded (%s)", r);
    r = ascset("work_mode_lvl,get", DAY);
    CHECK(strstr(r, "STATUS=I,When=0,Code=118,Msg=ASC 0 set info: workmode 1 worklevel 0,") &&
                    sent_n == 0,
            "work_mode_lvl,get -> I/118 workmode <mode> worklevel 0 (%s)", r);
    g_adj = 0;
    r     = ascset("workmode,get", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set info: workmode 1,"),
            "workmode,get answers while the running mode is aging (%s)", r);
    g_adj = 1;

    static const char *const not_get[] = { "workmode,GET", "workmode,get,1", "worklevel,Get",
        "work_mode_lvl,get,0" };
    for (size_t i = 0; i < sizeof not_get / sizeof not_get[0]; i++) {
        r = ascset(not_get[i], DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") && sent_n == 0,
                "%s is not the nano3s get form -> E/120, nothing forwarded (%s)", not_get[i], r);
    }
    CHECK(!mc_api_cmd_writes("ascset|0,workmode,get", "ascset") &&
                    !mc_api_cmd_writes("ascset|0,worklevel,get", "ascset") &&
                    !mc_api_cmd_writes(
                            "{\"command\":\"ascset\",\"parameter\":\"0,work_mode_lvl,get\"}",
                            "ascset"),
            "workmode,get, worklevel,get and work_mode_lvl,get only read: :4028 answers them");
    CHECK(mc_api_cmd_writes("ascset|0,workmode,1", "ascset") &&
                    mc_api_cmd_writes("ascset|0,worklevel,0", "ascset") &&
                    mc_api_cmd_writes("ascset|0,work_mode_lvl,1,0", "ascset") &&
                    mc_api_cmd_writes("ascset|0,workmode,GET", "ascset") &&
                    mc_api_cmd_writes("ascset|0,worklevel,get,1", "ascset"),
            "...a value or another get form writes: :4028 answers E/45");
    g_workmode = 0;

    void *vctx = NULL;
    CHECK(mc_api_setter_lookup("WORKMODE", &vctx) != NULL,
            "the registry matches \"WORKMODE\" case-insensitively");
    r = ascset("WORKMODE,0", DAY);
    CHECK(strstr(r, "STATUS=S,When=0,Code=119,Msg=ASC 0 set OK,") && sent_n == 1 &&
                    sent_cmd.op == CTRL_OP_WORKMODE && sent_cmd.v.workmode.mode == 0,
            "ascset 0,WORKMODE,0 applies and forwards op WORKMODE — the same as "
            "the lowercase spelling, not an Unknown option");
    r = ascset("WORKMODE", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: missing workmode setting"),
            "an argument-less case variant answers the captured missing-arg wording "
            "with the CANONICAL name — the reply never echoes the wire spelling");
    CHECK(sent_n == 0, "...and forwards nothing");
    r = ascset("Work_Mode_Lvl,0-0", DAY);
    CHECK(strstr(r, "STATUS=S,When=0,Code=119") && sent_n == 1 && sent_cmd.op == CTRL_OP_WORKMODE,
            "mixed case is folded too, not just all-upper");
    r = ascset("workmode", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing workmode setting"),
            "the registered spelling still gets the captured API missing-arg "
            "wording, so moving the guard past the op switch changed nothing the "
            "app can reach");
    r = ascset("worklevel", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing worklevel setting"), "...same for worklevel");
    r = ascset("work_mode_lvl", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing work_mode_lvl setting"),
            "...and for work_mode_lvl");
}

static void light_checks(void)
{
    const char *r;

    r = ascset("ledmode,1", DAY);
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=118") &&
                    strstr(r, "Msg=ASC 0 set info: success set:1"),
            "ledmode dark -> I/118 + success set:1");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_LEDMODE && sent_cmd.v.ledmode.mode == 1,
            "ledmode forwarded as DATA: mode %u", sent_cmd.v.ledmode.mode);
    r = ascset("ledmode,0", DARK);
    CHECK(strstr(r, "Msg=ASC 0 set info: success set:0"), "ledmode day -> success set:0");

    const char *bad_ledmode[] = { "2", "dark", "day", "0x1", "1abc", "-1" };
    for (unsigned i = 0; i < sizeof bad_ledmode / sizeof bad_ledmode[0]; i++) {
        char cmd[32];
        snprintf(cmd, sizeof cmd, "ledmode,%s", bad_ledmode[i]);
        r = ascset(cmd, DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: Input param error.") && sent_n == 0,
                "%s -> E/120 Input param error., not forwarded", cmd);
    }

    core_setters_ledmode_set(DAY);
    r = ascset("ledmode,1", DAY);
    CHECK(core_setters_ledmode() == DARK, "forwarded ledmode 1 updates core's light mode");
    r = ascset("nightlamp,1-180", core_setters_ledmode());
    CHECK(strstr(r, "Msg=ASC 0 set info: success set:1 180") && sent_n == 1,
            "a nightlamp right behind ledmode 1 is gated on dark, and forwarded");
    r = ascset("ledmode,0", DARK);
    CHECK(core_setters_ledmode() == DAY, "forwarded ledmode 0 updates core's light mode");
    r = ascset("nightlamp,1-180", core_setters_ledmode());
    CHECK(strstr(r, "Msg=ASC 0 set failed: not in (dark)mode now") && sent_n == 0,
            "a nightlamp right behind ledmode 0 is refused");
    r = ascset("ledmode,1abc", DAY);
    CHECK(core_setters_ledmode() == DAY, "a refused ledmode leaves core's light mode alone");
    send_fail = 1;
    r         = ascset("ledmode,1", DAY);
    send_fail = 0;
    CHECK(core_setters_ledmode() == DAY, "an unsent ledmode leaves core's light mode alone");

    r = ascset("ledset,2-100-100-0-0-255", DAY);
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=118") &&
                    strstr(r, "Msg=ASC 0 set info: led set ok"),
            "ledset -> I/118 + led set ok");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_LEDSET && sent_cmd.v.ledset.effect == 2 &&
                    sent_cmd.v.ledset.bright == 100 && sent_cmd.v.ledset.temper == 100 &&
                    sent_cmd.v.ledset.r == 0 && sent_cmd.v.ledset.g == 0 &&
                    sent_cmd.v.ledset.b == 255,
            "ledset forwarded as DATA, field by field");
    CHECK(strstr(g_oplog, "API setting ledset forwarded to mm_miner") != NULL,
            "a forwarded ledset logs forwarded (got \"%s\")", g_oplog);

    r = ascset("ledset,2-100-100-0-0-255", DARK);
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=118") &&
                    strstr(r, "Msg=ASC 0 set info: not in (day)mode now"),
            "ledset in dark mode -> I/118 + not in (day)mode now");
    CHECK(sent_n == 0, "a dark-mode ledset is not forwarded");
    CHECK(strstr(g_oplog, "API setting ledset accepted") != NULL,
            "...and its OP log does not claim it was forwarded (got \"%s\")", g_oplog);
    r = ascset("ledset,1", DARK);
    CHECK(strstr(r, "Msg=ASC 0 set info: not in (day)mode now") && sent_n == 0,
            "the mode gate runs before the arguments are parsed");

    const char *bad_ledset[] = {
        "5-100-100-0-0-255",
        "1",
        "3,80,50,16711680",
        "1,100,100,0,0,255",
        "1-100-100-0-0",
        "1-100-100-0-0-255-0",
        "1-100-100-0-0-256",
        "1-101-100-0-0-255",
        "1-100-100-0-0-0xff",
    };
    for (unsigned i = 0; i < sizeof bad_ledset / sizeof bad_ledset[0]; i++) {
        char cmd[48];
        snprintf(cmd, sizeof cmd, "ledset,%s", bad_ledset[i]);
        r = ascset(cmd, DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: led parameter wrong") && sent_n == 0,
                "%s -> E/120 led parameter wrong, not forwarded", cmd);
    }

    r = ascset("nightlamp,1-180", DARK);
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=118") &&
                    strstr(r, "Msg=ASC 0 set info: success set:1 180"),
            "nightlamp -> I/118 + success set:1 180");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_NIGHTLAMP && sent_cmd.v.nightlamp.mode == 1 &&
                    sent_cmd.v.nightlamp.duration_s == 180,
            "nightlamp forwarded as DATA: mode %u dur %u", sent_cmd.v.nightlamp.mode,
            sent_cmd.v.nightlamp.duration_s);
    r = ascset("nightlamp,0-180", DARK);
    CHECK(strstr(r, "Msg=ASC 0 set info: success set:0 180"), "nightlamp off echoes mode 0");
    r = ascset("nightlamp,1-0", DARK);
    CHECK(strstr(r, "Msg=ASC 0 set info: success set:1 0") && sent_n == 1 &&
                    sent_cmd.v.nightlamp.duration_s == 0,
            "nightlamp 1-0 echoes and forwards 0; mm applies the 180 s default");

    r = ascset("nightlamp,1-180", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: not in (dark)mode now"),
            "nightlamp outside dark mode -> E/120 + not in (dark)mode now");
    CHECK(sent_n == 0, "a day-mode nightlamp is not forwarded");
    r = ascset("nightlamp", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing nightlamp setting") && sent_n == 0,
            "a missing argument is reported before the mode gate, as the API "
            "dispatcher does");

    const char *bad_nightlamp[] = { "1,180", "1", "1-", "2-180", "1-70000", "1-60-1", "dark" };
    for (unsigned i = 0; i < sizeof bad_nightlamp / sizeof bad_nightlamp[0]; i++) {
        char cmd[32];
        snprintf(cmd, sizeof cmd, "nightlamp,%s", bad_nightlamp[i]);
        r = ascset(cmd, DARK);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: Input param error.") && sent_n == 0,
                "%s -> E/120 Input param error., not forwarded", cmd);
    }

    r = ascset("ledmode,1", DARK);
    CHECK(strstr(r, "Msg=ASC 0 set info: success set:1") && sent_n == 1 &&
                    sent_cmd.op == CTRL_OP_LEDMODE,
            "ledmode in dark mode is NOT gated (ledset's half of the gate would fire)");
    r = ascset("ledmode,0", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set info: success set:0") && sent_n == 1 &&
                    sent_cmd.op == CTRL_OP_LEDMODE,
            "...nor in day mode (nightlamp's half would fire)");

    r = ascset("ledmode,2-100-100-0-0-255", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: Input param error.") && sent_n == 0,
            "ledmode refuses ledset's six-field grammar -> it is not in ledset's arm");

    r = ascset("LEDMODE,1", DARK);
    CHECK(strstr(r, "Msg=ASC 0 set info: success set:1") && sent_n == 1 &&
                    sent_cmd.op == CTRL_OP_LEDMODE,
            "ascset 0,LEDMODE,1 takes ledmode's own I/118 answer and forwards it");
    r = ascset("LEDMODE", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing ledmode setting") && sent_n == 0,
            "an argument-less LEDMODE answers the missing-arg wording with the canonical "
            "name: the handler takes the op rather than resolving its own name");

    r = ascset("ledmode", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing ledmode setting"),
            "missing ledmode arg -> E/120");

    send_fail = 1;
    r         = ascset("ledmode,1", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: control channel down"),
            "channel down -> E/120, not the success echo");
    send_fail = 0;
}

static void reboot_checks(const char *cfg)
{
    const char *r;

    g_log[0] = 0;
    r        = ascset("reboot,3", DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && strstr(r, "Msg=ASC 0 set OK"),
            "reboot,3 -> S/119 bare set OK");
    CHECK(strstr(g_log, "API setting reboot for unit 0, arguments (1 bytes): 3") != NULL,
            "reboot logs its delay (log \"%s\")", g_log);
    CHECK(!strstr(r, "failed") && !strstr(r, "Auth Failed"),
            "reboot reply carries neither of the app's failure substrings");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_REBOOT &&
                    sent_cmd.v.reboot.reason == MC_BOOTBY_API && sent_cmd.v.reboot.sub == 0,
            "reboot,3 forwards a CTRL_OP_REBOOT request to mm (reason API)");
    CHECK(strstr(g_oplog, "API setting reboot forwarded to mm_miner") != NULL,
            "reboot logs forwarded (got \"%s\")", g_oplog);
    CHECK(sent_cmd.v.reboot.delay_s == 3, "reboot,3 carries its 3 s delay to mm (got %u)",
            (unsigned)sent_cmd.v.reboot.delay_s);
    r = ascset("reboot,0", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set OK") && sent_n == 1 && sent_cmd.v.reboot.delay_s == 0,
            "reboot,0 forwards delay 0 (got %u)", (unsigned)sent_cmd.v.reboot.delay_s);
    r = ascset("reboot,300", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set OK") && sent_n == 1 && sent_cmd.v.reboot.delay_s == 300 &&
                    sent_cmd.v.reboot.reason == MC_BOOTBY_API,
            "reboot,300 forwards the top delay with reason API (got %u)",
            (unsigned)sent_cmd.v.reboot.delay_s);
    const char *bad_reboots[] = { "reboot,-1", "reboot,3junk", "reboot,65536",
        "reboot,999999999999999999999", "reboot, 3" };
    for (size_t i = 0; i < sizeof bad_reboots / sizeof bad_reboots[0]; i++) {
        r = ascset(bad_reboots[i], DAY);
        CHECK(strstr(r, "failed") && sent_n == 0, "invalid reboot delay rejected");
    }
    send_fail = 1;
    r         = ascset("reboot,3", DAY);
    CHECK(strstr(r, "failed"), "reboot IPC failure is reported");
    send_fail = 0;
    r         = ascset("reboot,all", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: reboot value not number") && sent_n == 0,
            "reboot,all rejected (non-numeric), nothing forwarded");
    r = ascset("reboot", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing reboot setting") && sent_n == 0,
            "reboot without a delay is refused and forwards nothing");
    r = ascset("reboot,301", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: reboot value should be 0~300 seconds") && sent_n == 0,
            "reboot above the delay limit is refused and forwards nothing");
    remove(cfg);
    r = ascset("reboot,3", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set OK") && sent_n == 1 && sent_cmd.op == CTRL_OP_REBOOT,
            "reboot works without systemcfg.ini");

    mc_api_set_peer_trusted(0);
    r = ascset("reboot_web", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: Invalid command") && sent_n == 0,
            "reboot_web refuses a request off the trusted channel and forwards nothing");
    mc_api_set_peer_trusted(1);
    r = ascset("reboot_web", DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && strstr(r, "Msg=ASC 0 set OK"),
            "reboot_web on the trusted channel -> S/119 bare set OK");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_REBOOT &&
                    sent_cmd.v.reboot.reason == MC_BOOTBY_WEB && sent_cmd.v.reboot.sub == 0 &&
                    sent_cmd.v.reboot.delay_s == 0,
            "reboot_web forwards a CTRL_OP_REBOOT request to mm (reason WEB, no delay)");
    CHECK(strstr(g_oplog, "API setting reboot_web forwarded to mm_miner") != NULL,
            "reboot_web logs forwarded (got \"%s\")", g_oplog);
    send_fail = 1;
    r         = ascset("reboot_web", DAY);
    CHECK(strstr(r, "failed"), "web reboot IPC failure is reported");
    send_fail = 0;
    mc_api_set_peer_trusted(0);
}

static void soft_checks(void)
{
    const char *r;

    r = topcmd("softoff", NULL);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && strstr(r, "success softoff now"),
            "bare softoff -> the immediate form");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_SOFTOFF && sent_cmd.v.soft.immediate == 1,
            "...forwarded as DATA: immediate=1");

    r = topcmd("softon", "1755000000");
    CHECK(strstr(r, "STATUS=S") && strstr(r, "success softon at 1755000000"),
            "softon|<epoch> -> armed, and the reply echoes the second");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_SOFTON && sent_cmd.v.soft.valid == 1 &&
                    sent_cmd.v.soft.ts == 1755000000,
            "...forwarded as DATA: valid=1 ts=1755000000, already parsed for mm");

    r = topcmd("softon", "T:1755000000");
    CHECK(strstr(r, "STATUS=S") && strstr(r, "success softon at 1755000000") && sent_n == 1 &&
                    sent_cmd.v.soft.ts == 1755000000,
            "softon|T:<epoch> -> armed: the t: prefix is case-insensitive like now");

    r = topcmd("softoff", "0,1755000000");
    CHECK(strstr(r, "success softoff cancelled") != NULL, "valid=0 reads as a cancel");
    CHECK(sent_n == 1, "...and is still forwarded (mm holds the armed state)");

    r = topcmd("softon", "tomorrow");
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120"), "an unparseable payload -> E/120");
    CHECK(sent_n == 0, "...and nothing reaches mm");

    r = topcmd("softoff", "4133981573");
    CHECK(strstr(r, "STATUS=S") && strstr(r, "success softoff at 4133981573"),
            "an epoch after 2100 is armed");
    CHECK(sent_n == 1 && sent_cmd.v.soft.ts == 4133981573LL, "...and forwarded to mm");

    r = topcmd("softoff", "99999999999999999999");
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120"), "an int64 overflow -> E/120");
    CHECK(sent_n == 0, "...and nothing reaches mm");

    send_fail = 1;
    r         = topcmd("softoff", NULL);
    CHECK(strstr(r, "control channel down") != NULL,
            "a down control channel is reported, not silently accepted");
    send_fail = 0;

    r = topcmd_json("softoff", NULL);
    CHECK(strstr(r, "unknown/unsupported command") == NULL,
            "json softoff is not an unknown command");
    CHECK(strstr(r, "\"STATUS\":\"S\"") && strstr(r, "\"Code\":119") &&
                    strstr(r, "\"Msg\":\"success softoff now\""),
            "...it answers the JSON STATUS envelope, same letter/code/text as text mode");
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_SOFTOFF && sent_cmd.v.soft.immediate == 1,
            "...and forwards to mm exactly as the text form does");

    r = topcmd_json("softon", "1755000000");
    CHECK(strstr(r, "\"Msg\":\"success softon at 1755000000\"") != NULL,
            "json softon echoes the armed second");
    CHECK(sent_n == 1 && sent_cmd.v.soft.valid == 1 && sent_cmd.v.soft.ts == 1755000000,
            "...payload read from the JSON \"parameter\", not a text tail");

    r = topcmd_json("softon", "tomorrow");
    CHECK(strstr(r, "\"STATUS\":\"E\"") && strstr(r, "\"Code\":120"),
            "a json refusal keeps the E/120 letter and code");
    CHECK(sent_n == 0, "...and nothing reaches mm");

    {
        uint32_t seq0;

        r    = topcmd("softoff", NULL);
        seq0 = sent_cmd.seq;
        r    = topcmd("softon", NULL);
        CHECK(seq0 != 0 && sent_cmd.seq != 0 && sent_cmd.seq != seq0,
                "each forwarded soft command carries its own nonzero seq (%u, %u)", seq0,
                sent_cmd.seq);

        ack_reply = (mc_ctrl_ack){
            .result = MC_SET_ERR_ARG, .why = MC_CTRL_WHY_PAST, .arg = 1793000000
        };
        r = topcmd("softoff", "1,1755000000");
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r,
                                "softoff refused: time 1755000000 is past (board time 1793000000)"),
                "mm refusing a past time -> E/120 with both times, not success (%s)", r);

        ack_reply = (mc_ctrl_ack){
            .result = MC_SET_ERR_STATE, .why = MC_CTRL_WHY_COOLDOWN, .arg = 120
        };
        r = topcmd("softon", NULL);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "softon refused: over-temperature cool-down 120 s left"),
                "softon in the cool-down -> E/120 with the seconds left (%s)", r);

        ack_reply = (mc_ctrl_ack){
            .result = MC_SET_ERR_STATE, .why = MC_CTRL_WHY_FAULT, .arg = MC_SOFTOFF_FAN_ERR
        };
        r = topcmd("softon", NULL);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r,
                                "softon refused: fault stop SoftOFF[5] needs softoff then softon"),
                "a lone softon in a fault stop -> E/120 naming the way out (%s)", r);

        r = topcmd_json("softon", NULL);
        CHECK(strstr(r, "\"STATUS\":\"E\"") && strstr(r, "\"Code\":120") &&
                        strstr(r, "fault stop SoftOFF[5]"),
                "...the JSON form answers the same refusal (%s)", r);

        ack_reply = (mc_ctrl_ack){ .result = MC_SET_ERR_UNSUP };
        r         = topcmd("softon", NULL);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "softon refused by mm_miner (result 2)"),
                "a refusal without a reason still answers E/120 (%s)", r);

        ack_reply = (mc_ctrl_ack){ .result = MC_SET_OK };
        ack_stale = 1;
        r         = topcmd("softoff", NULL);
        CHECK(strstr(r, "STATUS=S") && strstr(r, "success softoff now"),
                "an ACK for an earlier seq is skipped; the matching one answers (%s)", r);
        ack_stale = 0;

        ack_silent = 1;
        r          = topcmd("softoff", NULL);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "softoff not confirmed: no answer from mm_miner"),
                "no ACK within %u ms -> E/120, never success (%s)", CORE_CTRL_ACK_MS, r);
        ack_silent = 0;
    }
}

int main(void)
{
    (void)mkdir(CORE_SETTERS_FIXTURE_DIR, 0755);
    g_maxmode = cali_mode_top(CALI_MODE_BOARD_MASK);

    const char *cfg = CS_FIX("test_core_setters_syscfg.ini");
    FILE       *f   = fopen(cfg, "wb");
    fprintf(f,
            "[syscfg]\nwebuser=admin\n"
            "webpass=8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918\n");
    fclose(f);
    core_webpass_cache_set("8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918");

    int want_reg;
    if (!strcmp(BOARDCONF_SECTION, "NANO3S")) {
        g_fitted   = FITTED_NANO3S;
        g_fitted_n = 8;
        want_reg   = 13;
    } else if (!strcmp(BOARDCONF_SECTION, "X3")) {
        g_fitted   = FITTED_X3;
        g_fitted_n = 3;
        want_reg   = 8;
    } else {
        printf("FAIL: no expected setter set for descriptor [%s] — count it by "
               "hand off that board's own CAP_* and BOARD_CTRL_OPS\n",
                BOARDCONF_SECTION);
        return 1;
    }
#ifdef CAP_ACTIVATION
    want_reg++;
#endif

    mc_api_setter_reset();
    CHECK(core_setters_register(cfg) == want_reg,
            "the %d setters this descriptor declares register — every core-answered "
            "one plus one per CTRL_CMD_ASCSET row whose capability the board has",
            want_reg);

    CHECK((mc_api_setter_lookup("reboot", NULL) != NULL) == ctrl_cmd_available(CTRL_OP_REBOOT) &&
                    (mc_api_setter_lookup("reboot_web", NULL) != NULL) ==
                            ctrl_cmd_available(CTRL_OP_REBOOT),
            "reboot and reboot_web register only where the board offers the op");

    CTRL_CMD_ASCSET(CAP_ROW)
    CAP_ROW(WORKMODE, "worklevel", workmode, 1)
    CAP_ROW(WORKMODE, "work_mode_lvl", workmode, 1)

    CTRL_CMD_ASCSET(CASE_ROW)
    CASE_ROW(WORKMODE, "worklevel", workmode, 1)
    CASE_ROW(WORKMODE, "work_mode_lvl", workmode, 1)

    const char *r;

    if (ctrl_cmd_available(CTRL_OP_WORKMODE))
        workmode_checks();
    if (ctrl_cmd_available(CTRL_OP_LEDMODE) && ctrl_cmd_available(CTRL_OP_LEDSET) &&
            ctrl_cmd_available(CTRL_OP_NIGHTLAMP))
        light_checks();

    g_dna                  = "0201000012345678";
    const char *good_nonce = "9abcdef0";
    const char *qr_token   = "9abcdef0aaaaaaaaaaaaaaaaaaaaaaaa";
    CHECK(mc_api_qr_login_begin(qr_token), "browser QR session registered");
    const char *good_verify = "ff0000ee29377f60bb1973cec9f356da";
    char        tail[600];

    r = ascset("qr_auth", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing qr_auth setting"),
            "qr_auth without args -> missing qr_auth setting");

    g_oplog[0] = '\0';
    r          = ascset("qr_auth,9abcdef0", DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && strstr(r, "Msg=ASC 0 set OK"),
            "qr_auth without a comma -> S/119 set OK (nano3s sscanf ret 1)");
    CHECK(g_oplog[0] == '\0', "...and logs no OP line (got \"%s\")", g_oplog);
    snprintf(tail, sizeof tail, "qr_auth,%s,", good_nonce);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119"),
            "qr_auth with an empty verify field -> S/119 set OK");
    snprintf(tail, sizeof tail, "qr_auth,%s,,%s", good_nonce, good_verify);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119"),
            "qr_auth with the verify field after two commas -> S/119 set OK");
    snprintf(tail, sizeof tail, "qr_auth,,%s", good_verify);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119"),
            "qr_auth with an empty nonce field -> S/119 set OK (nano3s sscanf ret 0)");
    CHECK(g_oplog[0] == '\0', "an unparsed qr_auth logs no OP line (got \"%s\")", g_oplog);
    CHECK(sent_n == 0, "an unparsed qr_auth is never forwarded to mm");
    CHECK(mc_api_qr_login_take(qr_token) == 0, "an unparsed qr_auth approves nothing");

    snprintf(tail, sizeof tail, "qr_auth,%s,%0128d", good_nonce, 0);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Msg=ASC 0 set failed: input length error!"),
            "qr_auth with a 128-char verify field -> input length error!");
    snprintf(tail, sizeof tail, "qr_auth,%s,%0127d", good_nonce, 0);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: passwd error 02!"),
            "qr_auth with a 127-char verify field -> passwd error 02!");

    snprintf(tail, sizeof tail, "qr_auth,c925a220,%s", good_verify);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Msg=ASC 0 set failed: auth error 01!"),
            "qr_auth with a stale nonce -> auth error 01!");

    snprintf(tail, sizeof tail, "qr_auth,%s,ff0000ee00000000000000000000000000", good_nonce);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: passwd error 02!"),
            "qr_auth with the wrong password -> passwd error 02!");

    snprintf(tail, sizeof tail, "qr_auth,%s,%.31s", good_nonce, good_verify);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: passwd error 02!"),
            "qr_auth with the verify string one char short -> passwd error 02!");
    snprintf(tail, sizeof tail, "qr_auth,%s,%s0", good_nonce, good_verify);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: passwd error 02!"),
            "qr_auth with the verify string one char long -> passwd error 02!");

    snprintf(tail, sizeof tail, "qr_auth,%s,%s", good_nonce, good_verify);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && strstr(r, "Msg=ASC 0 set OK"),
            "qr_auth with the proof over this DNA and webpass -> S/119 set OK");
    CHECK(strstr(g_oplog, "API setting qr_auth accepted") != NULL,
            "...and logs accepted (got \"%s\")", g_oplog);
    CHECK(sent_n == 0, "qr_auth is answered by core, never forwarded to mm");

    CHECK(mc_api_qr_login_take(qr_token) == 1, "the approval is pending after qr_auth");
    CHECK(mc_api_qr_login_take(qr_token) == 0, "... and is consumed by the first take");

    CHECK(mc_api_qr_login_begin(qr_token), "new browser QR session registered");
    snprintf(tail, sizeof tail, "qr_auth,%s,ff0000ee00000000000000000000000000", good_nonce);
    ascset(tail, DAY);
    CHECK(mc_api_qr_login_take(qr_token) == 0, "a refused qr_auth approves nothing");
    g_dna = "d";

    r = ascset("password", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: missing password setting"),
            "password without args -> E/120 missing");
    r = ascset("password,adminonly", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: we need 2 arguments? your input is 1 arguments."),
            "password without a comma -> E/120 nano3s argument count");
    r = ascset("password,,new", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: we need 2 arguments? your input is 0 arguments."),
            "empty old password -> E/120 nano3s argument count 0");
    r = ascset("password,admin,new,extra", DAY);
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Msg=ASC 0 set info: new password success") &&
                    !strcmp(stored_webpass(cfg), hex_of("new")),
            "a third field is ignored (nano3s sscanf): new password = second field");
    r = ascset("password,new,admin,", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set info: new password success") &&
                    !strcmp(stored_webpass(cfg), hex_of("admin")),
            "a trailing comma after the new password is ignored too");
    r = ascset("password,admin,", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: we need 2 arguments? your input is 1 arguments."),
            "empty new password -> E/120 nano3s argument count 1");

    char pw257[258];
    memset(pw257, 'a', 257);
    pw257[257] = 0;
    snprintf(tail, sizeof tail, "password,%s,newpw", pw257);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: password too long"),
            "257-char old password -> E/120 too long, not a silent cut");
    snprintf(tail, sizeof tail, "password,admin,%s", pw257);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: password too long"),
            "257-char new password -> E/120 too long");

    r = ascset("password,wrongpass,hunter2", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: Auth Failed"),
            "wrong old password -> E/120 Auth Failed");
    CHECK(!strcmp(stored_webpass(cfg), hex_of("admin")),
            "a refused change leaves webpass untouched");
    CHECK(sent_n == 0, "password is answered by core, never forwarded to mm");

    r = ascset("password,admin,hunter2", DAY);
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=118") &&
                    strstr(r, "Msg=ASC 0 set info: new password success"),
            "correct old password -> I/118 new password success");
    CHECK(!strcmp(stored_webpass(cfg), hex_of("hunter2")), "stored webpass == sha256(new)");
    CHECK(sent_n == 0, "an applied password change is not forwarded either");

    r = ascset("password,admin,other", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: Auth Failed"), "the old password is dead immediately");
    ini_t *chk = ini_load(cfg);
    CHECK(chk && ini_get(chk, "syscfg", "webuser") &&
                    !strcmp(ini_get(chk, "syscfg", "webuser"), "admin"),
            "webuser survived the password rewrite");
    ini_free(chk);

    char pw65[66];
    memset(pw65, 'b', 65);
    pw65[65] = 0;
    snprintf(tail, sizeof tail, "password,hunter2,%s", pw65);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: password too long"), "65-char new password rejected");
    char pw64[65];
    memset(pw64, 'b', 64);
    pw64[64] = 0;
    snprintf(tail, sizeof tail, "password,hunter2,%s", pw64);
    r = ascset(tail, DAY);
    CHECK(strstr(r, "Msg=ASC 0 set info: new password success"), "64-char new password accepted");
    CHECK(!strcmp(stored_webpass(cfg), hex_of(pw64)), "stored webpass == sha256(64-char pw)");

    cfgwr_webpass_fail = 1;
    snprintf(tail, sizeof tail, "password,%s,failpw", pw64);
    r                  = ascset(tail, DAY);
    cfgwr_webpass_fail = 0;
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: config write error"),
            "mm's config writer refusing the webpass -> E/120 config write error");
    CHECK(!strcmp(stored_webpass(cfg), hex_of(pw64)), "...the stored webpass is unchanged");
    CHECK(core_webpass_matches(pw64, strlen(pw64)) &&
                    !core_webpass_matches("failpw", strlen("failpw")),
            "...and the live password is still the old one, not the refused one");
    r = ascset("password,failpw,x", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: Auth Failed"),
            "...so the refused password authenticates nothing on :4028");

    mc_api_set_peer_trusted(1);
    snprintf(tail, sizeof tail, "0,password,%s,a\\\\b\\\"c\\nd\\u00e9", pw64);
    r = topcmd_json("ascset", tail);
    CHECK(strstr(r, "\"STATUS\":\"I\"") && strstr(r, "new password success") &&
                    !strcmp(stored_webpass(cfg), hex_of("a\\b\"c\nd\xc3\xa9")),
            "a json-escaped new password is stored as the hash of the decoded string (%s)", r);
    CHECK(core_webpass_matches("a\\b\"c\nd\xc3\xa9", strlen("a\\b\"c\nd\xc3\xa9")),
            "...and the same plain text then authenticates");
    snprintf(tail, sizeof tail, "0,password,a\\\\b\\\"c\\nd\\u00e9,%s", pw64);
    r = topcmd_json("ascset", tail);
    CHECK(strstr(r, "new password success") && !strcmp(stored_webpass(cfg), hex_of(pw64)),
            "...also as the json-escaped old password of the next change (%s)", r);
    mc_api_set_peer_trusted(0);

    {
        char wrong_reply[512];
        tst_step = 0;
        tst_now += 60000;
        for (int i = 0; i < 4; i++)
            ascset("password,wrongpass,x", DAY);
        snprintf(tail, sizeof tail, "password,%s,%s", pw64, pw64);
        r = ascset(tail, DAY);
        CHECK(strstr(r, "new password success"),
                "4 wrong passwords then the right one -> accepted, no lockout yet");
        for (int i = 0; i < 5; i++)
            r = ascset("password,wrongpass,x", DAY);
        snprintf(wrong_reply, sizeof wrong_reply, "%s", r);
        snprintf(tail, sizeof tail, "password,%s,%s", pw64, pw64);
        r = ascset(tail, DAY);
        CHECK(strstr(r, "Msg=ASC 0 set failed: Auth Failed") &&
                        !strcmp(strstr(r, "Msg="), strstr(wrong_reply, "Msg=")),
                "5 wrong passwords in 60 s -> the right one is refused with the "
                "wrong-password reply");
        CHECK(!core_webpass_matches(pw64, strlen(pw64)),
                "...every password check fails during the lockout");
        tst_now += 59999;
        CHECK(!core_webpass_matches(pw64, strlen(pw64)),
                "...still locked 1 ms before the 60 s are up");
        tst_now += 1;
        r = ascset(tail, DAY);
        CHECK(strstr(r, "new password success"), "...and accepted once the 60 s lockout ends");
        for (int i = 0; i < 4; i++)
            ascset("password,wrongpass,x", DAY);
        tst_now += 60000;
        r = ascset("password,wrongpass,x", DAY);
        r = ascset(tail, DAY);
        CHECK(strstr(r, "new password success"),
                "failures more than 60 s apart do not add up to a lockout");

        g_dna = "0201000012345678";
        CHECK(mc_api_qr_login_pending(good_nonce), "the QR session is still pending");
        snprintf(tail, sizeof tail, "qr_auth,%s,ff0000ee00000000000000000000000000", good_nonce);
        for (int i = 0; i < 5; i++)
            r = ascset(tail, DAY);
        CHECK(strstr(r, "Msg=ASC 0 set failed: passwd error 02!") &&
                        !core_webpass_proof_matches("p", "p"),
                "5 wrong QR proofs in 60 s lock the proof check");
        snprintf(tail, sizeof tail, "password,%s,%s", pw64, pw64);
        r = ascset(tail, DAY);
        CHECK(strstr(r, "new password success") && core_webpass_matches(pw64, strlen(pw64)),
                "...but not the password check of ascset password");
        g_dna = "d";
        tst_now += 60000;
        CHECK(core_webpass_proof_matches("p", "p"), "...and the proof lock ends after 60 s");
        for (int i = 0; i < 5; i++)
            core_webpass_matches("wrongpass", strlen("wrongpass"));
        CHECK(!core_webpass_matches(pw64, strlen(pw64)) && core_webpass_proof_matches("p", "p"),
                "5 wrong passwords lock the password check, not the proof check");
        tst_now += 60000;
        CHECK(core_webpass_matches(pw64, strlen(pw64)), "...until the 60 s lockout ends");
        tst_step = 60000;
    }
    mc_api_set_peer_trusted(1);
    r = ascset("password,wrongpass,x", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: Auth Failed") &&
                    !strcmp(stored_webpass(cfg), hex_of(pw64)),
            "the trusted channel still needs the old password to change it");
    mc_api_set_peer_trusted(0);

    f = fopen(cfg, "wb");
    fprintf(f, "[syscfg]\nwebuser=admin\nwebpass=%0*d\n", 64, 0);
    fclose(f);
    {
        char z[65];
        memset(z, '0', 64);
        z[64] = 0;
        core_webpass_cache_set(z);
    }
    {
        char pc[65] = "x";
        CHECK(core_webpass_cached_copy(pc, sizeof pc) == -1 && !pc[0],
                "the unset sentinel is not handed out as a cached hash");
    }
    r = ascset("password,admin,hunter2", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: Auth Failed"),
            "unset webpass sentinel -> Auth Failed (fail-closed)");

    remove(cfg);
    r = ascset("password,admin,hunter2", DAY);
    CHECK(strstr(r, "Msg=ASC 0 set failed: Auth Failed"),
            "missing systemcfg -> Auth Failed (fail-closed)");

    remove(cfg);
    remove(CS_FIX("test_core_setters_syscfg.ini.lock"));
    remove(CS_FIX("test_core_setters_syscfg.ini.tmp"));

    f = fopen(cfg, "wb");
    fprintf(f, "[syscfg]\nwebuser=admin\nwebpass=%s\n", hex_of("hunter3"));
    fclose(f);
    core_webpass_cache_set(hex_of("hunter3"));
    {
        char pc[65], small[64] = "x";
        CHECK(core_webpass_cached_copy(pc, sizeof pc) == 0 && !strcmp(pc, hex_of("hunter3")),
                "core_webpass_cached_copy() returns the live hash");
        CHECK(core_webpass_cached_copy(small, sizeof small) == -1 && !small[0],
                "...and a buffer short of 65 bytes gets nothing, not a cut hash");
    }

    if (ctrl_cmd_available(CTRL_OP_REBOOT))
        reboot_checks(cfg);

#ifdef CAP_ACTIVATION
    const char *act_ini = CS_FIX("activation.ini");
    remove(act_ini);

    r = ascset("activate", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: missing activate setting"),
            "activate without args -> E/120 missing activate setting");
    CHECK(sent_n == 0, "a bare activate is not forwarded");

    snprintf(tail, sizeof tail, "activate,%s", "00112233");
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: set activation param error"),
            "a too-short signature -> E/120 set activation param error");
    CHECK(stored_activation(act_ini) == 0, "a refused activate persists nothing");

    g_dna = "0201000012345678";
    snprintf(tail, sizeof tail, "activate,%s", activation_hex("0201000012345679"));
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: set activation param error"),
            "a signature for another DNA -> E/120 set activation param error");
    CHECK(stored_activation(act_ini) == 0, "a refused signature persists nothing");

    snprintf(tail, sizeof tail, "activate,%s", activation_hex(g_dna));
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=S") && strstr(r, "Code=119") && strstr(r, "Msg=ASC 0 set OK"),
            "a signature for this DNA -> S/119 bare set OK");
    CHECK(!strstr(r, "failed"), "the activate reply carries no failure substring");
    CHECK(sent_n == 0, "activate is answered by core, never forwarded to mm");
    CHECK(stored_activation(act_ini) == 1, "activation.ini now reads activation = 1");
    CHECK(!strcmp(cfgwr_activation_path, act_ini),
            "activate hands mm the activation.ini beside the registered systemcfg");
    remove(act_ini);

    cfgwr_activation_fail = 1;
    snprintf(tail, sizeof tail, "activate,%s", activation_hex(g_dna));
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: config write error"),
            "activate with mm refusing the write -> E/120 config write error");
    CHECK(stored_activation(act_ini) == 0, "a refused cfgwr activation write persists nothing");
    cfgwr_activation_fail = 0;
    g_dna                 = "d";

    remove(act_ini);
    remove(CS_FIX("activation.ini.lock"));
    remove(CS_FIX("activation.ini.tmp"));
#else
    CHECK(mc_api_setter_lookup("activate", NULL) == NULL,
            "activate is not in the ascset registry without CAP_ACTIVATION");
    snprintf(tail, sizeof tail, "activate,%s", activation_hex(g_dna));
    r = ascset(tail, DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: Unknown option: activate"),
            "activate without CAP_ACTIVATION -> E/120 Unknown option");
    CHECK(sent_n == 0 && !cfgwr_activation_path[0] &&
                    stored_activation(CS_FIX("activation.ini")) == 0,
            "activate without CAP_ACTIVATION writes nothing");
#endif

    r = ascset("ip,dhcp", DAY);
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=ASC 0 set failed: Unknown option: ip"),
            "this board's API has no `ip`, so a well-formed request is still answered "
            "Unknown option");
    CHECK(sent_n == 0, "...and nothing reaches mm");

    if (fitted_here("filter-clean")) {
        r = ascset("filter-clean,1", DAY);
        CHECK(strstr(r, "STATUS=S,When=0,Code=119,Msg=ASC 0 set OK,"), "filter-clean 1 -> S/119");
        CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_FILTER_CLEAN && !strcmp(sent_cmd.v.text, "1"),
                "filter-clean forwarded as DATA (text payload, verbatim)");
        r = ascset("filter-clean,0", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: Input param error."),
                "filter-clean 0 -> E/120 Input param error.");
        CHECK(sent_n == 0, "a value other than 1 is not forwarded");
        r = ascset("filter-clean,x", DAY);
        CHECK(strstr(r, "Msg=ASC 0 set failed: No value passed to avalon-filter-clean"),
                "a non-numeric filter-clean -> E/120 No value passed");
        r = ascset("filter-clean", DAY);
        CHECK(strstr(r, "Msg=ASC 0 set failed: missing filter-clean setting"),
                "missing filter-clean arg -> E/120 (API dispatcher wording)");
    }

    loop_checks();

    if (fitted_here("lcd")) {
        r = ascset("lcd,1:50", DAY);
        CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=118") &&
                        strstr(r, "Msg=ASC 0 set info: success lcd:id[1] val[50] len=4"),
                "lcd 1:50 -> I/118 + success lcd echo");
        CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_LCD && !strcmp(sent_cmd.v.text, "1:50"),
                "lcd forwarded as DATA (text payload, verbatim)");
        const char *invalid[] = { "lcd,0:2", "lcd,0:-1", "lcd,1:65536", "lcd,1:50junk" };
        for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
            r = ascset(invalid[i], DAY);
            CHECK(strstr(r, "STATUS=E") && sent_n == 0,
                    "invalid LCD request rejected before IPC: %s", invalid[i]);
        }
        r = ascset("lcd,3", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                        strstr(r, "Msg=ASC 0 set failed: No value passed to lcd"),
                "lcd with one field -> E/120 No value passed to lcd");
        CHECK(sent_n == 0, "a malformed lcd is not forwarded");
        r = ascset("lcd", DAY);
        CHECK(strstr(r, "Msg=ASC 0 set failed: missing lcd setting"),
                "missing lcd arg -> E/120 (API dispatcher wording)");
    }

    {
        const char *want = "Msg=ASC 0 set failed: Unknown option: no-such-setting";

        CHECK(mc_api_setter_lookup("no-such-setting", NULL) == NULL,
                "no-such-setting is not in the ascset registry without an API plugin");
        r = ascset("no-such-setting,1", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") && strstr(r, want) && sent_n == 0,
                "...so `ascset 0,no-such-setting,1` answers E/120 Unknown option and forwards "
                "nothing");
        r = ascset("no-such-setting", DAY);
        CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") && strstr(r, want) && sent_n == 0,
                "...and so does a bare `ascset 0,no-such-setting`");
    }

    {
        mc_api_stats  id = { 0 };
        mc_api_id_buf idb;
        static char   est[MC_API_REPLY_CAP];
        char          want[128];

        static core_hwcfg_buf hb;

        core_hwcfg_set("AvalonNano3s", "NANO3S");
        mc_api_identity_fill(&id, &idb);
        core_hwcfg_apply(&id, &hb);
        CHECK(!strcmp(id.prod, "AvalonNano3s") && !strcmp(id.model, "NANO3S"),
                "the pair mm published in mm_periph reaches the API snapshot");
        mc_api_stats_text(&id, est, sizeof est, 0, 0);
        snprintf(want, sizeof want, "MM ID0=Ver[NANO3S-%s]", id.version);
        CHECK(strstr(est, want), "...and estats MM ID0 opens %s", want);

        core_hwcfg_set("", "");
        CHECK(!strcmp(live_prod(), "AvalonNano3s"), "an empty push leaves the live pair standing");
        core_hwcfg_set(PROD_NAME, MODEL);
        CHECK(!strcmp(live_prod(), PROD_NAME) && !strcmp(live_model(), MODEL),
                "...and before any push the compile-time pair is the default");
        core_hwcfg_set("AvalonNano3s", "NANO3S");
    }

    CHECK(eeprom_touch_n == 0,
            "...and core_miner has NOT touched the power-board i2c bus once in "
            "this whole run (%d call(s)): that EEPROM sits on the bus mm_miner "
            "writes the ASIC core rail on, and the bus mutex is process-local, so "
            "a core-side read — at startup or here — can land inside a live rail "
            "transaction. Move the read back into core_miner and this fails",
            eeprom_touch_n);

    remove(cfg);

    g_ipc_core_mm = (osif_ipc_t *)1;
    CHECK(core_soft_setter_register() ==
                    ctrl_cmd_available(CTRL_OP_SOFTON) + ctrl_cmd_available(CTRL_OP_SOFTOFF),
            "softon and softoff register as top-level commands where the board offers the op");

    r = ascset("softoff", DAY);
    CHECK(strstr(r, "Unknown option: softoff") != NULL,
            "ascset 0,softoff still answers the captured Unknown option");
    CHECK(sent_n == 0, "...and forwards nothing");

    if (ctrl_cmd_available(CTRL_OP_SOFTON) && ctrl_cmd_available(CTRL_OP_SOFTOFF))
        soft_checks();

    stod_n = 0;
    r      = topcmd("time", "set,t:1787102966,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=Time set failed: invalid timezone,"),
            "a %d-char zone is refused by the settz forward -> E/120 (%s)", CTRL_TEXT_LEN, r);
    CHECK(sent_n == 0 && stod_n == 0, "...nothing reaches mm and the system time is NOT set");

    stod_n = 0;
    r      = topcmd("time", "set,t:1787102966,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=131") &&
                    strstr(r, "time t:1787102966 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
            "a %d-char zone is forwarded and the time set -> I/131 (%s)", CTRL_TEXT_LEN - 1, r);
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_SETTZ && stod_n == 1 &&
                    !strcmp(sent_cmd.v.text, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
            "...mm receives the zone as a SETTZ command and the system time is set");

    stod_n    = 0;
    ack_reply = (mc_ctrl_ack){ .result = MC_SET_ERR_ARG };
    r         = topcmd("time", "set,t:1787102966,Mars/Olympus");
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Code=120") &&
                    strstr(r, "Msg=Time set failed: invalid timezone,"),
            "a zone mm refuses -> E/120 with the app's failure word (%s)", r);
    CHECK(sent_n == 1 && sent_cmd.op == CTRL_OP_SETTZ && stod_n == 0,
            "...mm was asked and the system time is NOT set");
    ack_reply = (mc_ctrl_ack){ .result = MC_SET_OK };

    stod_n     = 0;
    ack_silent = 1;
    r          = topcmd("time", "set,t:1787102966,Asia/Shanghai");
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Msg=Time set failed: timezone not confirmed,") &&
                    stod_n == 0,
            "no ACK within %u ms -> E/120, system time NOT set (%s)", CORE_CTRL_ACK_MS, r);
    ack_silent = 0;

    stod_n    = 0;
    send_fail = 1;
    r         = topcmd("time", "set,t:1787102966,Asia/Shanghai");
    send_fail = 0;
    CHECK(strstr(r, "STATUS=E") && strstr(r, "Msg=Time set failed: timezone not confirmed,") &&
                    stod_n == 0,
            "control channel down -> E/120, system time NOT set (%s)", r);

    CHECK(ini_file_update(cfg, set_tz_ini, (void *)"Europe/Paris") == 0, "systemcfg zone set");
    r = topcmd("time", "get");
    CHECK(strstr(r, "STATUS=I") && strstr(r, "Code=131") && strstr(r, "time t:") &&
                    !strstr(r, "Europe/Paris"),
            "time get before any MSG_MM_PERIPH answers an empty zone and does not "
            "read systemcfg (%s)",
            r);
    core_setters_tz_set("Asia/Shanghai");
    r = topcmd("time", "get");
    CHECK(strstr(r, "time t:Asia/Shanghai") && !strstr(r, "Europe/Paris"),
            "time get answers the zone mm pushed, not the one in systemcfg (%s)", r);
    core_setters_tz_set(NULL);
    r = topcmd("time", "get");
    CHECK(strstr(r, "time t:") && !strstr(r, "Asia/Shanghai"), "a NULL zone clears the cache (%s)",
            r);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
