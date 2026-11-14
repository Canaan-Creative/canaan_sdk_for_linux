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

#include "mm_factory.h"
#include "mm_cfgstore.h"
#include "mm_cfgwr.h"
#include "mm_display.h"
#include "mm_efuse.h"
#include "mm_internal.h"
#include "mm_led.h"
#include "mc_api_bootby.h"
#include "mc_paths.h"
#include "mc_sizes.h"
#include "fsync_dir.h"
#include "ini_file.h"
#include "mm_system.h"
#include "mm_tz.h"
#include "osif.h"
#ifdef SUPPORT_BLE
#include "wifi_mgr.h"
#endif /* SUPPORT_BLE */

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* files copied from the factory default directory into the user config directory */
static const char *const factory_default_files[] = {
    "systemcfg.ini",
    "pools.conf",
#ifdef SUPPORT_BLE
    "wpa_supplicant.conf",
#endif /* SUPPORT_BLE */
    "interfaces",
};

static atomic_int g_factory_busy;

int mm_factory_in_progress(void)
{
    return atomic_load(&g_factory_busy);
}

static void factory_freeze(int on, int store_on)
{
    atomic_store(&g_factory_busy, on);
    mm_cfgstore_freeze(store_on);
#ifdef SUPPORT_BLE
    wifi_cfg_freeze(on);
#endif /* SUPPORT_BLE */
}

static void factory_fill_store(void)
{
    int n = mm_cfgstore_fill(MC_SYSTEMCFG_FACTORY);

    if (n > 0)
        osif_log(OSIF_LOG_INFO,
                "systemcfg.ini: %d missing setting(s) set from the factory defaults", n);
    else if (n < 0)
        osif_log(OSIF_LOG_ERR, "systemcfg.ini: missing settings not set from the factory defaults");
}

static void factory_fill_file(const char *path, const char *src)
{
    int recreated;
    int n = ini_file_fill(path, src, &recreated);

    if (recreated)
        osif_log(OSIF_LOG_WARN, "%s unreadable — recreated from %s", path, src);
    else if (n > 0)
        osif_log(OSIF_LOG_INFO, "%s: %d missing setting(s) set from the factory defaults",
                strrchr(path, '/') + 1, n);
    else if (n < 0)
        osif_log(OSIF_LOG_ERR, "%s: missing settings not set from the factory defaults",
                strrchr(path, '/') + 1);
}

void mm_factory_boot_seed(void)
{
    char src[256], dst[256];

    for (size_t i = 0; i < sizeof factory_default_files / sizeof factory_default_files[0]; i++) {
        snprintf(src, sizeof src, "%s/%s", FACTORY_DEFAULT_DIR, factory_default_files[i]);
        snprintf(dst, sizeof dst, "%s/%s", USERCONFIG_DIR, factory_default_files[i]);
        int seeded = ini_file_seed(dst, src);
        if (seeded > 0)
            osif_log(OSIF_LOG_INFO, "%s missing — copied from %s", dst, src);
        else if (seeded < 0)
            osif_log(OSIF_LOG_ERR, "%s missing and not copied from the factory defaults",
                    factory_default_files[i]);
    }
}

void mm_factory_boot_fill(void)
{
    if (mm_cfgstore_open(MC_SYSTEMCFG_DEFAULT) != 0) {
        if (mm_cfgstore_recreate(MC_SYSTEMCFG_FACTORY) == 0)
            osif_log(OSIF_LOG_WARN, "%s unreadable — recreated from %s", MC_SYSTEMCFG_DEFAULT,
                    MC_SYSTEMCFG_FACTORY);
        else
            osif_log(OSIF_LOG_ERR, "systemcfg.ini not readable — settings are not loaded or saved");
    }
    factory_fill_store();
    mm_cfg_flush();
    factory_fill_file(CALI_INI_PATH, CALI_INI_FACTORY);
#ifdef CAP_ACTIVATION
    factory_fill_file(MC_ACTIVATION_DEFAULT, MC_ACTIVATION_FACTORY);
#endif /* CAP_ACTIVATION */
}

static void factory_reboot_failed(void)
{
    int reloaded = mm_cfgstore_reload() == 0;

    if (reloaded) {
        factory_fill_store();
        osif_log(OSIF_LOG_ERR,
                "Factory reset: reboot failed; settings reloaded, "
                "the device keeps running");
    } else {
        osif_log(OSIF_LOG_ERR,
                "Factory reset: reboot failed; settings not reloaded "
                "and not saved until the next reboot");
    }
    factory_freeze(0, !reloaded);
    mm_cfgwr_seed(MC_SYSTEMCFG_DEFAULT);
    filtermesh_load();
    mm_led_reload();
    mm_display_start();
}

static int factory_restore_file(const char *src, const char *dst)
{
    char        tmp[256], lock[256], buf[4096];
    struct stat st;
    int         in, out = -1, lfd, rc = -1;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", dst) >= (int)sizeof tmp ||
            snprintf(lock, sizeof lock, "%s.lock", dst) >= (int)sizeof lock)
        return -1;
    in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    if (fstat(in, &st) != 0)
        goto out_in;
    lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lfd < 0)
        goto out_in;
    if (flock(lfd, LOCK_EX) != 0)
        goto out_lock;
    out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0)
        goto out_lock;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto out_tmp;
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                goto out_tmp;
            }
            off += w;
        }
    }
    if (fchown(out, st.st_uid, st.st_gid) != 0 && errno != EPERM)
        goto out_tmp;
    if (fchmod(out, st.st_mode & 07777) != 0)
        goto out_tmp;
    struct timespec times[2] = { st.st_atim, st.st_mtim };
    (void)futimens(out, times);
    if (fsync(out) != 0)
        goto out_tmp;
    rc  = close(out);
    out = -1;
    if (rc == 0)
        rc = rename(tmp, dst);
    if (rc == 0) {
        fsync_parent_dir(dst);
        goto out_lock;
    }
out_tmp:
    rc = -1;
    if (out >= 0)
        close(out);
    unlink(tmp);
out_lock:
    close(lfd);
out_in:
    close(in);
    return rc;
}

static int factory_restore_defaults(void)
{
    char src[256], dst[256];
    int  rc = 0;

    for (size_t i = 0; i < sizeof factory_default_files / sizeof factory_default_files[0]; i++) {
        snprintf(src, sizeof src, "%s/%s", FACTORY_DEFAULT_DIR, factory_default_files[i]);
        snprintf(dst, sizeof dst, "%s/%s", USERCONFIG_DIR, factory_default_files[i]);
        if (factory_restore_file(src, dst) != 0) {
            osif_log(OSIF_LOG_ERR, "Factory reset: could not restore %s", factory_default_files[i]);
            rc = -1;
        }
    }
    return rc;
}

void mm_factory_recover(void)
{
    int restored, reloaded;
    factory_freeze(1, 1);
    (void)mm_cfgstore_reload();
    remove(MM_EFUSE_FILE_PATH);
    mm_efuse_invalidate();
    remove(MC_WEB_TOKEN_KEY);
    (void)mm_cfgwr_wp_clear_dir(MM_CFGWR_WP_DIR, NULL);
    sync();
    restored = factory_restore_defaults() == 0;
    reloaded = mm_cfgstore_reload() == 0;
    if (reloaded)
        factory_fill_store();
    if (!reloaded || filtermesh_keep() != 0)
        osif_log(OSIF_LOG_WARN,
                "Factory reset: filter run time (%u min) not kept; "
                "it is reset to the default",
                filtermesh_minutes());
    if (!restored || !reloaded) {
        osif_log(OSIF_LOG_ERR,
                "Factory reset: restoring defaults failed; "
                "rebooting WITHOUT recording a boot reason");
        if (reloaded && mm_cfgstore_sync() < 0)
            osif_log(OSIF_LOG_ERR, "Factory reset: settings not saved");
        if (mm_system_reboot(0, 0) != 0)
            factory_reboot_failed();
        return;
    }
    mm_tz_apply_default(FACTORY_TIMEZONE);
    if (mm_bootby_stage(MC_BOOTBY_FAC_RESTORE, 0) != 0 || mm_cfgstore_sync() < 0)
        osif_log(OSIF_LOG_ERR, "Factory reset: settings not saved");
    osif_log(OSIF_LOG_OP, "factory reset done");
    if (mm_system_reboot(MC_BOOTBY_FAC_RESTORE, 0) != 0)
        factory_reboot_failed();
}
