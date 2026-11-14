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

#ifndef MC_API_ESTATS_H
#define MC_API_ESTATS_H

#include <stddef.h>

#include "mc_api_types.h"

size_t mc_api_stats_(const mc_api_stats *s, char *buf, size_t cap, int show_all, int with_pools);
size_t mc_api_stats_text(const mc_api_stats *s, char *buf, size_t cap, int show_all,
        int with_pools);
size_t mc_api_litestats_(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_litestats_text(const mc_api_stats *s, char *buf, size_t cap);
int    mc_api_estats_all(const char *req);

#endif /* MC_API_ESTATS_H */
