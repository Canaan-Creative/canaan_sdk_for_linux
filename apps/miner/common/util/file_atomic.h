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

#ifndef FILE_ATOMIC_H
#define FILE_ATOMIC_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>
#include "fsync_dir.h"

static inline int file_lock(const char *path)
{
    char lock[512];
    if (snprintf(lock, sizeof lock, "%s.lock", path) >= (int)sizeof lock)
        return -1;
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lfd < 0)
        return -1;
    if (flock(lfd, LOCK_EX) != 0) {
        close(lfd);
        return -1;
    }
    return lfd;
}

static inline int file_write_atomic(const char *path, const char *text, size_t len)
{
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;

    FILE *f = fopen(tmp, "wbe");
    if (!f)
        return -1;
    int ok = fwrite(text, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0)
        ok = 0;
    if (ok && rename(tmp, path) != 0)
        ok = 0;
    if (!ok) {
        unlink(tmp);
        return -1;
    }

    fsync_parent_dir(path);
    return 0;
}

#endif /* FILE_ATOMIC_H */
