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

#ifndef MM_TZ_H
#define MM_TZ_H

#include <stddef.h>

#include "mc_set_result.h"
#include "miner_ipc.h"

#define MM_TZ_ZONEINFO_DIR "/usr/share/zoneinfo" /* IANA zoneinfo root, holds zone files */

/* symlink to the active zone file, replaced atomically via rename(); read by tzset() */
#define MM_TZ_LOCALTIME_PATH "/etc/localtime"

mc_set_result mm_apply_settz(const char *args);
mc_set_result mm_tz_apply_default(const char *name);
void          mm_tz_restore(void);
void          mm_tz_get(char *out, size_t cap);

#endif /* MM_TZ_H */
