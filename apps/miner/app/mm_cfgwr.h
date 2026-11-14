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

#ifndef MM_CFGWR_H
#define MM_CFGWR_H

#include <stddef.h>

#include "cfgwr_proto.h"

int  mm_cfgwr_apply(const mm_cfgwr_req *req, const char *systemcfg_path);
int  mm_cfgwr_start(const char *pools_path, const char *systemcfg_path);
void mm_cfgwr_webpass_set(const char *hex);
void mm_cfgwr_webpass_get(char *out, size_t cap);
void mm_cfgwr_hwcfg_set(const char *prod, const char *model);
void mm_cfgwr_hwcfg_get(char *prod, size_t prodcap, char *model, size_t modelcap);
int  mm_cfgwr_activation_get(void);
int  mm_cfgwr_pool_configured(void);
void mm_cfgwr_seed(const char *systemcfg_path);
int  mm_cfgwr_wp_clear_dir(const char *dir, const char *keep);

#endif /* MM_CFGWR_H */
