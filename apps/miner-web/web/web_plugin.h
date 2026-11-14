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

#ifndef WEB_PLUGIN_H
#define WEB_PLUGIN_H

#include <stddef.h>

typedef struct {
    const char        *path;
    const char *const *top;
    const char *const *ascset;
} web_rest_resource;

int                      web_plugin_file(const char *name, char *path, size_t cap, int *concat);
const web_rest_resource *web_plugin_rest(size_t *n);

#endif /* WEB_PLUGIN_H */
