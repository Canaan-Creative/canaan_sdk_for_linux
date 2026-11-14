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
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "  FAIL: %s\n", m); \
            fails++; \
        } else \
            printf("  ok: %s\n", m); \
    } while (0)

static volatile int got;          /* 1 once server_cb received a message */
static uint32_t     got_type;     /* type of the last message server_cb received */
static char         got_data[64]; /* payload of the last message server_cb received */
static int          got_len;      /* payload length of the last message, bytes */
static osif_ipc_t  *seq_srv;      /* server whose connection sequence seq_cb reads */
static volatile int seq_n;        /* messages recorded by seq_cb, up to 4 */
static unsigned     seq_seen[4];  /* connection sequence number per recorded message */
static char         seq_tag[4];   /* first payload byte per recorded message */

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void server_cb(uint32_t type, const void *data, int len)
{
    got_type = type;
    got_len  = len;
    if (len > 0 && len < (int)sizeof got_data)
        memcpy(got_data, data, (size_t)len);
    got = 1;
}
static void client_cb(uint32_t t, const void *d, int l)
{
    (void)t;
    (void)d;
    (void)l;
}

static void seq_cb(uint32_t type, const void *data, int len)
{
    (void)type;
    if (seq_n == 0)
        msleep(200);
    if (seq_n < 4 && len > 0) {
        seq_seen[seq_n] = osif_ipc_conn_seq(seq_srv);
        seq_tag[seq_n]  = *(const char *)data;
        seq_n++;
    }
}

static int sockets_missing_cloexec(void)
{
    int seen = 0, bad = 0;
    for (int fd = 3; fd < 1024; fd++) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode))
            continue;
        seen++;
        int fl = fcntl(fd, F_GETFD);
        if (fl < 0 || !(fl & FD_CLOEXEC))
            bad++;
    }
    return seen >= 3 ? bad : -1;
}

static const char *sock_path(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "/tmp/osif_ipc_g8_%s.%d.sock", name, (int)getpid());
    return buf;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int backlog_fill(const char *path)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    for (int i = 0; i < 16; i++) {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
        if (fd < 0)
            return -1;
        if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            int full = errno == EAGAIN;
            close(fd);
            return full ? 0 : -1;
        }
    }
    return -1;
}

static int deinit_busy_server(const char *path)
{
    osif_ipc_t *s = NULL, *held = NULL, *late = NULL;
    uint64_t    t0;

    unlink(path);
    alarm(5);
    if (osif_ipc_init(&s, client_cb, path, 1) != OSIF_OK ||
            osif_ipc_init(&held, client_cb, path, 0) != OSIF_OK)
        return 2;
    for (int i = 0; i < 300 && !osif_ipc_connected(s); i++)
        msleep(10);
    if (!osif_ipc_connected(s) || backlog_fill(path) != 0)
        return 3;
    if (osif_ipc_init(&late, client_cb, path, 0) != OSIF_OK)
        return 2;
    msleep(200);
    t0 = now_ms();
    osif_ipc_deinit(late);
    if (now_ms() - t0 > 1000u)
        return 4;
    return 0;
}

int main(void)
{
    char        pbuf[5][64];
    const char *path = sock_path(pbuf[0], sizeof pbuf[0], "test");
    unlink(path);

    pid_t pid = fork();
    if (pid == 0) {
        osif_ipc_t *c = NULL;
        msleep(150);
        if (osif_ipc_init(&c, client_cb, path, 0) != OSIF_OK)
            _exit(2);
        for (int i = 0; i < 100 && !osif_ipc_connected(c); i++)
            msleep(10);
        if (!osif_ipc_connected(c)) {
            osif_ipc_deinit(c);
            _exit(3);
        }
        osif_ipc_send(c, 0x1234u, "ping", 5);
        msleep(200);
        osif_ipc_deinit(c);
        _exit(0);
    }

    int         fails = 0;
    osif_ipc_t *srv   = NULL;
    if (osif_ipc_init(&srv, server_cb, path, 1) != OSIF_OK) {
        fprintf(stderr, "server init failed\n");
        return 2;
    }
    for (int i = 0; i < 300 && !got; i++)
        msleep(10);
    int st = 0;
    waitpid(pid, &st, 0);
    osif_ipc_deinit(srv);

    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "client connected + sent + exited clean");
    CHECK(got, "server received a message");
    CHECK(got_type == 0x1234u, "message type preserved (0x1234)");
    CHECK(got_len == 5 && strcmp(got_data, "ping") == 0, "payload preserved (ping)");

    {
        const char *p2 = sock_path(pbuf[1], sizeof pbuf[1], "cloexec");
        osif_ipc_t *s2 = NULL, *c2 = NULL;
        unlink(p2);
        int ok = osif_ipc_init(&s2, client_cb, p2, 1) == OSIF_OK &&
                osif_ipc_init(&c2, client_cb, p2, 0) == OSIF_OK;
        for (int i = 0; ok && i < 300 && !(osif_ipc_connected(s2) && osif_ipc_connected(c2)); i++)
            msleep(10);
        ok = ok && osif_ipc_connected(s2) && osif_ipc_connected(c2);
        CHECK(ok && sockets_missing_cloexec() == 0,
                "listen, accepted and client sockets are all FD_CLOEXEC");
        osif_ipc_deinit(c2);
        osif_ipc_deinit(s2);
    }

    {
        const char *p3 = sock_path(pbuf[2], sizeof pbuf[2], "deinit");
        osif_ipc_t *s3 = NULL;
        unlink(p3);
        int ok  = osif_ipc_init(&s3, client_cb, p3, 1) == OSIF_OK;
        int st3 = -1;
        if (ok) {
            pid_t cp = fork();
            if (cp == 0) {
                alarm(10);
                for (int i = 0; i < 200; i++) {
                    osif_ipc_t *c3 = NULL;
                    if (osif_ipc_init(&c3, client_cb, p3, 0) != OSIF_OK)
                        _exit(2);
                    if (i & 1)
                        msleep(1);
                    osif_ipc_deinit(c3);
                }
                _exit(0);
            }
            waitpid(cp, &st3, 0);
            osif_ipc_deinit(s3);
        }
        CHECK(ok && WIFEXITED(st3) && WEXITSTATUS(st3) == 0,
                "client deinit right after init against a live server never hangs (200 cycles)");
    }

    {
        int   st5 = -1;
        pid_t cp  = fork();
        if (cp == 0)
            _exit(deinit_busy_server(sock_path(pbuf[3], sizeof pbuf[3], "busy")));
        waitpid(cp, &st5, 0);
        CHECK(WIFEXITED(st5) && WEXITSTATUS(st5) == 0,
                "client deinit returns within 1 s while the server is busy and its "
                "backlog is full");
    }

    {
        const char *p4 = sock_path(pbuf[4], sizeof pbuf[4], "seq");
        osif_ipc_t *ca = NULL, *cb = NULL;
        unlink(p4);
        int      ok   = osif_ipc_init(&seq_srv, seq_cb, p4, 1) == OSIF_OK;
        unsigned seq0 = osif_ipc_conn_seq(seq_srv);
        ok            = ok && osif_ipc_init(&ca, client_cb, p4, 0) == OSIF_OK;
        for (int i = 0; ok && i < 300 && !osif_ipc_connected(ca); i++)
            msleep(10);
        ok = ok && osif_ipc_send(ca, 1u, "a", 2) == OSIF_OK &&
                osif_ipc_send(ca, 1u, "a", 2) == OSIF_OK;
        osif_ipc_deinit(ca);
        ok = ok && osif_ipc_init(&cb, client_cb, p4, 0) == OSIF_OK;
        for (int i = 0; ok && i < 300 && !osif_ipc_connected(cb); i++)
            msleep(10);
        ok = ok && osif_ipc_send(cb, 1u, "b", 2) == OSIF_OK;
        for (int i = 0; ok && i < 300 && seq_n < 3; i++)
            msleep(10);
        CHECK(ok && seq0 == 0 && seq_n == 3,
                "conn_seq is 0 before any connection; all three messages arrive");
        CHECK(seq_tag[0] == 'a' && seq_tag[1] == 'a' && seq_tag[2] == 'b' && seq_seen[0] == 1 &&
                        seq_seen[1] == 1 && seq_seen[2] == 2,
                "a delayed receiver reads the closed connection's queued messages "
                "under its own conn_seq, before the next connection's");
        CHECK(osif_ipc_conn_seq(seq_srv) == 2, "conn_seq counts accepted connections");
        osif_ipc_deinit(cb);
        osif_ipc_deinit(seq_srv);
    }

    if (fails) {
        fprintf(stderr, "test_osif_ipc: %d FAIL\n", fails);
        return 1;
    }
    printf("test_osif_ipc: all pass\n");
    return 0;
}
