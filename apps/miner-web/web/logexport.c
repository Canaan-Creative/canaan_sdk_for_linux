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

#include "logexport.h"
#include "osif_logpath.h"
#include "sha256.h"
#include "hex.h"
#include "web_logfmt.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define LOGEXPORT_ROT1_MAX 288 /* "<path>.1" buffer, bytes including NUL */

int logexport_body_name(const char *body, char *name, size_t cap)
{
    size_t n = body ? strlen(body) : 0;

    while (n && strchr(" \t\r\n", body[n - 1]))
        n--;
    if (n >= cap)
        return -1;
    if (n)
        memcpy(name, body, n);
    name[n] = '\0';
    return 0;
}

void logexport_safe_name(const char *in, char *out, size_t cap)
{
    size_t n = in ? strlen(in) : 0;
    size_t keep;

    if (!cap)
        return;
    while (n && strchr(" \t\r\n", in[n - 1]))
        n--;
    keep = n < cap ? n : cap > 4 ? cap - 4 : cap - 1;
    for (size_t i = 0; i < keep; i++) {
        unsigned char c = (unsigned char)in[i];

        out[i] = c < 0x20 || c >= 0x7f ? '?' : (char)c;
    }
    if (keep < n && cap > 4) {
        memcpy(out + keep, "...", 3);
        keep += 3;
    }
    out[keep] = '\0';
}

int logexport_lookup(const char *name, char *path, size_t cap)
{
    static const char *const logs[]   = { MINER_LOG_PATH, MINER_OP_LOG_PATH, MINER_WEB_LOG_PATH };
    const char              *log_path = MINER_LOG_PATH;
    int                      dir_len  = (int)(strrchr(log_path, '/') - log_path);

    if (!name || !path || !cap)
        return -1;
    for (size_t i = 0; i < sizeof logs / sizeof logs[0]; i++)
        if (!strcmp(name, strrchr(logs[i], '/') + 1))
            return snprintf(path, cap, "%.*s/%s", dir_len, log_path, name) < (int)cap ? 0 : -1;
    return -1;
}

static int file_state(const char *path)
{
    struct stat st;
    int         fd  = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int         err = errno;
    int         reg;

    if (fd < 0) {
        if (err == ENOENT)
            return 0;
        return lstat(path, &st) == 0 && !S_ISREG(st.st_mode) ? -ELOOP : -err;
    }
    reg = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    close(fd);
    return reg ? 1 : -ELOOP;
}

static int serve_refused(const char *name, const char *path, int state)
{
    char safe[LOGEXPORT_NAME_MAX];
    char safe_path[LOGEXPORT_ROT1_MAX];

    logexport_safe_name(name, safe, sizeof safe);
    logexport_safe_name(path, safe_path, sizeof safe_path);
    if (!state) {
        web_logf(stderr, "WARN", "%s download refused: %s does not exist\n", safe, safe_path);
        return 404;
    }
    if (state == -ELOOP) {
        web_logf(stderr, "WARN", "%s download refused: %s is not a regular file\n", safe,
                safe_path);
        return 404;
    }
    web_logf(stderr, "ERR", "%s download failed: file not readable (%s)\n", safe, strerror(-state));
    return 500;
}

int logexport_serve_check(const char *name, const char *path, int concat)
{
    char rot1[LOGEXPORT_ROT1_MAX];
    int  state = file_state(path);

    if (state <= 0)
        return serve_refused(name, path, state);
    if (concat) {
        snprintf(rot1, sizeof rot1, "%s.1", path);
        state = file_state(rot1);
        if (state < 0)
            return serve_refused(name, rot1, state);
    }
    return 200;
}

void logexport_pass_digest(const char *plaintext, char out_hex[WEBPASS_HEX_LEN + 1])
{
    unsigned char d[SHA256_DIGEST_SIZE];
    sha256(plaintext ? plaintext : "", plaintext ? strlen(plaintext) : 0, d);
    hex_encode(out_hex, d, SHA256_DIGEST_SIZE);
    explicit_bzero(d, sizeof d);
}
