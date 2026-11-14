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

#ifndef INI_H
#define INI_H

#include <stddef.h>

/* Largest file ini_load() reads; a bigger one fails with EFBIG. */
#define INI_MAX_BYTES (64 * 1024)

typedef struct ini ini_t;

ini_t      *ini_parse(const char *text);
ini_t      *ini_load(const char *path);
const char *ini_get(const ini_t *ini, const char *section, const char *key);
int         ini_set(ini_t *ini, const char *section, const char *key, const char *value);
int         ini_fill(ini_t *dst, const ini_t *src);
size_t      ini_write(const ini_t *ini, char *out, size_t cap);
void        ini_free(ini_t *ini);

#endif /* INI_H */
