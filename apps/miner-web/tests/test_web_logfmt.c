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
#include "osif_logpath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static char g_op_path[]  = "/tmp/web_logfmt_op_XXXXXX";
static char g_web_path[] = "/tmp/web_logfmt_web_XXXXXX";

#undef MINER_OP_LOG_PATH
#undef MINER_WEB_LOG_PATH
#define MINER_OP_LOG_PATH  g_op_path
#define MINER_WEB_LOG_PATH g_web_path
#include "web_logfmt.h"

static int  fails;
static char g_out_path[] = "/tmp/web_logfmt_out_XXXXXX";
static char g_out[4096];
static char g_op[4096];
static char g_web[4096];

static void slurp(const char *path, char *buf, size_t cap)
{
    FILE  *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;

    if (f)
        fclose(f);
    buf[n] = '\0';
}

static void capture(const char *level)
{
    pid_t pid;

    if (level)
        setenv("MINER_LOG_LEVEL", level, 1);
    else
        unsetenv("MINER_LOG_LEVEL");
    CHECK(truncate(g_op_path, 0) == 0, "operator log truncated");
    CHECK(truncate(g_web_path, 0) == 0, "web log truncated");
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "  FAIL: fork\n");
        fails++;
        return;
    }
    if (pid == 0) {
        FILE *f = fopen(g_out_path, "w");

        if (!f)
            _exit(1);
        web_logf(f, WEB_LOG_OP, "line op\n");
        web_logf(f, "ERR", "line err\n");
        web_logf(f, "WARN", "line warn\n");
        web_logf(f, "INFO", "line info\n");
        web_logf(f, "DBG", "line dbg\n");
        web_logf(f, "XYZ", "line unknown\n");
        web_logf_noweb(f, "ERR", "line noweb\n");
        setenv("MINER_LOG_LEVEL", "DBG", 1);
        web_logf(f, "DBG", "line late dbg\n");
        fclose(f);
        _exit(0);
    }
    (void)waitpid(pid, NULL, 0);
    slurp(g_out_path, g_out, sizeof g_out);
    slurp(g_op_path, g_op, sizeof g_op);
    slurp(g_web_path, g_web, sizeof g_web);
}

static void check_default(const char *level, const char *what)
{
    char msg[128];

    capture(level);
    snprintf(msg, sizeof msg, "%s: INFO printed", what);
    CHECK(strstr(g_out, "line info"), msg);
    snprintf(msg, sizeof msg, "%s: WARN printed", what);
    CHECK(strstr(g_out, "line warn"), msg);
    snprintf(msg, sizeof msg, "%s: DBG dropped", what);
    CHECK(!strstr(g_out, "line dbg"), msg);
}

int main(void)
{
    int fd = mkstemp(g_op_path);

    if (fd < 0)
        return 1;
    close(fd);
    fd = mkstemp(g_web_path);
    if (fd < 0)
        return 1;
    close(fd);
    fd = mkstemp(g_out_path);
    if (fd < 0)
        return 1;
    close(fd);

    check_default(NULL, "unset");
    CHECK(strstr(g_out, "[OP] ") && strstr(g_out, "line op"), "unset: OP printed");
    CHECK(strstr(g_out, "line err"), "unset: ERR printed");
    CHECK(strstr(g_out, "[XYZ] ") && strstr(g_out, "line unknown"), "unset: unknown tag printed");
    CHECK(!strstr(g_out, "line late dbg"), "level is read once and cached");
    CHECK(strstr(g_op, "line op") && !strstr(g_op, "line err"),
            "unset: only OP goes to the operator log");
    CHECK(strstr(g_web, "[OP] ") && strstr(g_web, "line op") && strstr(g_web, "[ERR] ") &&
                    strstr(g_web, "line err") && strstr(g_web, "line warn") &&
                    strstr(g_web, "line info") && strstr(g_web, "line unknown"),
            "unset: every printed line also goes to the web log");
    CHECK(!strstr(g_web, "line dbg"), "unset: DBG below the level stays out of the web log");
    CHECK(strstr(g_out, "line noweb") && !strstr(g_web, "line noweb") &&
                    !strstr(g_op, "line noweb"),
            "web_logf_noweb prints to the sink only");
    CHECK(strlen(g_web) > 0 && !strncmp(g_out, g_web, strlen(g_web)),
            "unset: the web log lines are byte-identical to the sink lines");

    check_default("", "empty");
    check_default("INFO", "INFO");
    check_default("OP", "OP (not accepted)");
    check_default("3", "number (not accepted)");
    check_default("bogus", "invalid");

    capture("DBG");
    CHECK(strstr(g_out, "line dbg"), "DBG: DBG printed");

    capture("err");
    CHECK(strstr(g_out, "line err"), "err: ERR printed");
    CHECK(!strstr(g_out, "line warn"), "err: WARN dropped");
    CHECK(!strstr(g_out, "line info"), "err: INFO dropped");
    CHECK(strstr(g_out, "line op"), "err: OP printed");
    CHECK(strstr(g_out, "line unknown"), "err: unknown tag printed");
    CHECK(strstr(g_op, "line op"), "err: OP goes to the operator log");
    CHECK(strstr(g_web, "line err") && strstr(g_web, "line op") && !strstr(g_web, "line warn") &&
                    !strstr(g_web, "line info"),
            "err: the web log holds the same lines as the sink");

    capture("Warn");
    CHECK(strstr(g_out, "line warn"), "Warn: WARN printed");
    CHECK(!strstr(g_out, "line info"), "Warn: INFO dropped");
    CHECK(strstr(g_out, "line op"), "Warn: OP printed");

    unlink(g_op_path);
    unlink(g_web_path);
    unlink(g_out_path);
    return fails ? 1 : 0;
}
