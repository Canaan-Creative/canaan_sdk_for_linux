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

#ifndef POOL_CONFIG_H
#define POOL_CONFIG_H

#include "mc_paths.h"
#include "pool.h"

typedef int (*pool_config_update_fn)(pool_set *ps, void *ctx);

int pool_config_load(pool_set *ps, const char *path);
int pool_config_load_json(pool_set *ps, const char *json);
int pool_config_save(const pool_set *ps, const char *path);
int pool_config_update(const char *path, pool_config_update_fn update, void *ctx);
int pool_config_apply_slot(pool_set *ps, int slot, const char *url, const char *user,
        const char *pass);
int pool_config_apply_slots(pool_set *ps, void *slots);

#endif /* POOL_CONFIG_H */
