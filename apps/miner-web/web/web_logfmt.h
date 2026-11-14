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

#ifndef WEB_LOGFMT_H
#define WEB_LOGFMT_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "osif_logpath.h"

#define WEB_LOG_STAMP_MAX 40   /* "[time] [level] " stamp buffer, bytes */
#define WEB_LOG_OP        "OP" /* operator-event level tag */

struct web_log_file {
    int fd;
    int tried;
};

extern char *program_invocation_short_name; /* process name, without directory */

__attribute__((weak)) struct web_log_file web_log_op_file  = { -1, 0 };
__attribute__((weak)) struct web_log_file web_log_web_file = { -1, 0 };

static inline void web_log_stamp(char out[WEB_LOG_STAMP_MAX], const char *lvl)
{
    struct timespec rt;
    struct tm       tm;
    char            wall[24] = "-";

    clock_gettime(CLOCK_REALTIME, &rt);
    if (localtime_r(&rt.tv_sec, &tm)) {
        size_t wn = strftime(wall, sizeof wall, "%Y-%m-%d %H:%M:%S", &tm);
        snprintf(wall + wn, sizeof wall - wn, ".%03ld", rt.tv_nsec / 1000000L);
    }
    snprintf(out, WEB_LOG_STAMP_MAX, "[%s] [%s] ", wall, lvl);
}

static inline int web_log_rank(const char *lvl)
{
    static const char *const tags[] = { WEB_LOG_OP, "ERR", "WARN", "INFO", "DBG" };
    size_t                   i;

    for (i = 0; i < sizeof tags / sizeof tags[0]; i++)
        if (!strcasecmp(lvl, tags[i]))
            return (int)i;
    return -1;
}

static inline int web_log_threshold(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *e = getenv("MINER_LOG_LEVEL");
        int         r = e ? web_log_rank(e) : -1;

        cached = r > web_log_rank(WEB_LOG_OP) ? r : web_log_rank("INFO");
    }
    return cached;
}

static inline int web_log_file_fd(struct web_log_file *lf, const char *path, const char *what)
{
    if (lf->tried)
        return lf->fd;
    lf->tried = 1;
    lf->fd    = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (lf->fd < 0) {
        char stamp[WEB_LOG_STAMP_MAX];
        web_log_stamp(stamp, "ERR");
        fprintf(stderr,
                "%s%s: cannot open the log file for %s (%s); they "
                "go to the console only until restart\n",
                stamp, program_invocation_short_name, what, strerror(errno));
    }
    return lf->fd;
}

static inline int web_log_op_fd(void)
{
    return web_log_file_fd(&web_log_op_file, MINER_OP_LOG_PATH, "operator events");
}

static inline int web_log_web_fd(void)
{
    return web_log_file_fd(&web_log_web_file, MINER_WEB_LOG_PATH, "web messages");
}

static inline void web_log_append(int fd, const char *line, size_t len)
{
    if (fd >= 0) {
        ssize_t w = write(fd, line, len);
        (void)w;
    }
}

__attribute__((format(printf, 4, 0))) static inline void web_vlogf(FILE *sink, int to_web,
        const char *lvl, const char *fmt, va_list ap)
{
    char   line[512];
    size_t len;
    int    m;

    if (web_log_rank(lvl) > web_log_threshold())
        return;
    web_log_stamp(line, lvl);
    len = strlen(line);
    snprintf(line + len, sizeof line - len, "%s: ", program_invocation_short_name);
    len = strlen(line);
    m   = vsnprintf(line + len, sizeof line - len, fmt, ap);
    len += m > 0 ? (size_t)m : 0;
    if (len > sizeof line - 1) {
        len           = sizeof line - 1;
        line[len - 1] = '\n';
    }
    fputs(line, sink);
    if (!strcmp(lvl, WEB_LOG_OP))
        web_log_append(web_log_op_fd(), line, len);
    if (to_web)
        web_log_append(web_log_web_fd(), line, len);
}

__attribute__((format(printf, 3, 4))) static inline void web_logf(FILE *sink, const char *lvl,
        const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    web_vlogf(sink, 1, lvl, fmt, ap);
    va_end(ap);
}

__attribute__((format(printf, 3, 4))) static inline void web_logf_noweb(FILE *sink, const char *lvl,
        const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    web_vlogf(sink, 0, lvl, fmt, ap);
    va_end(ap);
}

#endif /* WEB_LOGFMT_H */
