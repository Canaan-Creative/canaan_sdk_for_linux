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

#ifndef INI_FILE_H
#define INI_FILE_H

#include "ini.h"

typedef int (*ini_file_update_fn)(ini_t *ini, void *ctx);

ini_t *ini_file_load(const char *path);
char  *ini_file_serialize(const ini_t *ini);
int    ini_file_update(const char *path, ini_file_update_fn update, void *ctx);
int    ini_file_write(const char *path, const char *text, size_t len);
int    ini_file_seed(const char *path, const char *src);
int    ini_file_fill(const char *path, const char *src, int *recreated);
int    ini_file_head_fill(const char *path, const char *src);

#endif /* INI_FILE_H */
