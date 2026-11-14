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

#define _GNU_SOURCE
#include "mc_api_time.h"
#include <stdio.h>
#include <string.h>

#include <sys/time.h>
#include "osif.h"

#define TIME_EPOCH_MIN 1577836800LL /* 2020-01-01 00:00:00 UTC, s */
#define TIME_EPOCH_MAX 4102444800LL /* 2100-01-01 00:00:00 UTC, s */

#define TIME_CODE_MISSING 130 /* Code: time parameter missing */
#define TIME_CODE_INFO    131 /* Code: time info */
#define TIME_TZ_MAX       64  /* timezone name length, max bytes */
#define TIME_CMD_MAX      16  /* subcommand length, max bytes */

_Static_assert(TIME_TZ_MAX == 64, "TIME_TZ_MAX must match the %64s width in mc_api_time_run");
_Static_assert(TIME_CMD_MAX == 16, "TIME_CMD_MAX must match the %16[^,] widths in this file");

static mc_set_result (*s_tz_fwd)(const char *tzname);
static int (*s_tz_read)(char *out, size_t cap);

void mc_api_time_set_forwarder(mc_set_result (*fn)(const char *tzname))
{
    s_tz_fwd = fn;
}

void mc_api_time_set_tz_reader(int (*fn)(char *out, size_t cap))
{
    s_tz_read = fn;
}

static int time_payload(const char *req, char *out, size_t cap)
{
    if (!req || !out || cap == 0)
        return -1;
    int         is_json = 0;
    const char *v       = NULL;
    const char *j       = strstr(req, "\"parameter\"");
    if (j) {
        const char *c = strchr(j, ':');
        if (!c)
            return -1;
        const char *q = strchr(c, '"');
        if (!q)
            return -1;
        is_json = 1;
        v       = q + 1;
    } else {
        const char *bar = strchr(req, '|');
        if (!bar)
            return -1;
        v = bar + 1;
    }
    size_t n = 0;
    while (*v && n + 1 < cap) {
        if (is_json && *v == '"')
            break;
        if (!is_json && *v == '|')
            break;
        if ((unsigned char)*v < 0x20 || (unsigned char)*v == 0x7f)
            break;
        out[n++] = *v++;
    }
    out[n] = 0;
    return n ? 0 : -1;
}

static int time_apply(long long epoch)
{
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    return settimeofday(&tv, NULL);
}

static mc_set_result time_info(char *msg, size_t msgcap, int *code, const char *info)
{
    *code = TIME_CODE_INFO;
    if (msgcap) {
        snprintf(msg, msgcap, "Time set info: %s", info);
        mc_api_msg_safe(msg, msgcap);
    }
    return MC_SET_INFO;
}

static mc_set_result time_err(mc_set_result r, char *msg, size_t msgcap, int *code,
        const char *reason)
{
    *code = mc_set_code(r);
    if (msgcap)
        snprintf(msg, msgcap, "Time set failed: %s", reason);
    return r;
}

int mc_api_time_writes(const char *req)
{
    char pay[128], cmd[TIME_CMD_MAX + 1] = { 0 };

    if (time_payload(req, pay, sizeof pay) != 0)
        return 0;
    return sscanf(pay, "%16[^,]", cmd) == 1 && !strcmp(cmd, "set");
}

mc_set_result mc_api_time_run(const char *req, char *msg, size_t msgcap, int *code)
{
    if (msgcap)
        msg[0] = 0;

    char pay[128];
    if (time_payload(req, pay, sizeof pay) != 0) {
        *code = TIME_CODE_MISSING;
        if (msgcap)
            snprintf(msg, msgcap, "Time parameter is not available");
        return MC_SET_INFO;
    }

    char      cmd[TIME_CMD_MAX + 1] = { 0 }, tz[TIME_TZ_MAX + 1] = { 0 }, info[96];
    long long epoch = 0;
    int       ret   = sscanf(pay, "%16[^,],t:%lld,%64s", cmd, &epoch, tz);
    if (ret == 1 && !strcmp(cmd, "get")) {
        if (s_tz_read && s_tz_read(tz, sizeof tz) != 0)
            tz[0] = 0;
        snprintf(info, sizeof info, "time t:%s", tz);
        return time_info(msg, msgcap, code, info);
    }

    if (ret != 3 || strcmp(cmd, "set"))
        return time_info(msg, msgcap, code, "time unknown argument");

    if (epoch < TIME_EPOCH_MIN || epoch >= TIME_EPOCH_MAX)
        return time_err(MC_SET_ERR_ARG, msg, msgcap, code, "time out of range");

    mc_set_result r = s_tz_fwd ? s_tz_fwd(tz) : MC_SET_OK;
    if (r == MC_SET_ERR_ARG)
        return time_err(r, msg, msgcap, code, "invalid timezone");
    if (r != MC_SET_OK)
        return time_err(MC_SET_ERR_HW, msg, msgcap, code, "timezone not confirmed");
    if (time_apply(epoch) != 0)
        return time_err(MC_SET_ERR_HW, msg, msgcap, code, "system clock not set");
    osif_log(OSIF_LOG_OP, "time set epoch=%lld tz=%s", epoch, tz);

    snprintf(info, sizeof info, "time t:%lld %s", epoch, tz);
    return time_info(msg, msgcap, code, info);
}
