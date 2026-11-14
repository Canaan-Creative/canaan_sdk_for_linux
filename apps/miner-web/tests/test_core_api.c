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
#define _DEFAULT_SOURCE
#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core_api.h"
#include "api_sock.h"
#undef CORE_API_TIMEOUT_MS
#undef MC_API_SOCK_PATH
#define CORE_API_TIMEOUT_MS 200
#define MC_API_SOCK_PATH    "test_core_api.sock"
#undef _DEFAULT_SOURCE
#include "core_api.c"

#define ECHO NULL

static long g_stall_ms;

static int listen_peer(void)
{
    struct sockaddr_un a  = { .sun_family = AF_UNIX };
    int                fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    strcpy(a.sun_path, MC_API_SOCK_PATH);
    unlink(MC_API_SOCK_PATH);
    assert(bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    assert(listen(fd, 8) == 0);
    return fd;
}

static pid_t serve_once(int lfd, const char *reply)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        static char req[16384];
        size_t      got = 0;
        int         c   = accept(lfd, NULL, NULL);
        if (c < 0)
            _exit(1);
        while (got == 0 || req[got - 1] != '}') {
            ssize_t r = read(c, req + got, sizeof req - got);
            if (r <= 0)
                _exit(1);
            got += (size_t)r;
        }
        const char *out = reply ? reply : req;
        size_t      n   = reply ? strlen(reply) : got;
        for (size_t off = 0; off < n;) {
            ssize_t w = write(c, out + off, n - off);
            if (w <= 0)
                _exit(1);
            off += (size_t)w;
        }
        if (g_stall_ms) {
            struct timespec ts = { g_stall_ms / 1000, g_stall_ms % 1000 * 1000000L };
            nanosleep(&ts, NULL);
        }
        close(c);
        _exit(0);
    }
    return pid;
}

static int call_served(const char *reply, const char *cmd, const char *param, char *out, size_t cap)
{
    int   lfd = listen_peer();
    pid_t pid = serve_once(lfd, reply);
    int   rc  = core_api_call(cmd, param, out, cap);
    int   st;
    assert(waitpid(pid, &st, 0) == pid);
    close(lfd);
    unlink(MC_API_SOCK_PATH);
    return rc;
}

int main(void)
{
    char out[8192];

    {
        static const char want[] = "{\"command\":\"a\\\"b\",\"parameter\":\"x\\\\y\"}";
        int               rc     = call_served(ECHO, "a\"b", "x\\y", out, sizeof out);
        assert(rc == (int)strlen(want) && !strcmp(out, want));
    }

    {
        static const char want[] = "{\"command\":\"version\",\"parameter\":\"\"}";
        int               rc     = call_served(ECHO, "version", NULL, out, sizeof out);
        assert(rc == (int)strlen(want) && !strcmp(out, want));
    }

    {
        char param[4097];
        char want[4200];
        memset(param, 'p', sizeof param - 1);
        param[sizeof param - 1] = '\0';
        snprintf(want, sizeof want, "{\"command\":\"pools\",\"parameter\":\"%s\"}", param);
        int rc = call_served(ECHO, "pools", param, out, sizeof out);
        assert(rc == (int)strlen(want) && !strcmp(out, want));
    }

    {
        static const char reply[] = "{\"STATUS\":[{\"STATUS\":\"S\",\"Msg\":\"0123456789\"}]}";
        char              small[16];
        int               rc = call_served(reply, "summary", NULL, small, sizeof small);
        assert(rc == CORE_API_TRUNCATED && small[0] == '\0');
    }

    {
        static const char reply[] = "{\"STATUS\":\"S\"}";
        char              exact[sizeof reply];
        int               rc = call_served(reply, "summary", NULL, exact, sizeof exact);
        assert(rc == (int)sizeof reply - 1 && !strcmp(exact, reply));
    }

    {
        static const char reply[] = "{\"STATUS\":[{\"STATUS\":\"S\"";
        g_stall_ms                = 600;
        strcpy(out, "stale");
        int rc     = call_served(reply, "summary", NULL, out, sizeof out);
        g_stall_ms = 0;
        assert(rc == -1 && out[0] == '\0');
    }

    {
        static const char reply[] = "{\"STATUS\":\"S\"}";
        char              exact[sizeof reply];
        g_stall_ms = 600;
        int rc     = call_served(reply, "summary", NULL, exact, sizeof exact);
        g_stall_ms = 0;
        assert(rc == -1 && exact[0] == '\0');
    }

    {
        unlink(MC_API_SOCK_PATH);
        strcpy(out, "stale");
        assert(core_api_call("summary", NULL, out, sizeof out) == -1);
        assert(out[0] == '\0');
    }

    {
        int lfd = listen_peer();
        assert(core_api_call("summary", NULL, out, sizeof out) == -1);
        close(lfd);
        unlink(MC_API_SOCK_PATH);
    }

    printf("core_api: all assertions passed\n");
    return 0;
}
