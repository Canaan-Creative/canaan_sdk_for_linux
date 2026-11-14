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
#include "osif.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* AF_UNIX SOCK_SEQPACKET IPC channel */
struct osif_ipc {
    int              is_server;
    int              listen_fd; /* listening socket, -1 for a client */
    volatile int     conn_fd;   /* connected socket, -1 if none */
    char             path[108];
    osif_ipc_recv_cb cb;
    pthread_t        rx;
    volatile int     running;
    _Atomic unsigned conn_seq;  /* connections established, count */
    pthread_mutex_t  send_lock; /* serializes sends and conn_fd changes */
};

static int make_seqpacket(void)
{
    return socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
}

static void set_addr(struct osif_ipc *g, struct sockaddr_un *a)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path, g->path, strnlen(g->path, sizeof a->sun_path - 1));
}

static int connect_client(const struct sockaddr_un *a)
{
    int c = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int fl;

    if (c < 0)
        return -1;
    if (connect(c, (const struct sockaddr *)a, sizeof *a) != 0 || (fl = fcntl(c, F_GETFL)) < 0 ||
            fcntl(c, F_SETFL, fl & ~O_NONBLOCK) != 0) {
        close(c);
        return -1;
    }
    return c;
}

static void deliver(struct osif_ipc *g, const uint8_t *buf, int n)
{
    if (n < (int)sizeof(uint32_t) || !g->cb)
        return;
    uint32_t type;
    memcpy(&type, buf, sizeof type);
    g->cb(type, buf + sizeof type, n - (int)sizeof type);
}

static int serve_conn(struct osif_ipc *g, int fd)
{
    uint8_t buf[sizeof(uint32_t) + OSIF_IPC_MAX];
    int     n;

    pthread_mutex_lock(&g->send_lock);
    if (!g->running) {
        pthread_mutex_unlock(&g->send_lock);
        close(fd);
        return -1;
    }
    g->conn_fd = fd;
    atomic_fetch_add(&g->conn_seq, 1);
    pthread_mutex_unlock(&g->send_lock);

    while ((n = (int)recv(fd, buf, sizeof buf, 0)) > 0)
        deliver(g, buf, n);

    pthread_mutex_lock(&g->send_lock);
    g->conn_fd = -1;
    close(fd);
    pthread_mutex_unlock(&g->send_lock);
    return 0;
}

static void *rx_thread(void *arg)
{
    struct osif_ipc   *g = arg;
    struct sockaddr_un a;

    set_addr(g, &a);
    while (g->running) {
        int c;

        if (g->is_server) {
            c = accept4(g->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        } else {
            c = connect_client(&a);
        }
        if (c < 0) {
            if (!g->running)
                break;
            osif_sleep_ms(50);
            continue;
        }
        if (serve_conn(g, c) != 0)
            break;
    }
    return NULL;
}

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    if (!h || !path)
        return OSIF_ERR;
    struct osif_ipc *g = calloc(1, sizeof *g);
    if (!g)
        return OSIF_ERR;
    g->cb        = cb;
    g->is_server = is_server;
    g->listen_fd = g->conn_fd = -1;
    pthread_mutex_init(&g->send_lock, NULL);
    strncpy(g->path, path, sizeof g->path - 1);

    if (is_server) {
        g->listen_fd = make_seqpacket();
        if (g->listen_fd < 0)
            goto fail;
        unlink(g->path);
        struct sockaddr_un a;
        set_addr(g, &a);
        if (bind(g->listen_fd, (struct sockaddr *)&a, sizeof a) != 0 ||
                listen(g->listen_fd, 1) != 0)
            goto fail;
        if (chmod(g->path, 0700) != 0)
            osif_log(OSIF_LOG_WARN,
                    "IPC socket %s: could not restrict permissions (%s); "
                    "left at default",
                    g->path, strerror(errno));
    }
    g->running = 1;
    *h         = g;
    if (pthread_create(&g->rx, NULL, rx_thread, g) == 0)
        return OSIF_OK;
    *h = NULL;
fail:
    if (g->listen_fd >= 0) {
        close(g->listen_fd);
        unlink(g->path);
    }
    pthread_mutex_destroy(&g->send_lock);
    free(g);
    return OSIF_ERR;
}

int osif_ipc_connected(osif_ipc_t *g)
{
    return g && g->conn_fd >= 0;
}

unsigned osif_ipc_conn_seq(osif_ipc_t *g)
{
    return g ? atomic_load(&g->conn_seq) : 0;
}

osif_status_t osif_ipc_send(osif_ipc_t *g, uint32_t type, const void *data, int len)
{
    if (!g || g->conn_fd < 0)
        return OSIF_ERR;
    if (len < 0 || len > OSIF_IPC_MAX)
        return OSIF_ERR;
    struct iovec iov[2] = {
        { .iov_base = &type, .iov_len = sizeof type },
        { .iov_base = (void *)data, .iov_len = (size_t)len },
    };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
    size_t        n = sizeof type + (size_t)len;
    pthread_mutex_lock(&g->send_lock);
    ssize_t w = g->conn_fd >= 0 ? sendmsg(g->conn_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL) : -1;
    pthread_mutex_unlock(&g->send_lock);
    return (w == (ssize_t)n) ? OSIF_OK : OSIF_ERR;
}

void osif_ipc_deinit(osif_ipc_t *g)
{
    if (!g)
        return;
    g->running = 0;
    if (g->listen_fd >= 0)
        shutdown(g->listen_fd, SHUT_RDWR);
    pthread_mutex_lock(&g->send_lock);
    if (g->conn_fd >= 0)
        shutdown(g->conn_fd, SHUT_RDWR);
    pthread_mutex_unlock(&g->send_lock);
    pthread_join(g->rx, NULL);
    if (g->listen_fd >= 0)
        close(g->listen_fd);
    if (g->is_server)
        unlink(g->path);
    pthread_mutex_destroy(&g->send_lock);
    free(g);
}
