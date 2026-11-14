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
#include "mc_api_command.h"
#include "mc_api_qrlogin.h"
#include "mc_api_setter.h"
#include "mc_api_time.h"
#include "mc_api_estats.h"
#include "mc_api_lcd.h"
#include "jwrite.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* built-in API command names, NULL-terminated */
static const char *const common_cmds[] = { "version", "summary", "devs", "edevs", "pools", "coin",
    "config", "devdetails", "lcd", "stats", "estats", "litestats", "ascset", "time", "check",
    "lockstats", "qrlogin", NULL };

static size_t json_status_esc(char *buf, size_t cap, uint32_t when, char st, int code,
        const char *esc)
{
    char status[91 + MINER_VERSION_CAP + MC_API_LEGACY_MSG_CAP];
    snprintf(status, sizeof status,
            "[{\"STATUS\":\"%c\",\"When\":%lu,\"Code\":%d,\"Msg\":\"%s\","
            "\"Description\":\"AvalonMiner %s\"}]",
            st, (unsigned long)when, code, esc, AM_VER);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

_Static_assert(MC_API_MSG_CAP <= MC_API_LEGACY_MSG_CAP, "json_status_esc holds any Msg");
static size_t json_status(char *buf, size_t cap, uint32_t when, char st, int code, const char *msg)
{
    char esc[MC_API_MSG_CAP];
    json_escape(msg, esc, sizeof esc);
    return json_status_esc(buf, cap, when, st, code, esc);
}

size_t mc_api_status_reply(int is_json, char *buf, size_t cap, uint32_t when, char st, int code,
        const char *msg)
{
    return is_json ? json_status(buf, cap, when, st, code, msg) :
                     mc_api_text_status(buf, cap, when, st, code, msg);
}

size_t mc_api_status_reply_legacy(int is_json, char *buf, size_t cap, uint32_t when, char st,
        int code, const char *msg)
{
    char esc[MC_API_LEGACY_MSG_CAP];
    if (is_json) {
        json_escape(msg, esc, sizeof esc);
        return json_status_esc(buf, cap, when, st, code, esc);
    }
    size_t n = 0;
    for (; *msg; msg++) {
        int bs = (*msg == ',' || *msg == '|' || *msg == '=' || *msg == '\\');
        if (n + bs + 2 > sizeof esc)
            break;
        if (bs)
            esc[n++] = '\\';
        esc[n++] = *msg;
    }
    esc[n] = '\0';
    return (size_t)snprintf(buf, cap,
            "STATUS=%c,When=%lu,Code=%d,Msg=%s,Description=AvalonMiner %s|", st,
            (unsigned long)when, code, esc, AM_VER);
}

size_t mc_api_lockstats(int is_json, const mc_api_stats *s, char *buf, size_t cap)
{
    return mc_api_status_reply(is_json, buf, cap, s->when, 'W', 124, "Lock stats not enabled");
}

static int name_listed(const char *const *list, const char *name)
{
    for (; *list; list++)
        if (!strcasecmp(*list, name))
            return 1;
    return 0;
}

static int cmd_exists(const char *name)
{
    if (name_listed(common_cmds, name))
        return 1;
    return mc_api_command_lookup(name, NULL) != NULL;
}

static int cmd_writes(const char *name)
{
    if (!strcasecmp(name, "qrlogin"))
        return 1;
    return !name_listed(common_cmds, name) && mc_api_command_lookup(name, NULL) != NULL;
}

static const char *check_param(const char *req, char *out, size_t cap)
{
    if (!req || cap == 0)
        return NULL;
    const char *j = strstr(req, "\"parameter\"");
    if (j) {
        j = strchr(j, ':');
        if (j)
            j = strchr(j, '"');
        if (j) {
            j++;
            const char *end = strchr(j, '"');
            if (end && (size_t)(end - j) < cap) {
                memcpy(out, j, (size_t)(end - j));
                out[end - j] = '\0';
                return out;
            }
        }
        return NULL;
    }
    const char *bar = strchr(req, '|');
    if (bar && bar[1]) {
        bar++;
        size_t len = strlen(bar);
        while (len && (bar[len - 1] == '\n' || bar[len - 1] == '\r'))
            len--;
        if (!len)
            return NULL;
        if (len >= cap)
            len = cap - 1;
        memcpy(out, bar, len);
        out[len] = '\0';
        return out;
    }
    return NULL;
}

int mc_api_cmd_writes(const char *req, const char *cmd)
{
    char canon[MC_API_CMD_CANON_CAP];
    cmd = mc_api_cmd_canon(cmd, canon, sizeof canon);
    if (!strcmp(cmd, "ascset"))
        return !mc_api_setter_reads(req);
    if (!strcmp(cmd, "time"))
        return mc_api_time_writes(req);
    return cmd_writes(cmd);
}

size_t mc_api_cmd_check(const char *req, const mc_api_stats *s, char *buf, size_t cap)
{
    char        pname[64];
    const char *p = check_param(req, pname, sizeof pname);
    if (!p)
        return json_status(buf, cap, s->when, 'E', 71, "Missing check cmd");
    int  exists = cmd_exists(p);
    int  access = exists && (mc_api_peer_trusted() || !cmd_writes(p));
    char status[256];
    snprintf(status, sizeof status,
            "[{\"STATUS\":\"S\",\"When\":%lu,\"Code\":72,\"Msg\":\"Check command\","
            "\"Description\":\"AvalonMiner %s\"}]",
            (unsigned long)s->when, AM_VER);
    char sec[64];
    snprintf(sec, sizeof sec, "[{\"Exists\":\"%s\",\"Access\":\"%s\"}]", exists ? "Y" : "N",
            access ? "Y" : "N");
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "CHECK", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

size_t mc_api_cmd_check_text(const char *req, const mc_api_stats *s, char *buf, size_t cap)
{
    char        pname[64];
    const char *p = check_param(req, pname, sizeof pname);
    if (!p)
        return mc_api_text_status(buf, cap, s->when, 'E', 71, "Missing check cmd");
    int    exists = cmd_exists(p);
    int    access = exists && (mc_api_peer_trusted() || !cmd_writes(p));
    size_t n      = mc_api_text_status(buf, cap, s->when, 'S', 72, "Check command");
    n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "CHECK,Exists=%s,Access=%s|",
            exists ? "Y" : "N", access ? "Y" : "N");
    return n;
}

size_t mc_api_dispatch_text(const char *req, const char *cmd, const mc_api_stats *s, char *buf,
        size_t cap)
{
    char canon[MC_API_CMD_CANON_CAP];
    cmd = mc_api_cmd_canon(cmd, canon, sizeof canon);
    if (!strcmp(cmd, "version"))
        return mc_api_version_text(s, buf, cap);
    if (!strcmp(cmd, "summary"))
        return mc_api_summary_text(s, buf, cap);
    if (!strcmp(cmd, "devs") || !strcmp(cmd, "edevs"))
        return mc_api_devs_text(s, buf, cap);
    if (!strcmp(cmd, "pools"))
        return mc_api_pools_text(s, buf, cap);
    if (!strcmp(cmd, "coin"))
        return mc_api_coin_text(s, buf, cap);
    if (!strcmp(cmd, "config"))
        return mc_api_config_text(s, buf, cap);
    if (!strcmp(cmd, "devdetails"))
        return mc_api_devdetails_text(s, buf, cap);
    if (!strcmp(cmd, "lcd"))
        return mc_api_lcd_text(s, buf, cap);
    if (!strcmp(cmd, "check"))
        return mc_api_cmd_check_text(req, s, buf, cap);
    if (!strcmp(cmd, "litestats"))
        return mc_api_litestats_text(s, buf, cap);
    if (!strcmp(cmd, "stats") || !strcmp(cmd, "estats"))
        return mc_api_stats_text(s, buf, cap, mc_api_estats_all(req), !strcmp(cmd, "stats"));
    if (!strcmp(cmd, "lockstats"))
        return mc_api_lockstats(0, s, buf, cap);
    if (!strcmp(cmd, "ascset")) {
        char          msg[MC_API_SET_MSG_CAP];
        mc_set_result r = mc_api_setter_run(req, s, msg, sizeof msg);
        return mc_api_text_status(buf, cap, s->when, mc_set_letter(r), mc_set_code(r), msg);
    }
    if (!strcmp(cmd, "time")) {
        char          msg[128];
        int           code;
        mc_set_result r = mc_api_time_run(req, msg, sizeof msg, &code);
        return mc_api_text_status(buf, cap, s->when, mc_set_letter(r), code, msg);
    }
    void         *cctx = NULL;
    mc_api_cmd_fn f    = mc_api_command_lookup(cmd, &cctx);
    if (f)
        return mc_api_command_run(f, cmd, req, s, 0, buf, cap, cctx);
    return (size_t)snprintf(buf, cap, "STATUS=E,When=%lu,Code=14,Msg=Invalid command|",
            (unsigned long)s->when);
}
