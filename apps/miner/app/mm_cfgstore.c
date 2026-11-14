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

#include "mm_cfgstore.h"
#include "ini_cache.h"
#include "ini_file.h"
#include "mc_api_bootby.h"
#include "mc_paths.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define BOOTBY_TEXT_MAX 32 /* stored boot reason value incl. NUL, bytes */

static ini_cache_t     g_store; /* systemcfg.ini RAM copy */
static int             g_open;
static atomic_int      g_frozen;                                  /* factory recovery freeze flag */
static pthread_mutex_t g_freeze_lock = PTHREAD_MUTEX_INITIALIZER; /* orders writes against freeze */

int mm_cfgstore_open(const char *path)
{
    if (g_open)
        ini_cache_close(&g_store);
    g_open = 1;
    return ini_cache_open(&g_store, path ? path : MC_SYSTEMCFG_DEFAULT);
}

int mm_cfgstore_get(const char *section, const char *key, char *buf, size_t cap)
{
    if (!g_open)
        return -1;
    return ini_cache_get_copy(&g_store, section, key, buf, cap);
}

int mm_cfgstore_set(const char *section, const char *key, const char *value)
{
    int rc = -1;
    pthread_mutex_lock(&g_freeze_lock);
    if (g_open && !atomic_load(&g_frozen))
        rc = ini_cache_set(&g_store, section, key, value);
    pthread_mutex_unlock(&g_freeze_lock);
    return rc;
}

int mm_cfgstore_set_n(const char *const *sections, const char *const *keys,
        const char *const *values, size_t n)
{
    int rc = -1;
    pthread_mutex_lock(&g_freeze_lock);
    if (g_open && !atomic_load(&g_frozen))
        rc = ini_cache_set_n(&g_store, sections, keys, values, n);
    pthread_mutex_unlock(&g_freeze_lock);
    return rc;
}

int mm_cfgstore_put(const char *section, const char *key, const char *value)
{
    if (!g_open)
        return -1;
    return ini_cache_set(&g_store, section, key, value);
}

int mm_cfgstore_commit(const char *section, const char *const *keys, const char *const *values,
        size_t n)
{
    int rc = -1;
    pthread_mutex_lock(&g_freeze_lock);
    if (g_open && !atomic_load(&g_frozen))
        rc = ini_cache_commit(&g_store, section, keys, values, n);
    pthread_mutex_unlock(&g_freeze_lock);
    return rc;
}

int mm_cfgstore_flush(void)
{
    if (atomic_load(&g_frozen))
        return -1;
    return mm_cfgstore_sync();
}

int mm_cfgstore_sync(void)
{
    if (!g_open)
        return 0;
    return ini_cache_flush(&g_store);
}

int mm_cfgstore_reload(void)
{
    if (!g_open)
        return -1;
    return ini_cache_reload(&g_store);
}

int mm_cfgstore_fill(const char *defaults_path)
{
    if (!g_open)
        return -1;
    ini_t *defaults = ini_load(defaults_path);
    if (!defaults)
        return -1;
    int rc = ini_cache_fill(&g_store, defaults);
    ini_free(defaults);
    return rc;
}

int mm_cfgstore_recreate(const char *defaults_path)
{
    int recreated;

    if (!g_open || g_store.ini)
        return -1;
    if (ini_file_fill(g_store.path, defaults_path, &recreated) < 0 || !recreated)
        return -1;
    return ini_cache_reload(&g_store);
}

void mm_cfgstore_freeze(int on)
{
    pthread_mutex_lock(&g_freeze_lock);
    atomic_store(&g_frozen, on);
    pthread_mutex_unlock(&g_freeze_lock);
}

int mm_cfgstore_frozen(void)
{
    return atomic_load(&g_frozen);
}

int mm_bootby_stage(uint8_t reason, uint32_t sub)
{
    char              r[BOOTBY_TEXT_MAX], s[BOOTBY_TEXT_MAX];
    const char *const sects[] = { MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_SECT };
    const char *const keys[]  = { MC_BOOTBY_INI_REASON, MC_BOOTBY_INI_SUB };
    const char *const vals[]  = { r, s };
    if (!g_open)
        return -1;
    snprintf(r, sizeof r, "%u", reason);
    snprintf(s, sizeof s, "%u", sub);
    return ini_cache_set_n(&g_store, sects, keys, vals, 2);
}

int mm_bootby_persist(uint8_t reason, uint32_t sub)
{
    if (mm_bootby_stage(reason, sub) != 0)
        return -1;
    if (atomic_load(&g_frozen))
        return 0;
    return mm_cfgstore_flush() < 0 ? -1 : 0;
}

void mm_bootby_recover(int wdt_bootstatus, uint8_t *out_reason, uint32_t *out_sub)
{
    char reason[BOOTBY_TEXT_MAX];
    char sub[BOOTBY_TEXT_MAX];
    int  have_reason =
            mm_cfgstore_get(MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_REASON, reason, sizeof reason) == 0;
    int have_sub = mm_cfgstore_get(MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_SUB, sub, sizeof sub) == 0;

    mc_bootby_resolve(have_reason ? reason : NULL, have_sub ? sub : NULL, wdt_bootstatus,
            out_reason, out_sub);
    (void)mm_bootby_persist(MC_BOOTBY_POWRON, 0);
}
