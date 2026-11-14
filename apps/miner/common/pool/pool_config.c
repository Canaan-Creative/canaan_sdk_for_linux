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

#include "pool_config.h"
#include "file_atomic.h"
#include "jread.h"
#include "jwrite.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *read_file_alloc(const char *path, int *missing)
{
    FILE *f = fopen(path, "rbe");
    if (missing)
        *missing = !f && errno == ENOENT;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || n > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    return buf;
}

int pool_config_load_json(pool_set *ps, const char *json)
{
    json_value *root = json_parse(json);
    if (!root)
        return -1;

    const json_value *pools  = json_object_get(root, "pools");
    int               loaded = 0;

    pool_init(ps, ps->backoff_base_ms, ps->backoff_max_ms);

    for (int i = 0; i < POOL_VALID_NUM; i++) {
        char key[16];
        snprintf(key, sizeof key, "pool%d", i);
        const json_value *p = json_object_get(pools, key);
        if (!p)
            continue;
        const char *url  = json_string(json_object_get(p, "url"));
        const char *user = json_string(json_object_get(p, "user"));
        const char *pass = json_string(json_object_get(p, "pass"));
        if (!url[0])
            continue;
        int idx = pool_add(ps, url, user, pass);
        if (idx >= 0) {
            ps->pools[idx].slot = i;
            if (json_bool(json_object_get(p, "disabled")))
                ps->pools[idx].enabled = 0;
            loaded++;
        }
    }

    json_free(root);
    return loaded;
}

int pool_config_load(pool_set *ps, const char *path)
{
    char *buf = read_file_alloc(path, NULL);
    if (!buf)
        return -1;

    int rc = pool_config_load_json(ps, buf);
    free(buf);
    return rc;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static const char *skip_value(const char *p)
{
    int depth = 0, in_str = 0;
    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == '"')
                in_str = 0;
            continue;
        }
        switch (*p) {
        case '"':
            in_str = 1;
            break;
        case '{':
        case '[':
            depth++;
            break;
        case '}':
        case ']':
            if (depth == 0)
                return p;
            if (--depth == 0)
                return p + 1;
            break;
        case ',':
            if (depth == 0)
                return p;
            break;
        }
    }
    return p;
}

static int find_pools_value(const char *text, size_t *vs, size_t *ve)
{
    const char *p = skip_ws(text);
    if (*p != '{')
        return 0;
    p = skip_ws(p + 1);
    while (*p == '"') {
        const char *k = ++p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        if (*p != '"')
            return 0;
        size_t klen = (size_t)(p - k);
        p           = skip_ws(p + 1);
        if (*p != ':')
            return 0;
        p               = skip_ws(p + 1);
        const char *v   = p;
        const char *end = skip_value(p);
        if (klen == 5 && !memcmp(k, "pools", 5)) {
            *vs = (size_t)(v - text);
            *ve = (size_t)(end - text);
            return 1;
        }
        p = skip_ws(end);
        if (*p != ',')
            break;
        p = skip_ws(p + 1);
    }
    return 0;
}

static const pool_t *slot_pool(const pool_set *ps, int slot)
{
    const pool_t *p = NULL;
    for (int i = 0; i < ps->count; i++)
        if (ps->pools[i].slot == slot)
            p = &ps->pools[i];
    return p;
}

static size_t build_pools(const pool_set *ps, char *out, size_t cap)
{
    jwriter w;
    jw_init(&w, out, cap);
    jw_obj_open(&w);
    for (int s = 0; s < POOL_VALID_NUM; s++) {
        const pool_t *p = slot_pool(ps, s);
        char          key[16];
        snprintf(key, sizeof key, "pool%d", s);
        jw_key_obj(&w, key);
        jw_kv_str(&w, "url", p ? p->url : "");
        jw_kv_str(&w, "user", p ? p->user : "");
        jw_kv_str(&w, "pass", p ? p->pass : "");
        if (p && !p->enabled)
            jw_kv_raw(&w, "disabled", "true");
        jw_obj_close(&w);
    }
    jw_obj_close(&w);
    return jw_ok(&w) ? jw_len(&w) : 0;
}

int pool_config_save(const pool_set *ps, const char *path)
{
    if (!ps || !path || !*path)
        return -1;

    char   pools[4096];
    size_t plen = build_pools(ps, pools, sizeof pools);
    if (!plen)
        return -1;

    char       *old    = read_file_alloc(path, NULL);
    json_value *root   = old ? json_parse(old) : NULL;
    size_t      oldlen = old ? strlen(old) : 0, vs = 0, ve = 0;
    int         splice = root && json_is(root, JSON_OBJECT) && find_pools_value(old, &vs, &ve);

    size_t cap = oldlen + plen + 32;
    char  *doc = malloc(cap);
    int    rc  = -1;
    if (doc) {
        size_t len;
        if (splice) {
            memcpy(doc, old, vs);
            memcpy(doc + vs, pools, plen);
            memcpy(doc + vs + plen, old + ve, oldlen - ve);
            len = vs + plen + (oldlen - ve);
        } else {
            len = (size_t)snprintf(doc, cap, "{\n  \"pools\": %s\n}\n", pools);
        }
        rc = file_write_atomic(path, doc, len);
        free(doc);
    }

    json_free(root);
    free(old);
    return rc;
}

int pool_config_update(const char *path, pool_config_update_fn update, void *ctx)
{
    if (!path || !*path || !update)
        return -1;
    int lfd = file_lock(path);
    if (lfd < 0)
        return -1;

    int      rc = -1, missing;
    pool_set ps;
    pool_init(&ps, 0, 0);
    char *old = read_file_alloc(path, &missing);

    if ((old ? pool_config_load_json(&ps, old) >= 0 : missing) && update(&ps, ctx) == 0)
        rc = pool_config_save(&ps, path);
    free(old);

    close(lfd);
    return rc;
}

int pool_config_apply_slot(pool_set *ps, int slot, const char *url, const char *user,
        const char *pass)
{
    if (!ps || slot < 0 || slot >= POOL_VALID_NUM)
        return -1;
    int at = -1, ins = ps->count;
    for (int i = ps->count - 1; i >= 0; i--) {
        if (ps->pools[i].slot == slot)
            at = i;
        if (ps->pools[i].slot > slot)
            ins = i;
    }

    if (!url || !*url || !user || !*user || !pass || !*pass) {
        if (at >= 0) {
            for (int i = at; i + 1 < ps->count; i++)
                ps->pools[i] = ps->pools[i + 1];
            ps->count--;
            memset(&ps->pools[ps->count], 0, sizeof(pool_t));
        }
        return 0;
    }
    if (strlen(url) >= POOL_URL_MAX || strlen(user) >= POOL_USER_MAX ||
            strlen(pass) >= POOL_PASS_MAX)
        return -1;

    pool_t *p;
    if (at >= 0) {
        p = &ps->pools[at];
    } else {
        if (ps->count >= POOL_VALID_NUM)
            return -1;
        for (int i = ps->count; i > ins; i--)
            ps->pools[i] = ps->pools[i - 1];
        p = &ps->pools[ins];
        memset(p, 0, sizeof *p);
        p->enabled = 1;
        p->slot    = slot;
        ps->count++;
    }
    snprintf(p->url, sizeof p->url, "%s", url);
    snprintf(p->user, sizeof p->user, "%s", user);
    snprintf(p->pass, sizeof p->pass, "%s", pass);
    p->alive = 1;
    return 0;
}

int pool_config_apply_slots(pool_set *ps, void *slots)
{
    const pool_set *want = slots;
    if (!ps || !want)
        return -1;
    for (int s = 0; s < POOL_VALID_NUM; s++) {
        const pool_t *p                     = slot_pool(want, s);
        const pool_t *old                   = slot_pool(ps, s);
        char          old_url[POOL_URL_MAX] = "";
        if (old)
            snprintf(old_url, sizeof old_url, "%s", old->url);
        if (pool_config_apply_slot(ps, s, p ? p->url : "", p ? p->user : "", p ? p->pass : "") != 0)
            return -1;
        for (int i = 0; i < ps->count; i++)
            if (ps->pools[i].slot == s && strcmp(ps->pools[i].url, old_url) != 0)
                ps->pools[i].enabled = 1;
    }
    return 0;
}
