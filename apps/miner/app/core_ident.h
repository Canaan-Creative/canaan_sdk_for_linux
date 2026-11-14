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

#ifndef CORE_IDENT_H
#define CORE_IDENT_H

#include <stddef.h>

#include "mc_api_types.h"
#include "mc_sizes.h"

/* Caller-owned copy of the product and model IDs */
typedef struct {
    char prod[PROD_NAME_CAP];
    char model[MODEL_NAME_CAP];
} core_hwcfg_buf;

void core_hwcfg_set(const char *prod, const char *model);
void core_hwcfg_copy(core_hwcfg_buf *out);
void core_hwcfg_apply(mc_api_stats *s, core_hwcfg_buf *store);
void core_webpass_sha256_hex(const char *plain, size_t len, char out[WEBPASS_HEX_LEN + 1]);
void core_webpass_cache_set(const char *hex);
int  core_webpass_cached_copy(char *out, size_t cap);
int  core_webpass_matches(const char *plain, size_t len);
int  core_webpass_proof_matches(const char *expect, const char *given);

#endif /* CORE_IDENT_H */
