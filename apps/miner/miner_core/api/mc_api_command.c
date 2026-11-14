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

#include "mc_api_command.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "osif.h"

#define MC_CMD_MAX         20                   /* registered commands, max count */
#define MC_CMD_NAME        24                   /* command name buffer, bytes */
#define MC_CMD_LOG_MSG_CAP 160                  /* logged Msg buffer, bytes */
#define MC_CMD_LOG_SECRET  "workerpassword is " /* setpool Msg text before the pool password */

static struct {
    char          name[MC_CMD_NAME];
    mc_api_cmd_fn fn;
    void         *ctx;
} g_reg[MC_CMD_MAX];
static int g_n; /* registered commands, count */

const char *mc_api_cmd_canon(const char *cmd, char *out, size_t cap)
{
    if (!cmd || !out || cap == 0)
        return cmd;
    size_t n = 0;
    while (cmd[n] && n + 1 < cap) {
        out[n] = (char)tolower((unsigned char)cmd[n]);
        n++;
    }
    out[n] = 0;
    return out;
}

int mc_api_command_register(const char *cmd, mc_api_cmd_fn fn, void *ctx)
{
    if (!cmd || !*cmd || !fn)
        return -1;
    if (strlen(cmd) >= MC_CMD_NAME)
        return -1;
    if (g_n >= MC_CMD_MAX)
        return -1;
    for (int i = 0; i < g_n; i++)
        if (!strcasecmp(g_reg[i].name, cmd))
            return -1;
    strncpy(g_reg[g_n].name, cmd, MC_CMD_NAME - 1);
    g_reg[g_n].fn  = fn;
    g_reg[g_n].ctx = ctx;
    g_n++;
    return 0;
}

mc_api_cmd_fn mc_api_command_lookup(const char *cmd, void **ctx_out)
{
    if (!cmd)
        return NULL;
    for (int i = 0; i < g_n; i++)
        if (!strcasecmp(g_reg[i].name, cmd)) {
            if (ctx_out)
                *ctx_out = g_reg[i].ctx;
            return g_reg[i].fn;
        }
    return NULL;
}

void mc_api_command_reset(void)
{
    g_n = 0;
    memset(g_reg, 0, sizeof g_reg);
}

static void reply_status(const char *buf, char *st, int *code, char *msg, size_t cap)
{
    const char *p, *end;
    int         json = strstr(buf, "\"STATUS\":\"") != NULL;

    *st    = '?';
    *code  = 0;
    msg[0] = '\0';
    if ((p = strstr(buf, json ? "\"STATUS\":\"" : "STATUS=")) != NULL)
        *st = p[json ? 10 : 7];
    if ((p = strstr(buf, json ? "\"Code\":" : "Code=")) != NULL)
        *code = atoi(p + (json ? 7 : 5));
    if ((p = strstr(buf, json ? "\"Msg\":\"" : "Msg=")) == NULL)
        return;
    p += json ? 7 : 4;
    size_t n = 0;
    for (end = p; *end && n + 1 < cap; end++) {
        if (*end == '\\' && end[1]) {
            msg[n++] = *++end;
            continue;
        }
        if (json ? *end == '"' : (*end == ',' || *end == '|'))
            break;
        msg[n++] = *end;
    }
    msg[n] = '\0';
}

static void mask_secret(char *msg, size_t cap)
{
    char *p = strstr(msg, MC_CMD_LOG_SECRET);

    if (p) {
        size_t off = (size_t)(p - msg) + strlen(MC_CMD_LOG_SECRET);
        snprintf(msg + off, cap - off, "***");
    }
}

size_t mc_api_command_run(mc_api_cmd_fn fn, const char *cmd, const char *req, const mc_api_stats *s,
        int is_json, char *buf, size_t cap, void *ctx)
{
    size_t n = fn(req, s, is_json, buf, cap, ctx);
    char   st, msg[MC_CMD_LOG_MSG_CAP];
    int    code;

    if (cap == 0)
        return n;
    buf[n < cap ? n : cap - 1] = '\0';
    reply_status(buf, &st, &code, msg, sizeof msg);
    mask_secret(msg, sizeof msg);
    osif_log(OSIF_LOG_OP, "API command %s %s (code %d: %s)", cmd,
            st == 'E' ? "refused" : "accepted", code, msg);
    return n;
}
