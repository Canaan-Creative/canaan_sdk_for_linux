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

#include "mm_tz.h"
#include "mm_cfgstore.h"
#include "osif.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MM_TZ_PATH_MAX 256 /* zone file path buffer, bytes, including NUL */

static int tz_name_ok(const char *name)
{
    if (!name || !*name || *name == '/')
        return 0;
    size_t n = strlen(name);
    if (n >= MM_TZ_NAME_CAP || name[n - 1] == '/')
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '/') {
            if (name[i + 1] == '/')
                return 0;
            continue;
        }
        if (!isalnum((unsigned char)c) && c != '_' && c != '+' && c != '-')
            return 0;
    }
    return 1;
}

static int mm_tz_install(const char *name)
{
    char zi[MM_TZ_PATH_MAX];

    if (!tz_name_ok(name))
        return -1;

    if (snprintf(zi, sizeof zi, "%s/%s", MM_TZ_ZONEINFO_DIR, name) >= (int)sizeof zi)
        return -1;
    struct stat st;
    if (stat(zi, &st) != 0 || !S_ISREG(st.st_mode))
        return -2;

    static const char tmp[] = MM_TZ_LOCALTIME_PATH ".new";
    unlink(tmp);
    if (symlink(zi, tmp) != 0)
        return -3;
    if (rename(tmp, MM_TZ_LOCALTIME_PATH) != 0) {
        unlink(tmp);
        return -3;
    }

    unsetenv("TZ");
    tzset();
    return 0;
}

static const char *tz_why(int rc)
{
    /* clang-format off */
    return rc == -1 ? "illegal zone name" :
            rc == -2 ? "no such zone in the image" :
            "localtime relink failed";
    /* clang-format on */
}

static int tz_persist(const char *name, int gated)
{
    if (gated)
        return mm_cfgstore_set("syscfg", "timezone", name);
    return mm_cfgstore_put("syscfg", "timezone", name);
}

static mc_set_result tz_apply(const char *args, int gated)
{
    if (!args || !*args)
        return MC_SET_ERR_ARG;
    int rc = mm_tz_install(args);
    if (rc != 0) {
        osif_log(OSIF_LOG_WARN, "settz %s: \"%s\" — timezone unchanged", tz_why(rc), args);
        return MC_SET_ERR_ARG;
    }
    if (tz_persist(args, gated) != 0 && !(gated && mm_cfgstore_frozen()))
        osif_log(OSIF_LOG_OP, "settz %s applied but NOT persisted", args);
    osif_log(OSIF_LOG_INFO, "timezone -> %s (%s)", args, tzname[0]);
    return MC_SET_OK;
}

mc_set_result mm_apply_settz(const char *args)
{
    return tz_apply(args, 1);
}

mc_set_result mm_tz_apply_default(const char *name)
{
    return tz_apply(name, 0);
}

void mm_tz_restore(void)
{
    char name[MM_TZ_NAME_CAP];
    if (mm_cfgstore_get("syscfg", "timezone", name, sizeof name) != 0 || !name[0])
        return;
    int rc = mm_tz_install(name);
    if (rc == 0)
        osif_log(OSIF_LOG_INFO, "timezone restored -> %s (%s)", name, tzname[0]);
    else
        osif_log(OSIF_LOG_WARN,
                "timezone %s: \"%s\" — staying on the "
                "image default",
                tz_why(rc), name);
}

void mm_tz_get(char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (mm_cfgstore_get("syscfg", "timezone", out, cap) != 0)
        out[0] = 0;
}
