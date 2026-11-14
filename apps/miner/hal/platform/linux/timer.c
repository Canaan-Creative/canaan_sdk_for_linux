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

#include "timer.h"
#include "board/board_tables.h"
#include "boardconf.h"
#include "osif.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* tach counter ioctl: set the sampling window, s */
#define TMIOC_SET_TIMEOUT _IOW('T', 0x20, int)

static int timer_fd1[FAN_COUNT]; /* tach counter fd + 1 per fan, 0 when not open */

int timer_init(unsigned int timeout)
{
    int ok = 0;

    for (size_t i = 0; i < board_fan_count; i++) {
        if (board_fans[i].tach_node == NULL)
            continue;
        if (timer_fd1[i] != 0) {
            ok++;
            continue;
        }
        const char *path = board_fans[i].tach_node;
        int         fd   = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            osif_log(OSIF_LOG_ERR, "Fan %d: speed sensor %s unavailable: %s", (int)i, path,
                    strerror(errno));
            continue;
        }
        (void)ioctl(fd, TMIOC_SET_TIMEOUT, &timeout);
        timer_fd1[i] = fd + 1;
        ok++;
    }
    return ok > 0 ? 0 : -1;
}

int get_timer_count(unsigned int *value, int index)
{
    if (index < 0 || (size_t)index >= board_fan_count || timer_fd1[index] == 0)
        return -1;
    return (int)read(timer_fd1[index] - 1, value, sizeof(*value));
}
