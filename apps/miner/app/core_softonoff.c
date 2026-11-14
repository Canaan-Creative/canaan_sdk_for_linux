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

#include "ctrl_cmds.h"
#include "core_softonoff.h"
#include "core_ascset_internal.h"
#include "core_ctrl.h"
#include "ctrl_args.h"
#include "mc_api_command.h"
#include "mc_api_cmds.h"
#include "miner_ipc.h"

#include <stdio.h>
#include <string.h>

#define X(e, n, p, c) { CTRL_OP_##e, n },

static void soft_payload(const char *req, char *out, size_t cap)
{
    out[0] = 0;
    if (!req || cap == 0)
        return;

    int         is_json = 0;
    const char *v       = NULL;
    const char *j       = strstr(req, "\"parameter\"");
    if (j) {
        const char *c = strchr(j, ':');
        if (!c)
            return;
        const char *q = strchr(c, '"');
        if (!q)
            return;
        is_json = 1;
        v       = q + 1;
    } else {
        const char *bar = strchr(req, '|');
        if (!bar)
            return;
        v = bar + 1;
    }
    size_t n = 0;
    while (*v && n + 1 < cap) {
        if (is_json && *v == '"')
            break;
        if (!is_json && *v == '|')
            break;
        if ((unsigned char)*v < 0x20 || (unsigned char)*v == 0x7f)
            break;
        out[n++] = *v++;
    }
    out[n] = 0;
}

static size_t soft_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    uint16_t    op  = (uint16_t)(uintptr_t)ctx;
    const char *cmd = ctrl_cmd_name(op);

    char pay[96];
    soft_payload(req, pay, sizeof pay);

    mc_ctrl_cmd out;
    if (ctrl_parse_cmd(op, pay, &out) != 0) {
        char shown[sizeof pay];
        osif_log(OSIF_LOG_WARN, "%s refused, raw payload \"%s\"", cmd,
                mc_api_args_redact(pay, shown, sizeof shown));
        char why[48];
        snprintf(why, sizeof why, "invalid %s setting", cmd);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 120, why);
    }

    mc_ctrl_ack ack;
    char        msg[96];
    switch (core_ctrl_send(&out, &ack)) {
    case 0:
        break;
    case -1:
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 120, "control channel down");
    default:
        snprintf(msg, sizeof msg, "%s not confirmed: no answer from mm_miner", cmd);
        osif_log(OSIF_LOG_WARN, "%s", msg);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 120, msg);
    }

    if (ack.result != MC_SET_OK) {
        switch (ack.why) {
        case MC_CTRL_WHY_PAST:
            snprintf(msg, sizeof msg, "%s refused: time %lld is past (board time %lld)", cmd,
                    (long long)out.v.soft.ts, (long long)ack.arg);
            break;
        case MC_CTRL_WHY_COOLDOWN:
            snprintf(msg, sizeof msg, "%s refused: over-temperature cool-down %lld s left", cmd,
                    (long long)ack.arg);
            break;
        case MC_CTRL_WHY_FAULT:
            snprintf(msg, sizeof msg,
                    "%s refused: fault stop SoftOFF[%lld] needs "
                    "softoff then softon",
                    cmd, (long long)ack.arg);
            break;
        default:
            snprintf(msg, sizeof msg, "%s refused by mm_miner (result %d)", cmd, (int)ack.result);
            break;
        }
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 120, msg);
    }

    if (out.v.soft.immediate)
        snprintf(msg, sizeof msg, "success %s now", cmd);
    else if (out.v.soft.valid)
        snprintf(msg, sizeof msg, "success %s at %lld", cmd, (long long)out.v.soft.ts);
    else
        snprintf(msg, sizeof msg, "success %s cancelled", cmd);
    osif_log(OSIF_LOG_DBG, "%s accepted, %s", cmd, msg);
    return mc_api_status_reply(is_json, buf, cap, s->when, 'S', 119, msg);
}

int core_soft_setter_register(void)
{
    static const struct {
        uint16_t    op; /* CTRL_OP_* code */
        const char *name;
    } cmds[] = { CTRL_CMD_COMMAND(X) };

    int n = 0;
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (!ctrl_cmd_available(cmds[i].op))
            continue;
        if (mc_api_command_register(cmds[i].name, soft_handler, (void *)(uintptr_t)cmds[i].op) == 0)
            n++;
        else
            osif_log(OSIF_LOG_ERR, "API command %s unavailable", cmds[i].name);
    }
    return n;
}
