/* SPDX-License-Identifier: MIT */
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

#ifndef INI_CACHE_H
#define INI_CACHE_H

#include "ini.h"

#include <pthread.h>
#include <stddef.h>

#define INI_CACHE_PATH_MAX 256 /* longest cached file path incl. NUL, bytes */

typedef struct {
    pthread_mutex_t lock;       /* guards ini */
    pthread_mutex_t flush_lock; /* serializes flush, commit and reload; guards saved */
    char            path[INI_CACHE_PATH_MAX];
    ini_t          *ini;   /* RAM copy, NULL when the file could not be read */
    char           *saved; /* serialized copy last read from or written to the file */
} ini_cache_t;

int ini_cache_open(ini_cache_t *c, const char *path);
int ini_cache_get_copy(ini_cache_t *c, const char *section, const char *key, char *buf, size_t cap);
int ini_cache_set(ini_cache_t *c, const char *section, const char *key, const char *value);
int ini_cache_set_n(ini_cache_t *c, const char *const *sections, const char *const *keys,
        const char *const *values, size_t n);
int ini_cache_fill(ini_cache_t *c, const ini_t *defaults);
int ini_cache_flush(ini_cache_t *c);
int ini_cache_commit(ini_cache_t *c, const char *section, const char *const *keys,
        const char *const *values, size_t n);
int ini_cache_reload(ini_cache_t *c);
void ini_cache_close(ini_cache_t *c);

#endif /* INI_CACHE_H */
