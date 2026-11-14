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

#include "sv1.h"
#include "hex.h"
#include "jread.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SV1_INFLIGHT_MAX 64   /* tracked unanswered submits, count */
#define SV1_INFLIGHT_TTL 120  /* unanswered submit lifetime, s */
#define SV1_ESC_CAP      772  /* escaped JSON string buffer, bytes: 128 chars at 6 each */
#define SV1_LINE_CAP     2048 /* authorize or submit line buffer, bytes */

static pthread_mutex_t g_inflight_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_inflight_id   = 1; /* next JSON-RPC id */
/* unanswered submits: id (0 = free), send time (monotonic ms), share difficulty */
static struct {
    int      id;
    uint64_t at;
    double   diff;
} g_inflight[SV1_INFLIGHT_MAX];
static uint32_t g_inflight_n;                 /* used g_inflight slots, count */
static uint32_t g_inflight_stale;             /* submits expired or evicted unanswered, count */
static int      g_answered[SV1_INFLIGHT_MAX]; /* recently answered submit ids */
static uint32_t g_answered_w;                 /* g_answered writes, count */

static int alloc_id(sv1_client *c)
{
    pthread_mutex_lock(&g_inflight_lock);
    int id = g_inflight_id++;
    pthread_mutex_unlock(&g_inflight_lock);
    c->next_id = id + 1;
    return id;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void prune_locked(uint64_t now)
{
    for (int i = 0; i < SV1_INFLIGHT_MAX; i++)
        if (g_inflight[i].id && now > g_inflight[i].at + SV1_INFLIGHT_TTL * 1000u) {
            g_inflight[i].id = 0;
            g_inflight_n--;
            g_inflight_stale++;
        }
}

static void inflight_add(int id)
{
    uint64_t now  = mono_ms();
    int      slot = -1, oldest = 0;
    pthread_mutex_lock(&g_inflight_lock);
    prune_locked(now);
    for (int i = 0; i < SV1_INFLIGHT_MAX; i++) {
        if (!g_inflight[i].id) {
            slot = i;
            break;
        }
        if (g_inflight[i].at < g_inflight[oldest].at)
            oldest = i;
    }
    if (slot < 0) {
        slot = oldest;
        g_inflight_stale++;
        g_inflight_n--;
    }
    g_inflight[slot].id   = id;
    g_inflight[slot].at   = now;
    g_inflight[slot].diff = 0;
    g_inflight_n++;
    pthread_mutex_unlock(&g_inflight_lock);
}

static int answered_seen(int id)
{
    int seen = 0;
    pthread_mutex_lock(&g_inflight_lock);
    for (int i = 0; id && i < SV1_INFLIGHT_MAX; i++)
        if (g_answered[i] == id) {
            seen = 1;
            break;
        }
    pthread_mutex_unlock(&g_inflight_lock);
    return seen;
}

static int id_issued(int id)
{
    pthread_mutex_lock(&g_inflight_lock);
    int hi = g_inflight_id - 1;
    pthread_mutex_unlock(&g_inflight_lock);
    return id > 0 && id <= hi;
}

static int inflight_claim(int id)
{
    int found = 0;
    pthread_mutex_lock(&g_inflight_lock);
    prune_locked(mono_ms());
    for (int i = 0; id && i < SV1_INFLIGHT_MAX; i++)
        if (g_inflight[i].id == id) {
            g_inflight[i].id = 0;
            g_inflight_n--;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&g_inflight_lock);
    return found;
}

static void answered_mark(int id)
{
    if (!id)
        return;
    pthread_mutex_lock(&g_inflight_lock);
    g_answered[g_answered_w++ % SV1_INFLIGHT_MAX] = id;
    pthread_mutex_unlock(&g_inflight_lock);
}

uint32_t sv1_clear_inflight(sv1_client *c, double *diff)
{
    uint32_t cleared = 0;
    double   sum     = 0;
    pthread_mutex_lock(&g_inflight_lock);
    for (int i = 0; i < SV1_INFLIGHT_MAX; i++)
        if (g_inflight[i].id) {
            g_inflight[i].id = 0;
            g_inflight_n--;
            cleared++;
            sum += g_inflight[i].diff;
        }
    pthread_mutex_unlock(&g_inflight_lock);
    if (c)
        c->stale += cleared;
    if (diff)
        *diff = sum;
    return cleared;
}

void sv1_inflight_set_diff(int id, double diff)
{
    pthread_mutex_lock(&g_inflight_lock);
    for (int i = 0; id && i < SV1_INFLIGHT_MAX; i++)
        if (g_inflight[i].id == id) {
            g_inflight[i].diff = diff;
            break;
        }
    pthread_mutex_unlock(&g_inflight_lock);
}

uint32_t sv1_inflight_count(void)
{
    pthread_mutex_lock(&g_inflight_lock);
    prune_locked(mono_ms());
    uint32_t n = g_inflight_n;
    pthread_mutex_unlock(&g_inflight_lock);
    return n;
}

uint32_t sv1_stale_shares(void)
{
    pthread_mutex_lock(&g_inflight_lock);
    prune_locked(mono_ms());
    uint32_t n = g_inflight_stale;
    pthread_mutex_unlock(&g_inflight_lock);
    return n;
}

void sv1_init(sv1_client *c)
{
    memset(c, 0, sizeof *c);
    c->next_id = 1;
}

static void free_job(sv1_job *j)
{
    free(j->prevhash);
    free(j->coinb1);
    free(j->coinb2);
    for (size_t i = 0; i < j->n_merkle; i++)
        free(j->merkle[i]);
    memset(j, 0, sizeof *j);
}

void sv1_free(sv1_client *c)
{
    free_job(&c->job);
    c->has_job = 0;
}

static const char *json_esc(char *dst, size_t cap, const char *s)
{
    size_t n = 0;

    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        char          e[7];
        size_t        el;

        if (ch == '"' || ch == '\\') {
            e[0] = '\\';
            e[1] = (char)ch;
            el   = 2;
        } else if (ch < 0x20) {
            el = (size_t)snprintf(e, sizeof e, "\\u%04x", ch);
        } else {
            e[0] = (char)ch;
            el   = 1;
        }
        if (n + el >= cap)
            break;
        memcpy(dst + n, e, el);
        n += el;
    }
    dst[n] = '\0';
    return dst;
}

static int json_i32(const json_value *v)
{
    long long n = json_int(v);
    return n >= INT_MIN && n <= INT_MAX ? (int)n : 0;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char  *o = (char *)malloc(n + 1);
    if (o)
        memcpy(o, s, n + 1);
    return o;
}

char *sv1_build_configure(sv1_client *c)
{
    char buf[256];
    c->id_configure = alloc_id(c);
    snprintf(buf, sizeof buf,
            "{\"id\":%d,\"method\":\"mining.configure\","
            "\"params\":[[\"version-rolling\"],{\"version-rolling.mask\":\"%08x\"}]}",
            c->id_configure, SV1_VERSION_MASK_MAX);
    return dup_str(buf);
}

char *sv1_build_subscribe(sv1_client *c, const char *user_agent)
{
    char buf[256];
    c->id_subscribe = alloc_id(c);
    snprintf(buf, sizeof buf, "{\"id\":%d,\"method\":\"mining.subscribe\",\"params\":[\"%s\"]}",
            c->id_subscribe, user_agent ? user_agent : "");
    return dup_str(buf);
}

char *sv1_build_authorize(sv1_client *c, const char *user, const char *pass)
{
    char buf[SV1_LINE_CAP], eu[SV1_ESC_CAP], ep[SV1_ESC_CAP];
    c->id_authorize = alloc_id(c);
    snprintf(buf, sizeof buf,
            "{\"id\":%d,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"%s\"]}",
            c->id_authorize, json_esc(eu, sizeof eu, user), json_esc(ep, sizeof ep, pass));
    return dup_str(buf);
}

char *sv1_build_extranonce_subscribe(sv1_client *c)
{
    char buf[96];
    c->id_xnsub = alloc_id(c);
    snprintf(buf, sizeof buf,
            "{\"id\":%d,\"method\":\"mining.extranonce.subscribe\",\"params\":[]}", c->id_xnsub);
    return dup_str(buf);
}

char *sv1_build_pong(int req_id)
{
    char buf[64];
    snprintf(buf, sizeof buf, "{\"id\": %d, \"result\": \"pong\", \"error\": null}", req_id);
    return dup_str(buf);
}

char *sv1_build_version(int req_id, const char *user_agent)
{
    char buf[128];
    snprintf(buf, sizeof buf, "{\"id\": %d, \"result\": \"%s\", \"error\": null}", req_id,
            user_agent ? user_agent : "canaan-miner/1.0");
    return dup_str(buf);
}

char *sv1_build_submit(sv1_client *c, const char *user, const char *job_id, const char *xnonce2,
        const char *ntime, const char *nonce, const char *version_bits)
{
    char buf[SV1_LINE_CAP], eu[SV1_ESC_CAP], ej[SV1_ESC_CAP];
    int  id = alloc_id(c);
    json_esc(eu, sizeof eu, user);
    json_esc(ej, sizeof ej, job_id);
    if (version_bits && *version_bits)
        snprintf(buf, sizeof buf,
                "{\"id\":%d,\"method\":\"mining.submit\","
                "\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}",
                id, eu, ej, xnonce2, ntime, nonce, version_bits);
    else
        snprintf(buf, sizeof buf,
                "{\"id\":%d,\"method\":\"mining.submit\","
                "\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}",
                id, eu, ej, xnonce2, ntime, nonce);
    inflight_add(id);
    return dup_str(buf);
}

static void copy_fixed(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int is_hex(const char *s)
{
    return s && *s && !(strlen(s) % 2) && hex_valid(s, strlen(s));
}

static int extranonce_ok(const char *n1, int n2)
{
    return is_hex(n1) && strlen(n1) < SV1_XNONCE1_MAX && n2 >= 2 && n2 <= 16;
}

static int job_id_ok(const json_value *v)
{
    const char *s = json_string(v);
    return json_is(v, JSON_STRING) && *s && strlen(s) < SV1_ID_MAX;
}

static int parse_notify(sv1_client *c, const json_value *params)
{
    if (json_array_size(params) < 9)
        return -1;

    const json_value *v_id   = json_array_get(params, 0);
    const char       *v_prev = json_string(json_array_get(params, 1));
    const char       *v_cb1  = json_string(json_array_get(params, 2));
    const char       *v_cb2  = json_string(json_array_get(params, 3));
    const char       *v_ver  = json_string(json_array_get(params, 5));
    const char       *v_nbit = json_string(json_array_get(params, 6));
    const char       *v_time = json_string(json_array_get(params, 7));
    if (!job_id_ok(v_id) || !is_hex(v_prev) || !is_hex(v_cb1) || !is_hex(v_cb2) || !is_hex(v_ver) ||
            !is_hex(v_nbit) || !is_hex(v_time))
        return -1;
    if (strlen(v_prev) >= sizeof c->prev_hash || strlen(v_ver) >= sizeof c->job.version ||
            strlen(v_nbit) >= sizeof c->job.nbits || strlen(v_time) >= sizeof c->job.ntime)
        return -1;
    const json_value *mk = json_array_get(params, 4);
    if (!json_is(mk, JSON_ARRAY) || json_array_size(mk) > SV1_MAX_MERKLE)
        return -1;
    for (size_t i = 0; i < json_array_size(mk); i++)
        if (!is_hex(json_string(json_array_get(mk, i))))
            return -1;

    sv1_job nj;
    memset(&nj, 0, sizeof nj);
    copy_fixed(nj.job_id, sizeof nj.job_id, json_string(v_id));
    nj.prevhash = dup_str(v_prev);
    nj.coinb1   = dup_str(v_cb1);
    nj.coinb2   = dup_str(v_cb2);

    size_t m    = json_array_size(mk);
    nj.n_merkle = m;
    for (size_t i = 0; i < m; i++)
        nj.merkle[i] = dup_str(json_string(json_array_get(mk, i)));

    copy_fixed(nj.version, sizeof nj.version, v_ver);
    copy_fixed(nj.nbits, sizeof nj.nbits, v_nbit);
    copy_fixed(nj.ntime, sizeof nj.ntime, v_time);
    nj.clean_jobs = json_bool(json_array_get(params, 8));

    if (!nj.prevhash || !nj.coinb1 || !nj.coinb2) {
        free_job(&nj);
        return -1;
    }
    for (size_t i = 0; i < m; i++)
        if (!nj.merkle[i]) {
            free_job(&nj);
            return -1;
        }

    if (c->prev_hash[0] && strcmp(c->prev_hash, nj.prevhash) != 0)
        nj.clean_jobs = 1;
    copy_fixed(c->prev_hash, sizeof c->prev_hash, nj.prevhash);

    free_job(&c->job);
    c->job = nj;

    if (c->next_diff > 0) {
        c->difficulty = c->next_diff;
        c->next_diff  = c->diff_after;
        c->diff_after = 0;
    }
    c->has_job = 1;
    if (c->state >= SV1_AUTHORIZED)
        c->state = SV1_MINING;
    return 0;
}

static void capture_error(sv1_client *c, sv1_event *ev, const json_value *error)
{
    c->err_msg[0] = '\0';
    if (json_is(error, JSON_ARRAY)) {
        ev->err_code = json_i32(json_array_get(error, 0));
        copy_fixed(c->err_msg, sizeof c->err_msg, json_string(json_array_get(error, 1)));
    } else if (json_is(error, JSON_STRING)) {
        copy_fixed(c->err_msg, sizeof c->err_msg, json_string(error));
    }
    ev->err_msg = c->err_msg;
}

int sv1_feed_line(sv1_client *c, const char *line, sv1_event *ev)
{
    ev->type            = SV1_EV_NONE;
    ev->submit_accepted = 0;
    ev->submit_lost     = 0;
    ev->submit_id       = 0;
    ev->err_code        = 0;
    ev->err_msg         = "";

    json_value *v = json_parse(line);
    if (!v) {
        ev->type = SV1_EV_ERROR;
        return -1;
    }

    const json_value *method = json_object_get(v, "method");
    if (json_is(method, JSON_STRING)) {
        const char       *m      = json_string(method);
        const json_value *params = json_object_get(v, "params");
        if (!strcmp(m, "mining.notify")) {
            if (parse_notify(c, params) == 0)
                ev->type = SV1_EV_NOTIFY;
            else
                ev->type = SV1_EV_ERROR;
        } else if (!strcmp(m, "mining.set_difficulty")) {
            double d = json_number(json_array_get(params, 0));
            if (d > 0) {
                if (c->next_diff > 0)
                    c->diff_after = d;
                else
                    c->next_diff = d;
                ev->type = SV1_EV_SET_DIFFICULTY;
            } else {
                ev->type = SV1_EV_ERROR;
            }
        } else if (!strcmp(m, "mining.set_version_mask")) {
            const json_value *mp = json_is(params, JSON_ARRAY) ? json_array_get(params, 0) : params;
            const char       *ms = json_string(mp);
            if (is_hex(ms)) {
                uint32_t nm        = (uint32_t)strtoul(ms, NULL, 16) & SV1_VERSION_MASK_MAX;
                c->version_mask    = nm;
                c->version_rolling = nm != 0;
                ev->type           = SV1_EV_SET_VERSION_MASK;
            } else {
                ev->type = SV1_EV_ERROR;
            }
        } else if (!strcmp(m, "mining.ping")) {
            const json_value *id = json_object_get(v, "id");
            ev->req_id           = json_is(id, JSON_NUMBER) ? json_i32(id) : 0;
            ev->type             = SV1_EV_PING;
        } else if (!strcmp(m, "client.get_version")) {
            const json_value *id = json_object_get(v, "id");
            ev->req_id           = json_is(id, JSON_NUMBER) ? json_i32(id) : 0;
            ev->type             = SV1_EV_GET_VERSION;
        } else if (!strcmp(m, "client.show_message")) {
            copy_fixed(c->err_msg, sizeof c->err_msg, json_string(json_array_get(params, 0)));
            ev->err_msg = c->err_msg;
            ev->type    = SV1_EV_SHOW_MESSAGE;
        } else if (!strcmp(m, "client.reconnect")) {
            ev->type = SV1_EV_RECONNECT;
        } else if (!strcmp(m, "mining.set_extranonce")) {
            const char *n1 = json_string(json_array_get(params, 0));
            int         n2 = json_i32(json_array_get(params, 1));
            if (extranonce_ok(n1, n2)) {
                copy_fixed(c->xnonce1, sizeof c->xnonce1, n1);
                c->xnonce2_size = n2;
                ev->type        = SV1_EV_SET_EXTRANONCE;
            } else {
                ev->type = SV1_EV_ERROR;
            }
        }
        json_free(v);
        return 0;
    }

    const json_value *result    = json_object_get(v, "result");
    const json_value *error     = json_object_get(v, "error");
    const json_value *id        = json_object_get(v, "id");
    int               has_error = error && !json_is(error, JSON_NULL);
    int               reply_id  = json_is(id, JSON_NUMBER) ? json_i32(id) : 0;

    if (json_is(result, JSON_OBJECT) && json_object_get(result, "version-rolling")) {
        c->version_rolling     = json_bool(json_object_get(result, "version-rolling"));
        const json_value *mask = json_object_get(result, "version-rolling.mask");
        if (json_is(mask, JSON_STRING))
            c->version_mask = (uint32_t)strtoul(json_string(mask), NULL, 16);
        c->version_mask &= SV1_VERSION_MASK_MAX;
        if (!c->version_mask)
            c->version_rolling = 0;
        if (c->state < SV1_CONFIGURED)
            c->state = SV1_CONFIGURED;
        ev->type = SV1_EV_CONFIGURED;
    } else if (json_is(result, JSON_ARRAY) && json_is(json_array_get(result, 0), JSON_ARRAY)) {
        const char *n1 = json_string(json_array_get(result, 1));
        int         n2 = json_i32(json_array_get(result, 2));
        if (!extranonce_ok(n1, n2)) {
            ev->type = SV1_EV_ERROR;
        } else {
            copy_fixed(c->xnonce1, sizeof c->xnonce1, n1);
            c->xnonce2_size = n2;
            if (c->state < SV1_SUBSCRIBED)
                c->state = SV1_SUBSCRIBED;
            ev->type = SV1_EV_SUBSCRIBED;
        }
    } else if (c->state < SV1_CONFIGURED) {
        c->version_rolling = 0;
        c->state           = SV1_CONFIGURED;
        ev->type           = SV1_EV_CONFIGURED;
    } else if (c->state < SV1_AUTHORIZED && reply_id && reply_id == c->id_authorize) {
        if (json_bool(result) && !has_error) {
            c->state = SV1_AUTHORIZED;
            if (c->has_job)
                c->state = SV1_MINING;
            ev->type = SV1_EV_AUTHORIZED;
        } else {
            ev->type = SV1_EV_ERROR;
        }
    } else {
        ev->submit_id = reply_id;
        int own_req   = ev->submit_id &&
                (ev->submit_id == c->id_configure || ev->submit_id == c->id_subscribe ||
                        ev->submit_id == c->id_authorize || ev->submit_id == c->id_xnsub);
        int claimed = !own_req && inflight_claim(ev->submit_id);
        if (own_req) {
            ev->type = SV1_EV_NONE;
        } else if (claimed ||
                (result && !answered_seen(ev->submit_id) && id_issued(ev->submit_id))) {
            ev->submit_accepted = json_bool(result);
            ev->submit_lost     = !claimed;
            if (ev->submit_accepted)
                c->accepted++;
            else
                c->rejected++;
            if (!ev->submit_accepted)
                capture_error(c, ev, error);
            answered_mark(ev->submit_id);
            ev->type = SV1_EV_SUBMIT_RESULT;
        } else {
            c->untracked++;
            ev->type = SV1_EV_SUBMIT_UNTRACKED;
        }
    }

    json_free(v);
    return 0;
}
