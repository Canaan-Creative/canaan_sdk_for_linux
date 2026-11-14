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
#include "beep_dev.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "boardconf.h"
#include "input_bits.h"
#include "osif.h"

#define INPUT_DIR  "/dev/input"
#define SND_VOLUME (SND_TONE + 1) /* driver-specific EV_SND code setting the beeper volume */

static int g_fd = -1; /* beeper evdev node fd, -1 when closed */

static int node_is_beeper(int fd)
{
    char          name[64]            = { 0 };
    unsigned long snd[NBITS(SND_CNT)] = { 0 };

    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0 && strcasestr(name, "beeper"))
        return 1;
    if (ioctl(fd, EVIOCGBIT(EV_SND, sizeof snd), snd) >= 0 && BIT_SET(snd, SND_BELL))
        return 1;
    return 0;
}

int beep_init(void)
{
    DIR           *d;
    struct dirent *e;
    char           path[512];

    if (!CAP_BEEP)
        return -1;

    if (g_fd >= 0)
        return 0;

    d = opendir(INPUT_DIR);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof path, INPUT_DIR "/%s", e->d_name);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (node_is_beeper(fd)) {
            g_fd = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    if (g_fd < 0) {
        osif_log(OSIF_LOG_WARN, "Buzzer not found; sounds disabled");
        return -1;
    }
    return 0;
}

static void beep_write(unsigned short code, int value)
{
    struct input_event ev;

    if (g_fd < 0)
        return;
    memset(&ev, 0, sizeof ev);
    ev.type  = EV_SND;
    ev.code  = code;
    ev.value = value;
    if (write(g_fd, &ev, sizeof ev) != sizeof ev)
        osif_log(OSIF_LOG_WARN, "Buzzer: sound command could not be sent");
}

void beep_play(int en)
{
    beep_write(SND_BELL, en ? 1 : 0);
}

void beep_volume(int pct)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    beep_write(SND_VOLUME, pct / 2);
}

void beep_close(void)
{
    if (g_fd >= 0) {
        beep_play(0);
        close(g_fd);
        g_fd = -1;
    }
}
