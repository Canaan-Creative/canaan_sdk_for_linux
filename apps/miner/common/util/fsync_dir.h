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

#ifndef FSYNC_DIR_H
#define FSYNC_DIR_H

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static inline void fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

static inline void fsync_parent_dir(const char *path)
{
    char dir[PATH_MAX];

    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir)
        return;
    char *slash = strrchr(dir, '/');
    if (!slash)
        strcpy(dir, ".");
    else if (slash == dir)
        slash[1] = '\0';
    else
        *slash = '\0';
    fsync_dir(dir);
}

#endif /* FSYNC_DIR_H */
