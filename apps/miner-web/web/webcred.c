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

#include "webcred.h"
#include "ini_file.h"
#include "mc_paths.h"
#include "web_logfmt.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define WEB_AUTH_FAIL_MAX    5u
#define WEB_AUTH_FAIL_WIN_MS 60000u
#define WEB_AUTH_LOCK_MS     60000u
#define WEB_AUTH_GUARD_N     16

typedef struct {
    char     addr[48];
    uint64_t used_ms;
    uint64_t fail_since_ms;
    uint64_t lock_until_ms;
    unsigned fail_n;
    int      used;
} auth_guard;

typedef struct {
    const char *failures;
    const char *checks;
    auth_guard  slot[WEB_AUTH_GUARD_N];
} auth_guard_set;

/* One slot: a single systemcfg path is live per process, so a one-entry cache
 * covers every call. Guarded because civetweb serves requests from a pool. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* guards the cache entry */
static char            g_path[256];
static struct timespec g_mtime; /* mtime of g_path when g_cred was loaded */
static mc_webcred      g_cred;
static int             g_valid; /* 1 when the cache entry is filled */
static pthread_mutex_t g_guard_lock  = PTHREAD_MUTEX_INITIALIZER;
static auth_guard_set  g_login_guard = { .failures = "wrong web logins", .checks = "login checks" };
static auth_guard_set  g_proof_guard = { .failures = "rejected activation or QR proofs",
     .checks                                       = "activation and QR requests" };

int web_creds_load(const char *path, mc_webcred *out)
{
    struct stat st;

    if (!path || !out)
        return -1;

    int stamped = stat(path, &st) == 0;
    if (!stamped && errno == ENOENT && ini_file_seed(path, MC_SYSTEMCFG_FACTORY) > 0) {
        const char *name = strrchr(path, '/');

        web_logf(stderr, "INFO", "%s missing — copied from the factory default\n",
                name ? name + 1 : path);
        stamped = stat(path, &st) == 0;
    }

    pthread_mutex_lock(&g_lock);
    if (g_valid && stamped && strcmp(g_path, path) == 0 && g_mtime.tv_sec == st.st_mtim.tv_sec &&
            g_mtime.tv_nsec == st.st_mtim.tv_nsec) {
        *out = g_cred;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_lock);

    mc_webcred c;
    if (mc_api_load_creds(&c, path) != 0)
        return -1;

    /* Cache only when the load could be stamped; otherwise fall back to a
     * fresh parse next time rather than serve an un-dated entry. */
    if (stamped) {
        pthread_mutex_lock(&g_lock);
        snprintf(g_path, sizeof g_path, "%s", path);
        g_mtime = st.st_mtim;
        g_cred  = c;
        g_valid = 1;
        pthread_mutex_unlock(&g_lock);
    }

    *out = c;
    return 0;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static auth_guard *guard_victim(auth_guard_set *set, uint64_t now)
{
    auth_guard *old = NULL;

    for (size_t i = 0; i < WEB_AUTH_GUARD_N; i++) {
        auth_guard *g = &set->slot[i];
        if (g->used && now < g->lock_until_ms)
            continue;
        if (!old || (old->used && (!g->used || g->used_ms < old->used_ms)))
            old = g;
    }
    return old;
}

static auth_guard *guard_find(auth_guard_set *set, const char *addr, uint64_t now, int add)
{
    auth_guard *old;

    if (!addr)
        addr = "";
    for (size_t i = 0; i < WEB_AUTH_GUARD_N; i++) {
        auth_guard *g = &set->slot[i];
        if (g->used && !strcmp(g->addr, addr))
            return g;
    }
    if (!add || !(old = guard_victim(set, now)))
        return NULL;
    memset(old, 0, sizeof *old);
    snprintf(old->addr, sizeof old->addr, "%s", addr);
    old->used    = 1;
    old->used_ms = now;
    return old;
}

static int auth_guard_open(auth_guard_set *set, const char *addr)
{
    int open;
    pthread_mutex_lock(&g_guard_lock);
    uint64_t    now = mono_ms();
    auth_guard *g   = guard_find(set, addr, now, 0);
    open            = g ? now >= g->lock_until_ms : guard_victim(set, now) != NULL;
    pthread_mutex_unlock(&g_guard_lock);
    return open;
}

static int auth_guard_close(auth_guard_set *set, const char *addr, int ok)
{
    pthread_mutex_lock(&g_guard_lock);
    uint64_t    now = mono_ms();
    auth_guard *g   = guard_find(set, addr, now, !ok);
    if (g && now < g->lock_until_ms) {
        ok = 0;
    } else if (ok) {
        if (g)
            g->fail_n = 0;
    } else if (g) {
        g->used_ms = now;
        if (!g->fail_n || now - g->fail_since_ms >= WEB_AUTH_FAIL_WIN_MS) {
            g->fail_since_ms = now;
            g->fail_n        = 0;
        }
        if (++g->fail_n >= WEB_AUTH_FAIL_MAX) {
            g->fail_n        = 0;
            g->lock_until_ms = now + WEB_AUTH_LOCK_MS;
            web_logf(stderr, "WARN", "%u %s in %u s, %s refused for %u s\n", WEB_AUTH_FAIL_MAX,
                    set->failures, WEB_AUTH_FAIL_WIN_MS / 1000u, set->checks,
                    WEB_AUTH_LOCK_MS / 1000u);
        }
    }
    pthread_mutex_unlock(&g_guard_lock);
    return ok;
}

int web_auth_password(const mc_webcred *c, const char *user, const char *digest, const char *addr)
{
    return auth_guard_open(&g_login_guard, addr) &&
            auth_guard_close(&g_login_guard, addr, mc_api_check(c, user, digest));
}

int web_auth_login(const mc_webcred *c, const char *proof, const char *addr)
{
    return auth_guard_open(&g_login_guard, addr) &&
            auth_guard_close(&g_login_guard, addr, mc_api_check_cookie(c, proof));
}

int web_proof_guard_open(const char *addr)
{
    return auth_guard_open(&g_proof_guard, addr);
}

void web_proof_guard_fail(const char *addr)
{
    auth_guard_close(&g_proof_guard, addr, 0);
}
