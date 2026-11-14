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

#include "core_pool.h"
#include "mc_api_command.h"
#include "mc_api_cmds.h"
#include "mc_api_setpool.h"
#include "pool_config.h"
#include "cfgwr_client.h"
#include "webauth.h"
#include "osif.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *conf_path;
} pool_setter_ctx;

static pool_setter_ctx g_ctx;

static int apply_slot(pool_set *ps, void *ctx)
{
    const mc_setpool_req *r                     = (const mc_setpool_req *)ctx;
    char                  old_url[POOL_URL_MAX] = "";
    for (int i = 0; i < ps->count; i++)
        if (ps->pools[i].slot == r->index)
            snprintf(old_url, sizeof old_url, "%s", ps->pools[i].url);
    if (pool_config_apply_slot(ps, r->index, r->url, r->user, r->pass) != 0)
        return -1;
    for (int i = 0; i < ps->count; i++)
        if (ps->pools[i].slot == r->index && strcmp(ps->pools[i].url, old_url) != 0)
            ps->pools[i].enabled = 1;
    return 0;
}

static size_t setpool_reply(mc_setpool_rc rc, const mc_setpool_req *r, const mc_api_stats *s,
        int is_json, char *buf, size_t cap)
{
    char msg[MC_API_LEGACY_MSG_CAP];
    mc_api_setpool_msg(rc, r, msg, sizeof msg);
    return mc_api_status_reply_legacy(is_json, buf, cap, s->when, rc == MC_SETPOOL_OK ? 'S' : 'E',
            rc == MC_SETPOOL_OK ? MC_SETPOOL_CODE_OK : MC_SETPOOL_CODE_ERR, msg);
}

static size_t setpool_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    mc_setpool_req pr;
    mc_setpool_rc  rc = mc_api_setpool_parse(req, &pr);
    if (rc == MC_SETPOOL_OK)
        rc = mc_api_setpool_check(&pr);
    if (rc == MC_SETPOOL_OK && cfgwr_update_pools(g_ctx.conf_path, apply_slot, &pr) != 0)
        rc = MC_SETPOOL_WRITE;
    if (rc == MC_SETPOOL_OK)
        osif_log(OSIF_LOG_DBG, "pool %d set to %s", pr.index, pr.clear ? "<cleared>" : pr.url);
    return setpool_reply(rc, &pr, s, is_json, buf, cap);
}

int core_pool_setter_register(const char *conf_path, const char *systemcfg_ini)
{
    (void)systemcfg_ini;
    g_ctx.conf_path = conf_path && *conf_path ? conf_path : POOL_CONFIG_DEFAULT;
    return mc_api_command_register("setpool", setpool_handler, &g_ctx);
}
