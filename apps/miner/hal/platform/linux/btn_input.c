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
#include "btn_input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "boardconf.h"
#include "input_bits.h"
#include "osif.h"

#define BTN_INPUT_KEY_TP1   105 /* TP1 key code (KEY_LEFT) */
#define BTN_INPUT_KEY_TP2   103 /* TP2 key code (KEY_UP) */
#define BTN_INPUT_KEY_POWER 116 /* power key code (KEY_POWER) */

#define INPUT_DIR     "/dev/input"
#define BTN_MAX_DEV   16   /* most input devices watched at once */
#define BTN_RESCAN_MS 2000 /* idle time before rescanning INPUT_DIR, ms */

/* key codes reported */
static const int g_tracked[] = { BTN_INPUT_KEY_TP1, BTN_INPUT_KEY_TP2, BTN_INPUT_KEY_POWER };

static struct {
    btn_input_cb   cb;
    void          *arg;
    int            epfd;   /* epoll fd, -1 when stopped */
    int            stopfd; /* eventfd that stops the reader, -1 when stopped */
    osif_thread_t *thr;    /* reader thread, NULL when stopped */
    int            fds[BTN_MAX_DEV];
    char           paths[BTN_MAX_DEV][512]; /* node path of each fds entry */
    int            ndev;                    /* entries used in fds and paths */
} g = { .epfd = -1, .stopfd = -1 };

static int is_tracked(int code)
{
    for (size_t i = 0; i < sizeof g_tracked / sizeof g_tracked[0]; i++)
        if (g_tracked[i] == code)
            return 1;
    return 0;
}

static int already_open(const char *path)
{
    for (int i = 0; i < g.ndev; i++)
        if (strcmp(g.paths[i], path) == 0)
            return 1;
    return 0;
}

static int node_qualifies(int fd, const char *name)
{
    if (name && (strcasestr(name, "gpio-keys") || strcasestr(name, "gpio_keys")))
        return 1;

    unsigned long keybits[NBITS(KEY_MAX + 1)];
    memset(keybits, 0, sizeof keybits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
        return 0;
    for (size_t i = 0; i < sizeof g_tracked / sizeof g_tracked[0]; i++)
        if (BIT_SET(keybits, (unsigned)g_tracked[i]))
            return 1;
    return 0;
}

static void add_device(const char *path, int fd)
{
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    if (epoll_ctl(g.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        return;
    }
    g.fds[g.ndev] = fd;
    snprintf(g.paths[g.ndev], sizeof g.paths[0], "%s", path);
    g.ndev++;
}

static void drop_device(int idx)
{
    epoll_ctl(g.epfd, EPOLL_CTL_DEL, g.fds[idx], NULL);
    close(g.fds[idx]);
    osif_log(OSIF_LOG_WARN, "Buttons: input device %s disappeared", g.paths[idx]);
    g.ndev--;
    g.fds[idx] = g.fds[g.ndev];
    memcpy(g.paths[idx], g.paths[g.ndev], sizeof g.paths[0]);
}

static void scan_devices(void)
{
    DIR *d = opendir(INPUT_DIR);
    if (!d)
        return;
    struct dirent *e;
    while (g.ndev < BTN_MAX_DEV && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[512];
        snprintf(path, sizeof path, INPUT_DIR "/%s", e->d_name);
        if (already_open(path))
            continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        char name[128] = { 0 };
        (void)ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        if (!node_qualifies(fd, name)) {
            close(fd);
            continue;
        }
        add_device(path, fd);
        osif_log(OSIF_LOG_DBG, "Buttons: using input device %s (\"%s\")", path, name);
    }
    closedir(d);
}

static int drain_fd(int fd)
{
    struct input_event ie;
    ssize_t            n;
    while ((n = read(fd, &ie, sizeof ie)) == (ssize_t)sizeof ie) {
        if (ie.type == EV_KEY && (ie.value == 0 || ie.value == 1) && is_tracked(ie.code))
            g.cb(ie.code, ie.value, (uint32_t)osif_now_ms(), g.arg);
    }
    if (n == 0)
        return 0;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return 0;
    return 1;
}

static void *reader_thread(void *arg)
{
    (void)arg;
    scan_devices();
    for (;;) {
        struct epoll_event evs[BTN_MAX_DEV + 1];
        int                n = epoll_wait(g.epfd, evs, BTN_MAX_DEV + 1, BTN_RESCAN_MS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0) {
            scan_devices();
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (evs[i].data.fd == g.stopfd)
                return NULL;
            if (!drain_fd(evs[i].data.fd)) {
                for (int j = 0; j < g.ndev; j++)
                    if (g.fds[j] == evs[i].data.fd) {
                        drop_device(j);
                        break;
                    }
            }
        }
    }
    return NULL;
}

int btn_input_start(btn_input_cb cb, void *arg)
{
    if (!CAP_BUTTON)
        return -1;

    if (!cb || g.thr)
        return -1;

    g.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g.epfd < 0)
        return -1;

    g.stopfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g.stopfd < 0) {
        close(g.epfd);
        g.epfd = -1;
        return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = g.stopfd };
    if (epoll_ctl(g.epfd, EPOLL_CTL_ADD, g.stopfd, &ev) < 0)
        goto fail;

    g.cb   = cb;
    g.arg  = arg;
    g.ndev = 0;
    if (osif_thread_create(&g.thr, "btn_input", reader_thread, NULL, 0) != OSIF_OK) {
        g.thr = NULL;
        goto fail;
    }
    return 0;

fail:
    close(g.stopfd);
    g.stopfd = -1;
    close(g.epfd);
    g.epfd = -1;
    return -1;
}

void btn_input_stop(void)
{
    if (!g.thr)
        return;
    uint64_t one = 1;
    (void)!write(g.stopfd, &one, sizeof one);
    osif_thread_join(g.thr, NULL);
    g.thr = NULL;

    for (int i = 0; i < g.ndev; i++)
        close(g.fds[i]);
    g.ndev = 0;
    close(g.stopfd);
    g.stopfd = -1;
    close(g.epfd);
    g.epfd = -1;
}
