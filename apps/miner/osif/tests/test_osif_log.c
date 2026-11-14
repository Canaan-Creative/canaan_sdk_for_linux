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

#define _POSIX_C_SOURCE 200809L
#include "osif.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stdout, "  FAIL: " __VA_ARGS__); \
            fprintf(stdout, "\n"); \
            fails++; \
        } else { \
            printf("  ok: " __VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static int  fails;
static char g_path[256]; /* file the child's stderr is redirected to */
static char g_buf[4096]; /* stderr text captured from the last child, NUL-terminated */

static const char *capture(const char *level)
{
    if (level)
        setenv("MINER_LOG_LEVEL", level, 1);
    else
        unsetenv("MINER_LOG_LEVEL");

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stdout, "  FAIL: fork\n");
        fails++;
        return "";
    }
    if (pid == 0) {
        if (!freopen(g_path, "w+", stderr))
            _exit(1);
        osif_log(OSIF_LOG_ERR, "line err");
        osif_log(OSIF_LOG_WARN, "line warn");
        osif_log(OSIF_LOG_INFO, "line info %d", 42);
        osif_log(OSIF_LOG_DBG, "line dbg");
        fflush(stderr);
        _exit(0);
    }
    (void)waitpid(pid, NULL, 0);

    FILE  *r = fopen(g_path, "r");
    size_t n = r ? fread(g_buf, 1, sizeof g_buf - 1, r) : 0;
    if (r)
        fclose(r);
    g_buf[n] = '\0';
    return g_buf;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int prefix_ok(const char *line)
{
    const char *p     = line;
    const char *shape = "[dddd-dd-dd dd:dd:dd.ddd]";
    for (const char *s = shape; *s; s++, p++) {
        if (*s == 'd') {
            if (!is_digit(*p))
                return -1;
        } else if (*p != *s)
            return -1;
    }
    if (*p++ != ' ' || *p != '[')
        return -1;
    return (int)(p - line);
}

static void check_single_write(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        fprintf(stdout, "  FAIL: socketpair\n");
        fails++;
        return;
    }
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(sv[1], STDERR_FILENO);
    setvbuf(stderr, NULL, _IONBF, 0);
    osif_log(OSIF_LOG_ERR, "atomic line %d", 7);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(sv[1]);

    char    dg[600];
    ssize_t n         = recv(sv[0], dg, sizeof dg - 1, MSG_DONTWAIT);
    dg[n > 0 ? n : 0] = '\0';
    CHECK(n > 0 && dg[n - 1] == '\n' && strstr(dg, "atomic line 7") != NULL,
            "whole line arrives in one datagram (= one write)");
    ssize_t extra = recv(sv[0], dg, sizeof dg, MSG_DONTWAIT);
    CHECK(extra == 0, "no further datagram (line was not split across writes)");
    close(sv[0]);
}

int main(void)
{
    snprintf(g_path, sizeof g_path, "/tmp/test_osif_log.%d", (int)getpid());

    const char *out = capture(NULL);
    CHECK(strstr(out, "line err") != NULL, "default level emits ERR");
    CHECK(strstr(out, "line warn") != NULL, "default level emits WARN");
    CHECK(strstr(out, "line info 42") != NULL, "default level emits INFO (with args)");
    CHECK(strstr(out, "line dbg") == NULL, "default level SUPPRESSES DBG");

    int off = prefix_ok(out);
    CHECK(off > 0, "line is [YYYY-MM-DD HH:MM:SS.mmm] then a [TAG]");
    if (off > 0)
        CHECK(strncmp(out + off, "[ERR] ", 6) == 0, "tag follows the timestamp");

    unsigned int Y, Mo, D, H, Mi, S, mmm = 1000;
    int wall_fields = sscanf(out, "[%u-%u-%u %u:%u:%u.%u]", &Y, &Mo, &D, &H, &Mi, &S, &mmm);
    CHECK(wall_fields == 7, "wall-clock field parses as YYYY-MM-DD HH:MM:SS.mmm");
    CHECK(Mo >= 1 && Mo <= 12 && D >= 1 && D <= 31 && H < 24 && Mi < 60 && S <= 60 && mmm < 1000,
            "wall-clock fields are in range");

    out = capture("DBG");
    CHECK(strstr(out, "line dbg") != NULL, "MINER_LOG_LEVEL=DBG emits DBG");

    out = capture("ERR");
    CHECK(strstr(out, "line err") != NULL, "MINER_LOG_LEVEL=ERR emits ERR");
    CHECK(strstr(out, "line warn") == NULL, "MINER_LOG_LEVEL=ERR suppresses WARN");

    out = capture("warn");
    CHECK(strstr(out, "line warn") != NULL, "level parse is case-insensitive");
    CHECK(strstr(out, "line info") == NULL, "MINER_LOG_LEVEL=warn suppresses INFO");

    out = capture("chatty");
    CHECK(strstr(out, "line info 42") != NULL, "unknown level falls back to INFO");
    CHECK(strstr(out, "line dbg") == NULL, "unknown level is not DBG");

    unsetenv("MINER_LOG_LEVEL");
    if (freopen(g_path, "w+", stderr)) {
        char big[700];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        osif_log(OSIF_LOG_ERR, "%s", big);
        fflush(stderr);
        FILE  *r = fopen(g_path, "r");
        size_t n = r ? fread(g_buf, 1, sizeof g_buf - 1, r) : 0;
        if (r)
            fclose(r);
        g_buf[n] = '\0';
        CHECK(n > 0 && n < 600, "overlong message is truncated (%zu bytes)", n);
        CHECK(n > 0 && g_buf[n - 1] == '\n' && strchr(g_buf, '\n') == g_buf + n - 1,
                "truncated output is still exactly one '\\n'-terminated line");
        CHECK(prefix_ok(g_buf) > 0, "truncated line keeps the wall-clock prefix");
    } else {
        fprintf(stdout, "  FAIL: freopen stderr (trunc)\n");
        fails++;
    }

    check_single_write();

    remove(g_path);
    if (fails) {
        fprintf(stdout, "test_osif_log: %d FAIL\n", fails);
        return 1;
    }
    printf("test_osif_log: all pass\n");
    return 0;
}
