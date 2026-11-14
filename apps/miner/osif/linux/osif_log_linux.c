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
#include "osif.h"
#include "osif_logpath.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static int g_op_fd = -1; /* operator event log fd; -1 = not open */

static const char *lvl_tag(osif_loglvl_t l)
{
    switch (l) {
    case OSIF_LOG_OP:
        return "OP";
    case OSIF_LOG_ERR:
        return "ERR";
    case OSIF_LOG_WARN:
        return "WARN";
    case OSIF_LOG_INFO:
        return "INFO";
    default:
        return "DBG";
    }
}

static osif_loglvl_t log_threshold(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MINER_LOG_LEVEL");
        if (!e || !*e)
            cached = OSIF_LOG_INFO;
        else if (!strcasecmp(e, "ERR"))
            cached = OSIF_LOG_ERR;
        else if (!strcasecmp(e, "WARN"))
            cached = OSIF_LOG_WARN;
        else if (!strcasecmp(e, "DBG"))
            cached = OSIF_LOG_DBG;
        else
            cached = OSIF_LOG_INFO;
    }
    return (osif_loglvl_t)cached;
}

int osif_log_op_sink_open(void)
{
    static int tried;

    if (tried)
        return g_op_fd;
    tried   = 1;
    g_op_fd = open(MINER_OP_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (g_op_fd < 0)
        osif_log(OSIF_LOG_ERR,
                "Cannot open the operator log (%s); operator events go to the main log only",
                strerror(errno));
    return g_op_fd;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    if (lvl > log_threshold())
        return;

    struct timespec rt;
    struct tm       tm;
    char            wall[24];
    clock_gettime(CLOCK_REALTIME, &rt);
    size_t wn = strftime(wall, sizeof wall, "%Y-%m-%d %H:%M:%S", localtime_r(&rt.tv_sec, &tm));
    snprintf(wall + wn, sizeof wall - wn, ".%03ld", rt.tv_nsec / 1000000L);

    char line[512];
    int  n = snprintf(line, sizeof line, "[%s] [%s] %s: ", wall, lvl_tag(lvl),
             program_invocation_short_name);
    if (n < 0)
        n = 0;
    else if ((size_t)n >= sizeof line)
        n = sizeof line - 1;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
    if (len > sizeof line - 1)
        len = sizeof line - 1;
    line[len] = '\n';
    fwrite(line, 1, len + 1, stderr);
    if (lvl == OSIF_LOG_OP && g_op_fd >= 0) {
        ssize_t w = write(g_op_fd, line, len + 1);
        (void)w;
    }
}
