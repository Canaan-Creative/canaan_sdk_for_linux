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

#include "ini_cache.h"
#include "ini_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load(const char *path, ini_t **ini, char **saved)
{
    ini_t *in = ini_file_load(path);
    if (!in)
        return -1;
    char *text = ini_file_serialize(in);
    if (!text) {
        ini_free(in);
        return -1;
    }
    *ini   = in;
    *saved = text;
    return 0;
}

int ini_cache_open(ini_cache_t *c, const char *path)
{
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->lock, NULL);
    pthread_mutex_init(&c->flush_lock, NULL);
    if (!path || !*path)
        return -1;
    if (snprintf(c->path, sizeof c->path, "%s", path) >= (int)sizeof c->path)
        return -1;
    return load(c->path, &c->ini, &c->saved);
}

int ini_cache_get_copy(ini_cache_t *c, const char *section, const char *key, char *buf, size_t cap)
{
    int rc = -1;
    pthread_mutex_lock(&c->lock);
    const char *v   = c->ini ? ini_get(c->ini, section, key) : NULL;
    size_t      len = v ? strlen(v) : 0;
    if (v && len < cap) {
        memcpy(buf, v, len + 1);
        rc = 0;
    }
    pthread_mutex_unlock(&c->lock);
    return rc;
}

int ini_cache_set(ini_cache_t *c, const char *section, const char *key, const char *value)
{
    int rc = -1;
    pthread_mutex_lock(&c->lock);
    if (c->ini)
        rc = ini_set(c->ini, section, key, value);
    pthread_mutex_unlock(&c->lock);
    return rc;
}

int ini_cache_set_n(ini_cache_t *c, const char *const *sections, const char *const *keys,
        const char *const *values, size_t n)
{
    int rc = -1;
    pthread_mutex_lock(&c->lock);
    if (c->ini) {
        rc = 0;
        for (size_t i = 0; i < n && rc == 0; i++)
            rc = ini_set(c->ini, sections[i], keys[i], values[i]);
    }
    pthread_mutex_unlock(&c->lock);
    return rc;
}

int ini_cache_fill(ini_cache_t *c, const ini_t *defaults)
{
    int rc = -1;
    pthread_mutex_lock(&c->lock);
    if (c->ini && defaults)
        rc = ini_fill(c->ini, defaults);
    pthread_mutex_unlock(&c->lock);
    return rc;
}

int ini_cache_flush(ini_cache_t *c)
{
    int rc = -1;
    pthread_mutex_lock(&c->flush_lock);
    pthread_mutex_lock(&c->lock);
    char *text = c->ini ? ini_file_serialize(c->ini) : NULL;
    pthread_mutex_unlock(&c->lock);
    if (text && c->saved && !strcmp(text, c->saved)) {
        rc = 0;
    } else if (text && ini_file_write(c->path, text, strlen(text)) == 0) {
        free(c->saved);
        c->saved = text;
        text     = NULL;
        rc       = 1;
    }
    free(text);
    pthread_mutex_unlock(&c->flush_lock);
    return rc;
}

static int set_all(ini_t *ini, const char *section, const char *const *keys,
        const char *const *values, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (ini_set(ini, section, keys[i], values[i]) != 0)
            return -1;
    return 0;
}

int ini_cache_commit(ini_cache_t *c, const char *section, const char *const *keys,
        const char *const *values, size_t n)
{
    int rc = -1;
    pthread_mutex_lock(&c->flush_lock);
    pthread_mutex_lock(&c->lock);
    char *text = c->ini ? ini_file_serialize(c->ini) : NULL;
    pthread_mutex_unlock(&c->lock);
    ini_t *next = text ? ini_parse(text) : NULL;
    free(text);
    text = NULL;
    if (next && set_all(next, section, keys, values, n) == 0)
        text = ini_file_serialize(next);
    if (text && ini_file_write(c->path, text, strlen(text)) == 0) {
        pthread_mutex_lock(&c->lock);
        if (set_all(c->ini, section, keys, values, n) != 0) {
            ini_free(c->ini);
            c->ini = next;
            next   = NULL;
        }
        pthread_mutex_unlock(&c->lock);
        free(c->saved);
        c->saved = text;
        text     = NULL;
        rc       = 0;
    }
    free(text);
    ini_free(next);
    pthread_mutex_unlock(&c->flush_lock);
    return rc;
}

int ini_cache_reload(ini_cache_t *c)
{
    ini_t *ini;
    char  *saved;
    pthread_mutex_lock(&c->flush_lock);
    int rc = load(c->path, &ini, &saved);
    if (rc == 0) {
        pthread_mutex_lock(&c->lock);
        ini_free(c->ini);
        c->ini = ini;
        pthread_mutex_unlock(&c->lock);
        free(c->saved);
        c->saved = saved;
    }
    pthread_mutex_unlock(&c->flush_lock);
    return rc;
}

void ini_cache_close(ini_cache_t *c)
{
    ini_free(c->ini);
    free(c->saved);
    c->ini   = NULL;
    c->saved = NULL;
    pthread_mutex_destroy(&c->flush_lock);
    pthread_mutex_destroy(&c->lock);
}
