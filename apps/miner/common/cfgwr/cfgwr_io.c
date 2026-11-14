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
#include "cfgwr_io.h"

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int cfgwr_read_full(int fd, void *buf, size_t len)
{
    uint8_t *bytes = buf;
    size_t   off   = 0;

    while (off < len) {
        ssize_t received = read(fd, bytes + off, len - off);
        if (received > 0) {
            off += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        if (received == 0)
            errno = 0;
        return -1;
    }
    return 0;
}

int cfgwr_write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *bytes = buf;
    size_t         off   = 0;

    while (off < len) {
        ssize_t sent = send(fd, bytes + off, len - off, MSG_NOSIGNAL);
        if (sent > 0) {
            off += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}
