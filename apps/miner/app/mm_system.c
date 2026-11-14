// SPDX-License-Identifier: MIT
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

#include "mc_api_bootby.h"
#include "mm_cfgstore.h"
#include "mm_internal.h"
#include "mm_ota.h"
#include "osif.h"

#include <errno.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int mm_system_reboot(uint8_t reason, uint32_t sub)
{
    int err;

    atomic_fetch_add(&g_rebooting, 1);
    osif_log(OSIF_LOG_OP, "[BOOT] Reboot reason 0x%02X sub 0x%08X, %s", reason, sub,
            mc_bootby_string(reason));
    if (reason && mm_bootby_stage(reason, sub) != 0)
        osif_log(OSIF_LOG_ERR, "reboot reason could not be saved");
    mm_cfg_flush();
    sync();
    (void)rail_power_off_for_reboot();
    reboot(RB_AUTOBOOT);
    err = errno;
    osif_log(OSIF_LOG_ERR, "reboot failed: %s", strerror(err));
    atomic_store(&g_reboot_failed, 1);
    return -1;
}

void ota_arm_reboot(void)
{
    atomic_store(&g_ota_reboot, 1);
    if (mm_bootby_persist(MC_BOOTBY_UPGRADE, 0) != 0)
        osif_log(OSIF_LOG_ERR, "reboot reason could not be saved");
    mm_cfg_flush();
    osif_log(OSIF_LOG_INFO,
            "firmware update done — rebooting into the new "
            "image in %u s",
            MM_OTA_REBOOT_DELAY_MS / 1000u);
    osif_sleep_ms(MM_OTA_REBOOT_DELAY_MS);
    if (mm_system_reboot(MC_BOOTBY_UPGRADE, 0) != 0) {
        atomic_store(&g_ota_reboot, 0);
        osif_log(OSIF_LOG_ERR,
                "reboot failed — the new firmware starts at "
                "the next power cycle");
    }
}

int mm_system_poweroff(void)
{
    int err;

    atomic_fetch_add(&g_rebooting, 1);
    osif_log(OSIF_LOG_OP, "powering off");
    mm_cfg_flush();
    sync();
    reboot(RB_POWER_OFF);
    err = errno;
    osif_log(OSIF_LOG_ERR, "power off failed: %s", strerror(err));
    osif_log(OSIF_LOG_OP, "power off failed — mining stays stopped");
    return -1;
}
