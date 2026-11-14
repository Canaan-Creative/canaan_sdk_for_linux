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

#ifndef WEBFORM_H
#define WEBFORM_H

#include <stddef.h>

#include "pool.h"

size_t web_url_decode(char *dst, size_t cap, const char *src, size_t len);
int    web_form_get(const char *body, const char *key, char *out, size_t cap);
int    web_pools_from_form(const char *body, pool_set *ps);

#endif /* WEBFORM_H */
