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

#include "fsync_dir.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define FAKE_FD 1000

static int  fails;
static char opened[PATH_MAX]; /* path passed to the last open call */
static int  open_fail;        /* non-zero makes open fail with ENOENT */
static int  syncs;            /* fsync calls on FAKE_FD */
static int  closes;           /* close calls on FAKE_FD */

int open(const char *path, int flags, ...)
{
    (void)flags;
    snprintf(opened, sizeof opened, "%s", path);
    if (open_fail) {
        errno = ENOENT;
        return -1;
    }
    return FAKE_FD;
}

int fsync(int fd)
{
    if (fd == FAKE_FD)
        syncs++;
    return 0;
}

int close(int fd)
{
    if (fd == FAKE_FD)
        closes++;
    return 0;
}

static void reset(void)
{
    opened[0] = '\0';
    open_fail = 0;
    syncs     = 0;
    closes    = 0;
}

static void expect_parent(const char *path, const char *want)
{
    reset();
    fsync_parent_dir(path);
    if (strcmp(opened, want) != 0 || syncs != 1 || closes != 1) {
        fprintf(stderr, "  %-24s FAIL: opened \"%s\" (want \"%s\"), %d fsync, %d close\n", path,
                opened, want, syncs, closes);
        fails++;
    } else {
        printf("  %-24s ok (\"%s\")\n", path, opened);
    }
}

int main(void)
{
    expect_parent("file", ".");
    expect_parent("/file", "/");
    expect_parent("dir/file", "dir");
    expect_parent("/data/factory/efuse_info.txt", "/data/factory");

    reset();
    fsync_dir("/data/wp");
    if (strcmp(opened, "/data/wp") != 0 || syncs != 1 || closes != 1) {
        fprintf(stderr, "  fsync_dir FAIL: opened \"%s\", %d fsync, %d close\n", opened, syncs,
                closes);
        fails++;
    }

    reset();
    open_fail = 1;
    fsync_parent_dir("dir/file");
    if (syncs != 0 || closes != 0) {
        fprintf(stderr, "  open failure FAIL: %d fsync, %d close\n", syncs, closes);
        fails++;
    }

    if (fails) {
        fprintf(stderr, "test_fsync_dir: %d FAIL\n", fails);
        return 1;
    }
    printf("test_fsync_dir: all pass\n");
    return 0;
}
