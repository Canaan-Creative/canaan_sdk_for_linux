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

#include "webstatus.h"
#include "jwrite.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

size_t web_status_json(const char *st, int code, const char *msg, char *out, size_t cap)
{
    char    obj[256], num[24];
    jwriter w;

    if (!cap)
        return 0;
    jw_init(&w, obj, sizeof obj);
    jw_obj_open(&w);
    jw_kv_str(&w, "STATUS", st);
    snprintf(num, sizeof num, "%lld", (long long)time(NULL));
    jw_kv_raw(&w, "When", num);
    snprintf(num, sizeof num, "%d", code);
    jw_kv_raw(&w, "Code", num);
    jw_kv_str(&w, "Msg", msg);
    jw_obj_close(&w);
    snprintf(out, cap, "{\"STATUS\":[%s],\"id\":1}", obj);
    return strlen(out);
}

size_t web_status_bad_cmd(const char *name, char *out, size_t cap)
{
    char   msg[sizeof WEB_STATUS_BAD_CMD_MSG + 1 + WEB_STATUS_BAD_CMD_NAME];
    size_t n = sizeof WEB_STATUS_BAD_CMD_MSG - 1;

    memcpy(msg, WEB_STATUS_BAD_CMD_MSG, n);
    msg[n++] = ' ';
    for (; name && *name && n < sizeof msg - 1; name++)
        if (*name >= ' ' && *name <= '~')
            msg[n++] = *name;
    msg[n] = '\0';
    return web_status_json("E", WEB_STATUS_BAD_CMD_CODE, msg, out, cap);
}
