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

#include "cali.h"
#include "board_cali.h"
#include "cfg_u32.h"
#include "ini.h"
#include "ini_file.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CURING_DATA_CNT (sizeof(g_curing_data) / sizeof(g_curing_data[0]))

/* Factory calibration row for one ASIC count */
typedef struct {
    uint16_t   hash_asics;           /* ASIC count the row applies to */
    cali_param param[CALI_MODE_MAX]; /* calibration per work mode */
} cali_curing_data;

/* One INI entry to set */
struct cali_kv {
    const char *sect, *key, *val;
};

static const cali_curing_data g_curing_data[] = { BOARD_CALI_ROWS };

/* Parsed copy of the last calibration INI read */
static struct {
    pthread_mutex_t lock; /* guards path and ini */
    char           *path; /* file the cache mirrors, NULL when empty */
    ini_t          *ini;  /* parsed copy, NULL when the file could not be read */
} g_cache = { .lock = PTHREAD_MUTEX_INITIALIZER };

const cali_param *cali_default_param(uint16_t asic_cnt, uint8_t mode)
{
    if (mode < CALI_MODE_MAX) {
        for (size_t i = 0; i < CURING_DATA_CNT; i++) {
            if (g_curing_data[i].hash_asics == asic_cnt)
                return &g_curing_data[i].param[mode];
        }
    }
    return &g_curing_data[0].param[0];
}

int cali_format(const cali_param *p, char *buf, size_t cap)
{
    if (!p || !buf)
        return -1;
    int n = snprintf(buf, cap, "%u-%u-%u-%u-%u", p->max_pout, p->temp, p->volt, p->pll_start,
            p->pll_interval);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int cali_parse(const char *str, cali_param *out)
{
    if (!str || !out)
        return -1;
    unsigned mp, tp, vt, ps, pi;
    if (sscanf(str, "%u-%u-%u-%u-%u", &mp, &tp, &vt, &ps, &pi) != CALI_FIELD_COUNT)
        return -1;
    if (mp == 0 || tp == 0 || vt == 0 || ps == 0 || pi == 0)
        return -1;
    out->max_pout     = (uint16_t)mp;
    out->temp         = (uint16_t)tp;
    out->volt         = (uint16_t)vt;
    out->pll_start    = (uint16_t)ps;
    out->pll_interval = (uint16_t)pi;
    return 0;
}

static int cache_holds(const char *path)
{
    return g_cache.path && strcmp(g_cache.path, path) == 0;
}

static void cache_drop(void)
{
    ini_free(g_cache.ini);
    free(g_cache.path);
    g_cache.ini  = NULL;
    g_cache.path = NULL;
}

static const ini_t *cache_get(const char *path)
{
    if (cache_holds(path))
        return g_cache.ini;

    cache_drop();
    g_cache.path = strdup(path);
    if (g_cache.path)
        g_cache.ini = ini_load(path);
    return g_cache.ini;
}

int cali_get(const char *path, uint8_t mode, int8_t level, char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    if (level < 0 || level >= CALI_LEVEL_COUNT)
        return -1;

    char sect[16], key[16];
    snprintf(sect, sizeof sect, "mode%d", mode);
    snprintf(key, sizeof key, "cali_param%d", level);

    pthread_mutex_lock(&g_cache.lock);
    const ini_t *ini = cache_get(path);
    const char  *v   = ini ? ini_get(ini, sect, key) : NULL;
    if (v)
        snprintf(out, cap, "%s", v);
    pthread_mutex_unlock(&g_cache.lock);
    return 0;
}

static int set_kv(ini_t *ini, void *ctx)
{
    const struct cali_kv *kv = ctx;
    return ini_set(ini, kv->sect, kv->key, kv->val);
}

int cali_set(const char *path, uint8_t mode, int8_t level, const char *val)
{
    if (!val || level < 0 || level >= CALI_LEVEL_COUNT)
        return -1;

    char sect[16], key[16];
    snprintf(sect, sizeof sect, "mode%d", mode);
    snprintf(key, sizeof key, "cali_param%d", level);
    struct cali_kv kv = { sect, key, val };
    int            rc = ini_file_update(path, set_kv, &kv);

    if (rc == 0) {
        pthread_mutex_lock(&g_cache.lock);
        if (cache_holds(path)) {
            if (!g_cache.ini)
                g_cache.ini = ini_parse("");
            if (!g_cache.ini || set_kv(g_cache.ini, &kv) != 0)
                cache_drop();
        }
        pthread_mutex_unlock(&g_cache.lock);
    }
    return rc;
}

static int workmode_read(const cali_wm_store *s)
{
    char     buf[16];
    uint32_t mode;

    if (s->get(CALI_WORKMODE_SECT, CALI_WORKMODE_KEY, buf, sizeof buf) != 0 ||
            cfg_u32_parse(buf, UINT32_MAX, &mode) != 0 || mode >= CALI_MODE_MAX)
        return -1;
    return (int)mode;
}

uint8_t cali_workmode_get(const cali_wm_store *s)
{
    int mode = workmode_read(s);
    return mode < 0 ? CALI_MODE_DEFAULT : (uint8_t)mode;
}

int cali_workmode_set(const cali_wm_store *s, uint8_t mode)
{
    char buf[16];

    if (mode >= CALI_MODE_MAX)
        return -1;
    snprintf(buf, sizeof buf, "%u", (unsigned)mode);
    return s->set(CALI_WORKMODE_SECT, CALI_WORKMODE_KEY, buf) == 0 ? 0 : -1;
}

int cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level, cali_param *out,
        uint8_t *aging_finished)
{
    if (!out)
        return -1;

    char raw[CALI_STR_MAX];
    if (cali_get(path, mode, level, raw, sizeof raw) == 0 && raw[0] && cali_parse(raw, out) == 0) {
        if (aging_finished)
            *aging_finished = 1;
        return 0;
    }

    *out = *cali_default_param(asic_cnt, mode);
    if (aging_finished)
        *aging_finished = 0;
    return 0;
}
