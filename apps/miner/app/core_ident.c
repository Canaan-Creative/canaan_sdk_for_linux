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

#include "core_ident.h"

#include "boardconf.h"
#include "hex.h"
#include "osif.h"
#include "sha256.h"
#include "webauth.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define WEBPASS_FAIL_MAX    5u
#define WEBPASS_FAIL_WIN_MS 60000u
#define WEBPASS_LOCK_MS     60000u

typedef struct {
    const char *what; /* checked secret, for the lockout log */
    uint64_t    fail_since_ms;
    unsigned    fail_n;
    uint64_t    lock_until_ms;
} webpass_guard;

static pthread_mutex_t g_hwcfg_lock            = PTHREAD_MUTEX_INITIALIZER;
static char            g_prod[PROD_NAME_CAP]   = PROD_NAME;
static char            g_model[MODEL_NAME_CAP] = MODEL;
static pthread_mutex_t g_pass_lock             = PTHREAD_MUTEX_INITIALIZER;
static char            g_pass[WEBPASS_HEX_LEN + 1]; /* web password SHA-256 hex */
static pthread_mutex_t g_guard_lock  = PTHREAD_MUTEX_INITIALIZER;
static webpass_guard   g_pass_guard  = { .what = "password" };
static webpass_guard   g_proof_guard = { .what = "QR login proof" };

void core_hwcfg_set(const char *prod, const char *model)
{
    pthread_mutex_lock(&g_hwcfg_lock);
    if (prod && *prod)
        snprintf(g_prod, sizeof g_prod, "%s", prod);
    if (model && *model)
        snprintf(g_model, sizeof g_model, "%s", model);
    pthread_mutex_unlock(&g_hwcfg_lock);
}

void core_hwcfg_copy(core_hwcfg_buf *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&g_hwcfg_lock);
    snprintf(out->prod, sizeof out->prod, "%s", g_prod);
    snprintf(out->model, sizeof out->model, "%s", g_model);
    pthread_mutex_unlock(&g_hwcfg_lock);
}

void core_hwcfg_apply(mc_api_stats *s, core_hwcfg_buf *store)
{
    if (!s || !store)
        return;
    core_hwcfg_copy(store);
    s->prod  = store->prod;
    s->model = store->model;
}

void core_webpass_sha256_hex(const char *plain, size_t len, char out[WEBPASS_HEX_LEN + 1])
{
    sha256_ctx    c;
    unsigned char d[SHA256_DIGEST_SIZE];
    sha256_init(&c);
    sha256_update(&c, plain, len);
    sha256_final(&c, d);
    hex_encode(out, d, sizeof d);
}

void core_webpass_cache_set(const char *hex)
{
    mc_webcred cred;
    memset(&cred, 0, sizeof cred);
    cred.loaded = 1;
    if (hex)
        snprintf(cred.pass, sizeof cred.pass, "%s", hex);
    int ok = hex && strlen(hex) == WEBPASS_HEX_LEN && !mc_api_webpass_unset(&cred);
    pthread_mutex_lock(&g_pass_lock);
    if (ok)
        snprintf(g_pass, sizeof g_pass, "%s", hex);
    else
        g_pass[0] = 0;
    pthread_mutex_unlock(&g_pass_lock);
}

int core_webpass_cached_copy(char *out, size_t cap)
{
    int ret = -1;
    if (!out || !cap)
        return -1;
    out[0] = 0;
    pthread_mutex_lock(&g_pass_lock);
    if (strlen(g_pass) == WEBPASS_HEX_LEN && cap > WEBPASS_HEX_LEN) {
        memcpy(out, g_pass, WEBPASS_HEX_LEN + 1);
        ret = 0;
    }
    pthread_mutex_unlock(&g_pass_lock);
    return ret;
}

static int webpass_equal(const char *a, const char *b, size_t n)
{
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

static int webpass_hash_matches(const char *plain, size_t len)
{
    char hex[WEBPASS_HEX_LEN + 1];
    int  ok;
    core_webpass_sha256_hex(plain, len, hex);
    pthread_mutex_lock(&g_pass_lock);
    ok = strlen(g_pass) == WEBPASS_HEX_LEN && webpass_equal(hex, g_pass, WEBPASS_HEX_LEN);
    pthread_mutex_unlock(&g_pass_lock);
    return ok;
}

static int webpass_guard_open(const webpass_guard *g)
{
    int open;
    pthread_mutex_lock(&g_guard_lock);
    open = osif_now_ms() >= g->lock_until_ms;
    pthread_mutex_unlock(&g_guard_lock);
    return open;
}

static int webpass_guard_close(webpass_guard *g, int ok)
{
    uint64_t now = osif_now_ms();
    pthread_mutex_lock(&g_guard_lock);
    if (now < g->lock_until_ms) {
        ok = 0;
    } else if (ok) {
        g->fail_n = 0;
    } else {
        if (!g->fail_n || now - g->fail_since_ms >= WEBPASS_FAIL_WIN_MS) {
            g->fail_since_ms = now;
            g->fail_n        = 0;
        }
        if (++g->fail_n >= WEBPASS_FAIL_MAX) {
            g->fail_n        = 0;
            g->lock_until_ms = now + WEBPASS_LOCK_MS;
            osif_log(OSIF_LOG_WARN, "%u wrong %s checks in %u s, %s checks refused for %u s",
                    WEBPASS_FAIL_MAX, g->what, WEBPASS_FAIL_WIN_MS / 1000u, g->what,
                    WEBPASS_LOCK_MS / 1000u);
        }
    }
    pthread_mutex_unlock(&g_guard_lock);
    return ok;
}

int core_webpass_matches(const char *plain, size_t len)
{
    return webpass_guard_open(&g_pass_guard) &&
            webpass_guard_close(&g_pass_guard, webpass_hash_matches(plain, len));
}

int core_webpass_proof_matches(const char *expect, const char *given)
{
    return webpass_guard_open(&g_proof_guard) &&
            webpass_guard_close(&g_proof_guard,
                    strlen(given) == strlen(expect) &&
                            webpass_equal(expect, given, strlen(expect)));
}
