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

#include "core_internal.h"
#include "cfgwr_client.h"
#include "mc_api_addpool.h"
#include "mc_api_cmds.h"
#include "mc_api_command.h"
#include "mc_api_poolid.h"
#include "mc_api_poollist.h"
#include "pool.h"
#include "pool_config.h"
#include "webauth.h"

#include <stdio.h>
#include <string.h>

/* url: pool URL; user: pool worker name; nth: running pools before it with the same
 * url and user, count; dups: running pools with its url and user, count; pass: pool
 * password; pnth: running pools before it with the same url, user and pass, count */
typedef struct {
    char url[POOL_URL_MAX], user[POOL_USER_MAX];
    int  nth, dups;
    char pass[POOL_PASS_MAX];
    int  pnth;
} pool_key;

typedef struct {
    const pool_key *key;
    int             enabled; /* new enabled state, 0 or 1 */
    int             gone;    /* 1 if the pool is not in the config */
    int             last;    /* 1 if the pool is the last enabled one in the config */
} pool_enable_apply;

enum { ADD_OK = 0, ADD_DUP, ADD_FULL }; /* added; already configured; no free slot */
typedef struct {
    const char *url, *user, *pass;
    int         out_index; /* config index of the added or existing pool */
    int         status;    /* ADD_* result */
} addpool_apply;

enum { RM_OK = 0, RM_GONE, RM_LAST }; /* removed; not in config; last enabled pool */
typedef struct {
    const pool_key *key;
    int             status;            /* RM_* result */
    char            url[POOL_URL_MAX]; /* URL of the matched pool */
} removepool_apply;

enum { RO_OK = 0, RO_GONE }; /* reordered; a requested pool not in config */
typedef struct {
    pool_key key[POOL_VALID_NUM]; /* requested pools in new order */
    int      n;
    int      order[POOL_VALID_NUM]; /* config index of each requested pool */
    int      enable;                /* also enable the new first pool */
    int      status;                /* RO_* result */
    int      bad;                   /* index in key[] of the pool not found */
    char     url[POOL_URL_MAX];     /* URL of the new first pool */
} reorder_apply;

static const char *g_enable_conf;
static unsigned    g_pool_revive; /* pools to mark alive at the next selection, bit per id */

void publish_pools(const pool_set *ps)
{
    pthread_mutex_lock(&g_pools_lock);
    g_pool_pub_n = ps->count > POOL_VALID_NUM ? POOL_VALID_NUM : ps->count;
    for (int i = 0; i < g_pool_pub_n; i++) {
        snprintf(g_pool_pub[i].url, sizeof g_pool_pub[i].url, "%s", ps->pools[i].url);
        snprintf(g_pool_pub[i].user, sizeof g_pool_pub[i].user, "%s", ps->pools[i].user);
        snprintf(g_pool_pub[i].pass, sizeof g_pool_pub[i].pass, "%s", ps->pools[i].pass);
        g_pool_enabled[i] = ps->pools[i].enabled;
        g_pool_prio[i]    = i;
    }
    pthread_mutex_unlock(&g_pools_lock);
}
void publish_active_pool(int idx)
{
    pthread_mutex_lock(&g_pools_lock);
    g_pool_active = idx;
    pthread_mutex_unlock(&g_pools_lock);
}

void sync_pool_policy(pool_set *ps, int *prio)
{
    pthread_mutex_lock(&g_pools_lock);
    for (int i = 0; i < ps->count && i < POOL_VALID_NUM; i++) {
        ps->pools[i].enabled = g_pool_enabled[i];
        prio[i]              = g_pool_prio[i];
    }
    unsigned revive = g_pool_revive;
    g_pool_revive   = 0;
    pthread_mutex_unlock(&g_pools_lock);

    if (!revive)
        return;
    pthread_mutex_lock(&g_stats_lock);
    for (int i = 0; i < ps->count && i < POOL_VALID_NUM; i++)
        if (revive & (1u << i)) {
            pool_mark_alive(ps, i);
            g_pool_st[i].dead = 0;
        }
    pthread_mutex_unlock(&g_stats_lock);
}

static void pool_revive(int idx)
{
    if (idx < 0 || idx >= POOL_VALID_NUM)
        return;
    pthread_mutex_lock(&g_pools_lock);
    g_pool_revive |= 1u << idx;
    pthread_mutex_unlock(&g_pools_lock);
}

int pool_outranked(int idx)
{
    int dead[POOL_VALID_NUM];

    if (idx < 0 || idx >= POOL_VALID_NUM)
        return 0;
    pthread_mutex_lock(&g_stats_lock);
    for (int i = 0; i < POOL_VALID_NUM; i++)
        dead[i] = g_pool_st[i].dead;
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_pools_lock);
    int out = !g_pool_enabled[idx];
    for (int i = 0; i < g_pool_pub_n && !out; i++)
        out = i != idx && g_pool_enabled[i] && g_pool_prio[i] < g_pool_prio[idx] &&
                (!dead[i] || (g_pool_revive & (1u << i)));
    pthread_mutex_unlock(&g_pools_lock);
    return out;
}

static void pool_kick_if_outranked(void)
{
    pthread_mutex_lock(&g_stratum.lock);
    if (g_stratum.t) {
        pthread_mutex_lock(&g_pools_lock);
        int active = g_pool_active;
        pthread_mutex_unlock(&g_pools_lock);
        if (pool_outranked(active)) {
            g_stratum.user_drop = 1;
            stratum_tcp_shutdown(g_stratum.t);
        }
    }
    pthread_mutex_unlock(&g_stratum.lock);
}

static int probe_event(void *ctx, sv1_client *c, const sv1_event *ev)
{
    (void)c;
    if (ev->type != SV1_EV_NOTIFY)
        return 0;
    *(int *)ctx = 1;
    return 1;
}

static int pool_probe(const struct core_pool_pub *p)
{
    char              url[POOL_URL_MAX], host[POOL_URL_MAX];
    int               port, up = 0;
    stratum_transport t;
    sv1_client        sv1;

    snprintf(url, sizeof url, "%s", p->url);
    url[strcspn(url, "#")] = '\0';
    if (stratum_parse_url(url, host, sizeof host, &port) != 0 ||
            stratum_tcp_connect(&t, host, port) != STRATUM_CONNECT_OK)
        return 0;
    sv1_init(&sv1);
    g_trace_quiet = 1;
    stratum_run(&sv1, &t, p->user, p->pass, POOL_USER_AGENT, probe_event, &up);
    g_trace_quiet = 0;
    sv1_free(&sv1);
    stratum_tcp_close(&t);
    return up;
}

void pool_probe_tick(uint64_t now)
{
    static uint64_t up_since[POOL_VALID_NUM]; /* first of the unbroken good probes, ms; 0 = none */
    struct core_pool_pub pub[POOL_VALID_NUM];
    int                  want[POOL_VALID_NUM] = { 0 };
    int                  dead[POOL_VALID_NUM];

    pthread_mutex_lock(&g_stats_lock);
    for (int i = 0; i < POOL_VALID_NUM; i++)
        dead[i] = g_pool_st[i].dead;
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_pools_lock);
    int active = g_pool_active;
    for (int i = 0; active >= 0 && i < g_pool_pub_n; i++) {
        want[i] = i != active && g_pool_enabled[i] && dead[i] &&
                g_pool_prio[i] < g_pool_prio[active] && !(g_pool_revive & (1u << i));
        if (want[i])
            pub[i] = g_pool_pub[i];
    }
    pthread_mutex_unlock(&g_pools_lock);

    for (int i = 0; i < POOL_VALID_NUM; i++) {
        if (!want[i] || !pool_probe(&pub[i])) {
            up_since[i] = 0;
            continue;
        }
        if (!up_since[i]) {
            up_since[i] = now;
            osif_log(OSIF_LOG_INFO,
                    "pool %d answers again; switching back "
                    "if it stays up for %d s",
                    i, POOL_FALLBACK_MS / 1000);
        }
        if (now - up_since[i] < POOL_FALLBACK_MS)
            continue;
        up_since[i] = 0;
        osif_log(OSIF_LOG_OP, "pool %d stable for %d s; switching back to it", i,
                POOL_FALLBACK_MS / 1000);
        pool_revive(i);
        pool_kick_if_outranked();
    }
}

void *pool_probe_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pool_probe_tick(now_ms());
        osif_sleep_ms(POOL_PROBE_MS);
    }
    return NULL;
}

static const char *pool_cmd_param(const char *req)
{
    const char *p = strchr(req, '|');
    if (p)
        return p + 1;
    const char *j = strstr(req, "\"parameter\"");
    const char *c = j ? strchr(j, ':') : NULL;
    const char *q = c ? strchr(c, '"') : NULL;
    return q ? q + 1 : NULL;
}

static int pool_have_any(void)
{
    pthread_mutex_lock(&g_pools_lock);
    int n = g_pool_pub_n;
    pthread_mutex_unlock(&g_pools_lock);
    return n;
}

static void pool_pub_key_locked(int id, pool_key *k)
{
    snprintf(k->url, sizeof k->url, "%s", g_pool_pub[id].url);
    snprintf(k->user, sizeof k->user, "%s", g_pool_pub[id].user);
    snprintf(k->pass, sizeof k->pass, "%s", g_pool_pub[id].pass);
    k->nth = k->dups = k->pnth = 0;
    for (int i = 0; i < g_pool_pub_n; i++)
        if (!strcmp(g_pool_pub[i].url, k->url) && !strcmp(g_pool_pub[i].user, k->user)) {
            k->dups++;
            k->nth += i < id;
            k->pnth += i < id && !strcmp(g_pool_pub[i].pass, k->pass);
        }
}

static int pool_pub_key(int id, pool_key *k)
{
    pthread_mutex_lock(&g_pools_lock);
    int n = g_pool_pub_n;
    if (id >= 0 && id < n)
        pool_pub_key_locked(id, k);
    pthread_mutex_unlock(&g_pools_lock);
    return n;
}

static int pool_same(const pool_t *a, const pool_t *b)
{
    return !strcmp(a->url, b->url) && !strcmp(a->user, b->user) && !strcmp(a->pass, b->pass);
}

static int pool_conf_find(const pool_set *ps, const pool_key *k)
{
    int hit = -1, n = 0, at = -1, same = 0, pat = -1, psame = 0;
    for (int i = 0; i < ps->count; i++) {
        if (strcmp(ps->pools[i].url, k->url))
            continue;
        if (!strcmp(ps->pools[i].user, k->user)) {
            if (same++ == k->nth)
                at = i;
            if (!strcmp(ps->pools[i].pass, k->pass) && psame++ == k->pnth)
                pat = i;
            continue;
        }
        hit = i;
        n++;
    }
    if (k->dups > 1)
        return same == k->dups ? pat : -1;
    if (same)
        return at;
    return n == 1 ? hit : -1;
}

static size_t reply_pool_gone(int is_json, char *buf, size_t cap, unsigned long when, int id,
        const char *url)
{
    char msg[POOL_URL_MAX + 64];
    snprintf(msg, sizeof msg, "Pool %d:'%s' is no longer in the pool config", id, url);
    return mc_api_status_reply(is_json, buf, cap, when, 'E', 26, msg);
}

static int apply_pool_enable(pool_set *ps, void *ctx)
{
    pool_enable_apply *a  = (pool_enable_apply *)ctx;
    int                at = pool_conf_find(ps, a->key);
    if (at < 0) {
        a->gone = 1;
        return -1;
    }
    if (!a->enabled) {
        int enabled = 0;
        for (int i = 0; i < ps->count; i++)
            enabled += i != at && ps->pools[i].enabled;
        if (!enabled) {
            a->last = 1;
            return -1;
        }
    }
    ps->pools[at].enabled = a->enabled;
    return 0;
}

static size_t reply_bad_pool_id(int is_json, char *buf, size_t cap, unsigned long when, int id,
        int count)
{
    char msg[64];
    if (count <= 0)
        snprintf(msg, sizeof msg, "Invalid pool id %d - no pools configured", id);
    else
        snprintf(msg, sizeof msg, "Invalid pool id %d - range is 0 - %d", id, count - 1);
    return mc_api_status_reply(is_json, buf, cap, when, 'E', 26, msg);
}

static size_t pool_enable_cmd(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, int want)
{
    if (pool_have_any() == 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 8, "No pools");

    int id = 0;
    if (mc_api_poolid_parse(req, is_json, &id) != MC_POOLID_OK)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 25,
                "Missing pool id parameter");

    pool_key key = { "", "", 0, 0, "", 0 };
    int      n, cur = 0, enabled = 0;
    pthread_mutex_lock(&g_pools_lock);
    n           = g_pool_pub_n;
    int inrange = (id >= 0 && id < n);
    if (inrange) {
        cur = g_pool_enabled[id];
        pool_pub_key_locked(id, &key);
    }
    for (int i = 0; i < n; i++)
        enabled += g_pool_enabled[i] != 0;
    pthread_mutex_unlock(&g_pools_lock);

    if (!inrange)
        return reply_bad_pool_id(is_json, buf, cap, s->when, id, n);

    if (cur == want) {
        char msg[POOL_URL_MAX + 64];
        snprintf(msg, sizeof msg, "Pool %d:'%s' already %s", id, key.url,
                want ? "enabled" : "disabled");
        return mc_api_status_reply(is_json, buf, cap, s->when, 'I', want ? 49 : 50, msg);
    }

    if (!want && enabled <= 1) {
        char msg[POOL_URL_MAX + 64];
        snprintf(msg, sizeof msg, "Cannot disable last active pool %d:'%s'", id, key.url);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 51, msg);
    }

    pool_enable_apply ap = { .key = &key, .enabled = want };
    int               wr = cfgwr_update_pools(g_enable_conf, apply_pool_enable, &ap);
    if (ap.last) {
        char msg[POOL_URL_MAX + 64];
        snprintf(msg, sizeof msg, "Cannot disable last active pool %d:'%s'", id, key.url);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 51, msg);
    }
    if (wr != 0) {
        if (ap.gone)
            return reply_pool_gone(is_json, buf, cap, s->when, id, key.url);
        osif_log(OSIF_LOG_WARN, "pool %d %s but not saved; it reverts after a restart", id,
                want ? "enabled" : "disabled");
    }

    pthread_mutex_lock(&g_pools_lock);
    g_pool_enabled[id] = want;
    int up = want && (g_pool_active < 0 || g_pool_prio[id] < g_pool_prio[g_pool_active]);
    pthread_mutex_unlock(&g_pools_lock);
    if (up)
        pool_revive(id);
    pool_kick_if_outranked();
    if (wr != 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, "config write error");

    osif_log(OSIF_LOG_DBG, "%s pool %d", want ? "enabling" : "disabling", id);
    char msg[POOL_URL_MAX + 64];
    snprintf(msg, sizeof msg, "%s pool %d:'%s'", want ? "Enabling" : "Disabling", id, key.url);
    return mc_api_status_reply(is_json, buf, cap, s->when, 'S', want ? 47 : 48, msg);
}

static size_t enablepool_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    return pool_enable_cmd(req, s, is_json, buf, cap, 1);
}

static size_t disablepool_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    return pool_enable_cmd(req, s, is_json, buf, cap, 0);
}

static int apply_addpool(pool_set *ps, void *ctx)
{
    addpool_apply *a = (addpool_apply *)ctx;
    for (int i = 0; i < ps->count; i++)
        if (!strcmp(ps->pools[i].url, a->url) && !strcmp(ps->pools[i].user, a->user)) {
            a->out_index = i;
            a->status    = ADD_DUP;
            return 0;
        }
    if (ps->count >= POOL_VALID_NUM) {
        a->status = ADD_FULL;
        return -1;
    }
    int idx = pool_add(ps, a->url, a->user, a->pass);
    if (idx < 0) {
        a->status = ADD_FULL;
        return -1;
    }
    a->out_index = idx;
    a->status    = ADD_OK;
    return 0;
}

static size_t addpool_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    mc_addpool_req pr;
    mc_addpool_rc  rc = mc_api_addpool_parse(req, &pr);
    if (rc == MC_ADDPOOL_MISSING)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 52, "Missing addpool details");
    if (rc != MC_ADDPOOL_OK)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, "Invalid addpool details");

    addpool_apply ap = { .url = pr.url, .user = pr.user, .pass = pr.pass };
    int           wr = cfgwr_update_pools(g_enable_conf, apply_addpool, &ap);
    if (ap.status == ADD_FULL)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, "Invalid addpool details");
    char msg[POOL_URL_MAX + 64];
    if (ap.status == ADD_DUP) {
        snprintf(msg, sizeof msg, "Pool already exists %d:'%s'", ap.out_index, pr.url);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, msg);
    }
    if (wr != 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, "config write error");

    osif_log(OSIF_LOG_DBG, "pool %d added (%s)", ap.out_index, pr.url);
    snprintf(msg, sizeof msg, "Added pool %d:'%s'", ap.out_index, pr.url);
    return mc_api_status_reply(is_json, buf, cap, s->when, 'S', 55, msg);
}

static int apply_removepool(pool_set *ps, void *ctx)
{
    removepool_apply *a  = (removepool_apply *)ctx;
    int               at = pool_conf_find(ps, a->key);
    if (at < 0) {
        a->status = RM_GONE;
        return -1;
    }
    snprintf(a->url, sizeof a->url, "%s", ps->pools[at].url);
    int enabled = 0;
    for (int i = 0; i < ps->count; i++)
        enabled += i != at && ps->pools[i].enabled;
    if (!enabled) {
        a->status = RM_LAST;
        return -1;
    }
    pool_config_apply_slot(ps, ps->pools[at].slot, "", "", "");
    a->status = RM_OK;
    return 0;
}

static size_t removepool_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    if (pool_have_any() == 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 8, "No pools");

    int id = 0;
    if (mc_api_poolid_parse(req, is_json, &id) != MC_POOLID_OK)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 25,
                "Missing pool id parameter for removepool");

    pool_key key = { "", "", 0, 0, "", 0 };
    int      n   = pool_pub_key(id, &key);
    if (id < 0 || id >= n)
        return reply_bad_pool_id(is_json, buf, cap, s->when, id, n);

    char msg[POOL_URL_MAX + 64];
    pthread_mutex_lock(&g_pools_lock);
    int active = g_pool_active == id;
    pthread_mutex_unlock(&g_pools_lock);
    if (active) {
        snprintf(msg, sizeof msg, "Cannot remove active pool %d:'%s'", id, key.url);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 67, msg);
    }

    removepool_apply ap = { .key = &key };
    int              wr = cfgwr_update_pools(g_enable_conf, apply_removepool, &ap);

    if (ap.status == RM_GONE)
        return reply_pool_gone(is_json, buf, cap, s->when, id, key.url);
    if (ap.status == RM_LAST) {
        snprintf(msg, sizeof msg, "Cannot remove last pool %d:'%s'", id, ap.url);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 66, msg);
    }
    if (wr != 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, "config write error");

    osif_log(OSIF_LOG_DBG, "pool %d removed (%s)", id, ap.url);
    snprintf(msg, sizeof msg, "Removed pool %d:'%s'", id, ap.url);
    return mc_api_status_reply(is_json, buf, cap, s->when, 'S', 68, msg);
}

static int reorder_keys(reorder_apply *a, const int *ids, int n, int *count)
{
    a->n = n;
    for (int i = 0; i < n; i++) {
        *count = pool_pub_key(ids[i], &a->key[i]);
        if (ids[i] < 0 || ids[i] >= *count)
            return i;
    }
    return -1;
}

static int apply_reorder(pool_set *ps, void *ctx)
{
    reorder_apply *a = (reorder_apply *)ctx;
    for (int i = 0; i < a->n; i++) {
        a->order[i] = pool_conf_find(ps, &a->key[i]);
        if (a->order[i] < 0) {
            a->status = RO_GONE;
            a->bad    = i;
            return -1;
        }
    }
    if (a->n > 0)
        snprintf(a->url, sizeof a->url, "%s", ps->pools[a->order[0]].url);

    if (a->n > 0 && a->enable)
        ps->pools[a->order[0]].enabled = 1;

    pool_t np[POOL_VALID_NUM];
    int    used[POOL_VALID_NUM] = { 0 }, c = 0;
    for (int i = 0; i < a->n; i++) {
        int id = a->order[i];
        for (int j = 0; j < ps->count; j++)
            if (!used[j] && pool_same(&ps->pools[j], &ps->pools[id])) {
                id = j;
                break;
            }
        if (used[id])
            continue;
        np[c++]  = ps->pools[id];
        used[id] = 1;
    }
    for (int i = 0; i < ps->count; i++)
        if (!used[i])
            np[c++] = ps->pools[i];
    for (int i = 0; i < c; i++) {
        np[i].slot   = ps->pools[i].slot;
        ps->pools[i] = np[i];
    }
    a->status = RO_OK;
    return 0;
}

static void pool_prio_apply(const int *ids, int n, int enable)
{
    int prio[POOL_VALID_NUM], used[POOL_VALID_NUM] = { 0 }, next = 0;

    pthread_mutex_lock(&g_pools_lock);
    for (int i = 0; i < n; i++) {
        prio[ids[i]] = next++;
        used[ids[i]] = 1;
    }
    for (int pr = 0; pr < g_pool_pub_n; pr++)
        for (int i = 0; i < g_pool_pub_n; i++)
            if (!used[i] && g_pool_prio[i] == pr) {
                prio[i] = next++;
                used[i] = 1;
            }
    memcpy(g_pool_prio, prio, sizeof prio[0] * (size_t)g_pool_pub_n);
    if (enable)
        g_pool_enabled[ids[0]] = 1;
    pthread_mutex_unlock(&g_pools_lock);
    if (enable)
        pool_revive(ids[0]);
    pool_kick_if_outranked();
}

static size_t switchpool_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    if (pool_have_any() == 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 8, "No pools");

    int id = 0;
    if (mc_api_poolid_parse(req, is_json, &id) != MC_POOLID_OK)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 25,
                "Missing pool id parameter");

    reorder_apply ap = { .enable = 1, .status = RO_OK };
    int           n  = 0;
    if (reorder_keys(&ap, &id, 1, &n) >= 0)
        return reply_bad_pool_id(is_json, buf, cap, s->when, id, n);
    int wr = cfgwr_update_pools(g_enable_conf, apply_reorder, &ap);

    if (ap.status == RO_GONE)
        return reply_pool_gone(is_json, buf, cap, s->when, id, ap.key[0].url);
    pool_prio_apply(&id, 1, 1);
    if (wr != 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, "config write error");

    osif_log(OSIF_LOG_DBG, "switching to pool %d (%s)", id, ap.url);
    char msg[POOL_URL_MAX + 64];
    snprintf(msg, sizeof msg, "Switching to pool %d:'%s'", id, ap.url);
    return mc_api_status_reply(is_json, buf, cap, s->when, 'S', 27, msg);
}

static size_t poolpriority_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    if (pool_have_any() == 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 8, "No pools");

    mc_poollist_req pl;
    if (mc_api_poollist_parse(req, is_json, &pl) != MC_POOLLIST_OK)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 25,
                "Missing pool id parameter");

    int n = pool_have_any(), seen[POOL_VALID_NUM] = { 0 };
    for (int i = 0; i < pl.n; i++) {
        int id = pl.ids[i];
        if (id < 0 || id >= n)
            return reply_bad_pool_id(is_json, buf, cap, s->when, id, n);
        if (seen[id]) {
            char msg[64];
            snprintf(msg, sizeof msg, "Duplicate pool specified %d", id);
            return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 74, msg);
        }
        seen[id] = 1;
    }

    reorder_apply ap  = { .status = RO_OK };
    int           bad = reorder_keys(&ap, pl.ids, pl.n, &n);
    if (bad >= 0)
        return reply_bad_pool_id(is_json, buf, cap, s->when, pl.ids[bad], n);
    int wr = cfgwr_update_pools(g_enable_conf, apply_reorder, &ap);

    if (ap.status == RO_GONE)
        return reply_pool_gone(is_json, buf, cap, s->when, pl.ids[ap.bad], ap.key[ap.bad].url);
    pool_prio_apply(pl.ids, pl.n, 0);
    if (wr != 0)
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 53, "config write error");

    osif_log(OSIF_LOG_DBG, "pool priority changed (%d pools)", pl.n);
    return mc_api_status_reply(is_json, buf, cap, s->when, 'S', 73, "Changed pool priorities");
}

static void zero_all_stats(void)
{
    pthread_mutex_lock(&g_stratum.lock);
    if (g_stratum.pipe)
        mc_pipeline_zero_stats(g_stratum.pipe);
    pthread_mutex_lock(&g_stats_lock);
    g_diff_acc = g_diff_rej = g_diff1 = 0;
    memset(&g_nst, 0, sizeof g_nst);
    g_nst.disc_base = g_chip.nonce_discarded;
    memset(g_mw, 0, sizeof g_mw);
    g_local_work = 0;
    memset(g_hashwin.slot, 0, sizeof g_hashwin.slot);
    g_hashwin.live = 0;
    for (int i = 0; i < POOL_VALID_NUM; i++) {
        g_pool_st[i].acc = g_pool_st[i].rej = 0;
        g_pool_st[i].dacc = g_pool_st[i].drej = 0;
        g_pool_st[i].diff1 = g_pool_st[i].best = 0;
        g_pool_st[i].bad = g_pool_st[i].getworks = 0;
        g_pool_st[i].getfail = g_pool_st[i].rfail = 0;
        g_pool_st[i].stale                        = 0;
        g_pool_st[i].dstale                       = 0;
        g_pool_st[i].ltime                        = 0;
        g_pool_st[i].lsdiff                       = 0;
    }
    memset(g_board_diff1, 0, sizeof g_board_diff1);
    g_dev_start       = 0;
    g_last_valid_work = 0;
    g_start           = now_s();
    pthread_mutex_unlock(&g_stats_lock);
    pthread_mutex_unlock(&g_stratum.lock);
}

static size_t zero_handler(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx)
{
    (void)ctx;
    const char *p = pool_cmd_param(req);
    if (!p || !*p || *p == '"')
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 94, "Missing zero parameters");

    char which[16] = "";
    int  wi        = 0;
    while (*p && *p != ',' && *p != '"' && (unsigned char)*p >= 0x20 && (unsigned char)*p != 0x7f &&
            wi < (int)sizeof which - 1)
        which[wi++] = *p++;
    which[wi] = '\0';

    int all;
    if (!strcmp(which, "all")) {
        all = 1;
    } else if (!strcmp(which, "bestshare")) {
        all = 0;
    } else {
        char msg[48];
        snprintf(msg, sizeof msg, "Invalid zero parameter '%s'", which);
        return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 95, msg);
    }

    int summary = 0;
    if (*p == ',') {
        p++;
        summary = (*p == 't' || *p == 'T' || *p == '1');
    }

    if (all) {
        zero_all_stats();
    } else {
        pthread_mutex_lock(&g_stats_lock);
        for (int i = 0; i < POOL_VALID_NUM; i++)
            g_pool_st[i].best = 0;
        pthread_mutex_unlock(&g_stats_lock);
    }

    osif_log(OSIF_LOG_DBG, "statistics reset (%s)", which);
    char msg[64];
    snprintf(msg, sizeof msg, "Zeroed %s stats%s", which, summary ? " with summary" : "");
    return mc_api_status_reply(is_json, buf, cap, s->when, 'S', summary ? 96 : 97, msg);
}

int core_pool_cmds_register(const char *conf)
{
    g_enable_conf = conf && *conf ? conf : POOL_CONFIG_DEFAULT;
    int rc        = 0;
    rc |= mc_api_command_register("enablepool", enablepool_handler, NULL);
    rc |= mc_api_command_register("disablepool", disablepool_handler, NULL);
    rc |= mc_api_command_register("addpool", addpool_handler, NULL);
    rc |= mc_api_command_register("removepool", removepool_handler, NULL);
    rc |= mc_api_command_register("switchpool", switchpool_handler, NULL);
    rc |= mc_api_command_register("poolpriority", poolpriority_handler, NULL);
    rc |= mc_api_command_register("zero", zero_handler, NULL);
    return rc;
}
