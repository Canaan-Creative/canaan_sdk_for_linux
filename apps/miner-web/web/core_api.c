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

#define _DEFAULT_SOURCE

#include "core_api.h"
#include "api_sock.h"
#include "jwrite.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

static int core_api_open(void)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    struct timeval     to = { .tv_sec = CORE_API_TIMEOUT_MS / 1000,
            .tv_usec                  = (CORE_API_TIMEOUT_MS % 1000) * 1000 };
    int                fd;

    if (strlen(MC_API_SOCK_PATH) >= sizeof sa.sun_path)
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    strcpy(sa.sun_path, MC_API_SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t w = send(fd, buf, len, MSG_NOSIGNAL);
        if (w <= 0)
            return -1;
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}

int core_api_call(const char *cmd, const char *param, char *out, size_t cap)
{
    if (!cmd || !out || cap < 2)
        return -1;
    if (!param)
        param = "";

    size_t  rcap = 32 + 6 * (strlen(cmd) + strlen(param));
    char   *req  = malloc(rcap);
    jwriter w;
    if (!req)
        return -1;
    jw_init(&w, req, rcap);
    jw_obj_open(&w);
    jw_kv_str(&w, "command", cmd);
    jw_kv_str(&w, "parameter", param);
    jw_obj_close(&w);

    int     fd  = jw_ok(&w) ? core_api_open() : -1;
    size_t  got = 0;
    ssize_t r   = -1;
    if (fd >= 0 && send_all(fd, req, jw_len(&w)) == 0) {
        char c;
        while (got + 1 < cap && (r = read(fd, out + got, cap - 1 - got)) > 0)
            got += (size_t)r;
        if (got + 1 == cap)
            r = read(fd, &c, 1);
    }
    if (fd >= 0)
        close(fd);
    explicit_bzero(req, rcap);
    free(req);
    out[r == 0 ? got : 0] = '\0';
    if (r > 0)
        return CORE_API_TRUNCATED;
    return r == 0 && got ? (int)got : -1;
}
