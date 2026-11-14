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

#ifndef CFGWR_CLIENT_H
#define CFGWR_CLIENT_H

#include "pool_config.h"

int cfgwr_write_webpass(const char *hex, const char *path);
int cfgwr_write_hwcfg(const char *prod, const char *model, const char *path);
int cfgwr_write_activation(const char *path);
int cfgwr_write_wallpaper(unsigned uid, const char *format);
int cfgwr_clear_wallpaper(void);
int cfgwr_update_pools(const char *path, pool_config_update_fn update, void *ctx);
int cfgwr_read_pools(const char *path, pool_set *out);

#endif /* CFGWR_CLIENT_H */
