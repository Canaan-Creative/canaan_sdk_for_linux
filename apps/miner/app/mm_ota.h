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

#ifndef MM_OTA_H
#define MM_OTA_H

#include "ota_proto.h"

#define MM_OTA_REBOOT_DELAY_MS 3000u /* successful upgrade to reboot, ms */

/*
 * Success latch: created after the slot switch is committed; every later upload is refused
 * while it exists. Nothing deletes it, only a reboot clears it, so /tmp must be tmpfs.
 */
#define OTA_SUCCEED_FILE "/tmp/up_scceed_file"

/* key=slot lines for ubootenv_apply_list(), deleted after the env write */
#define OTA_ENV_LIST_FILE "/tmp/up_env_list"

#define OTA_UBI_SYSFS "/sys/class/ubi"

int mm_ota_start(void);

#endif /* MM_OTA_H */
