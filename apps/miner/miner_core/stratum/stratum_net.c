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

#include "stratum_net.h"
#include "osif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <netinet/in.h>

typedef struct {
    int             fd;
    char            rx[STRATUM_LINE_MAX];
    size_t          rx_len;
    int             rx_skip;    /* 1 while discarding an over-long line */
    size_t          rx_skipped; /* bytes discarded of that line */
    pthread_mutex_t tx_lock;
} tcp_io;

static stratum_trace_fn g_trace; /* line trace hook, NULL if none */

void stratum_set_trace(stratum_trace_fn fn)
{
    g_trace = fn;
}

void stratum_trace_line(int tx, const char *line)
{
    if (g_trace && line)
        g_trace(tx, line);
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int send_owned(stratum_transport *t, char *line)
{
    if (!line)
        return -1;
    stratum_trace_line(1, line);
    int rc = t->send_line(t->io, line);
    free(line);
    return rc;
}

const char *stratum_end_str(int reason)
{
    switch (reason) {
    case STRATUM_END_EOF:
        return "pool closed the connection";
    case STRATUM_END_STOPPED:
        return "stopped by the caller";
    case STRATUM_END_ERROR:
        return "socket error";
    case STRATUM_END_TIMEOUT:
        return "pool sent no usable line for the recv timeout";
    case STRATUM_END_SEND:
        return "send failed";
    case STRATUM_END_REFUSED:
        return "pool did not complete the handshake";
    default:
        return "unknown";
    }
}

int stratum_session_established(const sv1_client *c)
{
    return c && c->state >= SV1_AUTHORIZED;
}

int stratum_run(sv1_client *c, stratum_transport *t, const char *user, const char *pass,
        const char *user_agent, stratum_on_event on_event, void *ctx)
{
    if (send_owned(t, sv1_build_configure(c)) != 0)
        return STRATUM_END_SEND;

    char     line[STRATUM_LINE_MAX];
    uint64_t idle_at = mono_ms() + STRATUM_RECV_TIMEOUT_S * 1000u;
    for (;;) {
        int n = t->recv_line(t->io, line, sizeof line);
        if (n == 0)
            return STRATUM_END_EOF;
        if (n < 0)
            return n == -2 ? STRATUM_END_TIMEOUT : STRATUM_END_ERROR;

        stratum_trace_line(0, line);

        sv1_event ev;
        if (sv1_feed_line(c, line, &ev) != 0) {
            if (!stratum_session_established(c))
                return STRATUM_END_REFUSED;
            if (mono_ms() >= idle_at)
                return STRATUM_END_TIMEOUT;
            continue;
        }
        if (ev.type != SV1_EV_ERROR)
            idle_at = mono_ms() + STRATUM_RECV_TIMEOUT_S * 1000u;

        switch (ev.type) {
        case SV1_EV_CONFIGURED:
            if (send_owned(t, sv1_build_subscribe(c, user_agent)) != 0)
                return STRATUM_END_SEND;
            break;
        case SV1_EV_SUBSCRIBED:
            if (send_owned(t, sv1_build_authorize(c, user, pass ? pass : "")) != 0)
                return STRATUM_END_SEND;
            if (c->extranonce_subscribe && send_owned(t, sv1_build_extranonce_subscribe(c)) != 0)
                return STRATUM_END_SEND;
            break;
        case SV1_EV_PING:
            if (send_owned(t, sv1_build_pong(ev.req_id)) != 0)
                return STRATUM_END_SEND;
            break;
        case SV1_EV_GET_VERSION:
            if (send_owned(t, sv1_build_version(ev.req_id, user_agent)) != 0)
                return STRATUM_END_SEND;
            break;
        case SV1_EV_ERROR:
            if (!stratum_session_established(c))
                return STRATUM_END_REFUSED;
            break;
        default:
            break;
        }

        if (on_event && on_event(ctx, c, &ev))
            return STRATUM_END_STOPPED;
        if (mono_ms() >= idle_at)
            return STRATUM_END_TIMEOUT;
    }
}

int stratum_parse_url(const char *url, char *host, size_t host_cap, int *port)
{
    if (!url || !host || !port)
        return -1;
    const char *p     = strstr(url, "://");
    p                 = p ? p + 3 : url;
    const char *colon = strrchr(p, ':');
    if (!colon || colon == p)
        return -1;
    size_t hlen = (size_t)(colon - p);
    if (hlen >= host_cap)
        return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    int pt     = atoi(colon + 1);
    if (pt <= 0 || pt > 65535)
        return -1;
    *port = pt;
    return 0;
}

static int tcp_send(void *io_, const char *line)
{
    tcp_io      *io     = io_;
    struct iovec iov[2] = {
        { .iov_base = (void *)line, .iov_len = strlen(line) },
        { .iov_base = "\n", .iov_len = 1 },
    };
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };
    int           rc  = 0;

    pthread_mutex_lock(&io->tx_lock);
    while (msg.msg_iovlen) {
        ssize_t w = sendmsg(io->fd, &msg, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            rc = -1;
            break;
        }
        while (msg.msg_iovlen && (size_t)w >= msg.msg_iov->iov_len) {
            w -= (ssize_t)msg.msg_iov->iov_len;
            msg.msg_iov++;
            msg.msg_iovlen--;
        }
        if (msg.msg_iovlen) {
            msg.msg_iov->iov_base = (char *)msg.msg_iov->iov_base + w;
            msg.msg_iov->iov_len -= (size_t)w;
        }
    }
    pthread_mutex_unlock(&io->tx_lock);
    return rc;
}

static int tcp_recv(void *io_, char *buf, size_t cap)
{
    tcp_io  *io       = io_;
    uint64_t deadline = mono_ms() + STRATUM_RECV_TIMEOUT_S * 1000u;
    for (;;) {
        char *nl = memchr(io->rx, '\n', io->rx_len);
        if (nl) {
            size_t linelen  = (size_t)(nl - io->rx);
            size_t consumed = linelen + 1;
            size_t copy     = 0;
            int    skipped  = io->rx_skip;
            if (!skipped) {
                copy = linelen < cap - 1 ? linelen : cap - 1;
                memcpy(buf, io->rx, copy);
                buf[copy] = '\0';
            }
            memmove(io->rx, io->rx + consumed, io->rx_len - consumed);
            io->rx_len -= consumed;
            io->rx_skip = 0;
            if (skipped || linelen == 0)
                continue;
            return (int)copy;
        }
        if (!io->rx_skip && io->rx_len >= sizeof io->rx) {
            osif_log(OSIF_LOG_ERR,
                    "pool sent a message longer than %d bytes; "
                    "it is skipped",
                    (int)sizeof io->rx - 1);
            io->rx_skip    = 1;
            io->rx_skipped = 0;
        }
        if (io->rx_skip) {
            io->rx_skipped += io->rx_len;
            io->rx_len = 0;
            if (io->rx_skipped > STRATUM_SKIP_MAX) {
                osif_log(OSIF_LOG_ERR,
                        "pool sent over %d bytes without a line "
                        "break; disconnecting",
                        STRATUM_SKIP_MAX);
                return -1;
            }
        }
        uint64_t now = mono_ms();
        if (now >= deadline)
            return -2;
        struct pollfd pfd = { .fd = io->fd, .events = POLLIN };
        int           pr  = poll(&pfd, 1, (int)(deadline - now));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pr == 0)
            return -2;
        ssize_t r = read(io->fd, io->rx + io->rx_len, sizeof io->rx - io->rx_len);
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -2;
            return -1;
        }
        io->rx_len += (size_t)r;
    }
}

static int connect_bounded(int fd, const struct sockaddr *sa, socklen_t slen, int budget_ms)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;

    int rc = connect(fd, sa, slen);
    if (rc < 0 && errno == EINPROGRESS) {
        uint64_t      deadline = mono_ms() + (uint64_t)budget_ms;
        struct pollfd pfd      = { .fd = fd, .events = POLLOUT };
        int           pr;
        for (;;) {
            uint64_t now = mono_ms();
            pr           = poll(&pfd, 1, now >= deadline ? 0 : (int)(deadline - now));
            if (pr < 0 && errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            rc    = -1;
        } else if (pr > 0) {
            int       soerr = 0;
            socklen_t sl    = sizeof soerr;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0)
                rc = -1;
            else if (soerr) {
                errno = soerr;
                rc    = -1;
            } else
                rc = 0;
        }
    }

    int keep = errno;
    fcntl(fd, F_SETFL, fl);
    errno = keep;
    return rc;
}

int stratum_tcp_connect(stratum_transport *t, const char *host, int port)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);

    struct addrinfo  hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res   = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return STRATUM_CONNECT_RESOLVE;

    uint64_t deadline = mono_ms() + STRATUM_CONNECT_TOTAL_MS;
    int      fd = -1, cerr = 0, timedout = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        uint64_t now = mono_ms();
        if (now >= deadline) {
            cerr     = ETIMEDOUT;
            timedout = 1;
            break;
        }
        uint64_t left   = deadline - now;
        int      budget = left < STRATUM_CONNECT_ADDR_MS ? (int)left : STRATUM_CONNECT_ADDR_MS;
        fd              = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            cerr = errno;
            continue;
        }
        if (connect_bounded(fd, a->ai_addr, a->ai_addrlen, budget) == 0)
            break;
        cerr     = errno;
        timedout = (cerr == ETIMEDOUT);
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        errno = cerr;
        return timedout ? STRATUM_CONNECT_TIMEOUT : STRATUM_CONNECT_ERR;
    }

    struct timeval rto = { .tv_sec = STRATUM_RECV_TIMEOUT_S };
    struct timeval sto = { .tv_sec = STRATUM_SEND_TIMEOUT_S };
    int            ka  = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof rto);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof sto);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);

    tcp_io *io = calloc(1, sizeof *io);
    if (!io) {
        close(fd);
        return -1;
    }
    io->fd = fd;
    pthread_mutex_init(&io->tx_lock, NULL);
    t->send_line = tcp_send;
    t->recv_line = tcp_recv;
    t->io        = io;
    return 0;
}

void stratum_tcp_close(stratum_transport *t)
{
    if (!t || !t->io)
        return;
    tcp_io *io = t->io;
    if (io->fd >= 0)
        close(io->fd);
    pthread_mutex_destroy(&io->tx_lock);
    free(io);
    t->io = NULL;
}

void stratum_tcp_shutdown(stratum_transport *t)
{
    if (!t || !t->io)
        return;
    tcp_io *io = t->io;
    if (io->fd >= 0)
        shutdown(io->fd, SHUT_RDWR);
}
