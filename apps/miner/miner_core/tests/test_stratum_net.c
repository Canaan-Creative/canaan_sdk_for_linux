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
#include "pool.h"
#include "jread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <time.h>

#include "osif.h"

#ifndef STRATUM_FIXTURES_DIR
/* Directory holding the stratum/ fixtures when the build does not set it */
#define STRATUM_FIXTURES_DIR "."
#endif

#define MAX_LINES 256 /* Scripted receive lines kept */

#define MAX_TX 32 /* Sent lines kept */

#define IDLE_STEP_MS 10000 /* Simulated time each idle line advances CLOCK_MONOTONIC, ms */
/* Idle lines mock_recv_idle() returns before reporting the peer closed */
#define IDLE_LINES_MAX 100

#define TRICKLE_STEP_MS 30000 /* Simulated time between trickled bytes, ms */
#define TRICKLE_BYTES   20    /* Bytes trickle_sender() sends at most */

#define TX_RACE_LINES 2000 /* Lines each tx_race_sender() thread sends */
#define MAX_CONN      8    /* Connections tracked in g_cs */

/* Argument of tx_race_sender(): transport to send on and the line to send */
typedef struct {
    stratum_transport *t;
    const char        *line;
} tx_race_arg;

/* Per-connection happy.jsonl figures: first/last line, max rx gap, last rx time, rx, submits */
typedef struct {
    double first, last, max_rx_gap, prev_rx;
    int    rx, submits;
} conn_stat;

static int64_t g_mono_off_ms; /* Offset added to CLOCK_MONOTONIC, ms; applied in whole seconds */

static int         g_err_n;            /* OSIF_LOG_ERR messages logged */
static char       *g_lines[MAX_LINES]; /* Scripted lines mock_recv() returns in order */
static int         g_nlines, g_pos;    /* Scripted lines loaded; index of the next one to return */
static int         g_after_lines;      /* mock_recv() return once the scripted lines run out */
static int         g_send_fails;       /* Non-zero makes every mock_send() fail */
static char        g_tx[MAX_TX][256];  /* Lines mock_send() was given */
static int         g_tx_n;             /* mock_send() calls, including those not kept */
static const char *g_send_fail_on;     /* Substring that makes mock_send() fail when set */
/* Scripted reply ids of mining.configure, .subscribe and .authorize */
static int g_script_id[3] = { 1, 2, 3 };
static int g_cfg_id; /* id of the last mining.configure mock_send() was given */

static char        g_tr[64][256]; /* Lines passed to trace_cb() */
static int         g_tr_dir[64];  /* tx flag of each traced line */
static int         g_tr_n;        /* Lines traced */
static const char *g_idle_line;   /* Line mock_recv_idle() repeats once the script runs out */
/* Every how many idle lines a mining.ping replaces it, 0 for never; idle lines returned */
static int g_idle_ping_every, g_idle_n;

static int       g_notify;       /* SV1_EV_NOTIFY events seen by on_event() */
static int       g_stop;         /* Value on_event() returns */
static conn_stat g_cs[MAX_CONN]; /* Figures per connection index */
static double    g_tmax;         /* Latest line time in happy.jsonl */

int clock_gettime(clockid_t id, struct timespec *ts)
{
    int rc = (int)syscall(SYS_clock_gettime, id, ts);
    if (rc == 0 && id == CLOCK_MONOTONIC)
        ts->tv_sec += __atomic_load_n(&g_mono_off_ms, __ATOMIC_RELAXED) / 1000;
    return rc;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)fmt;
    if (lvl == OSIF_LOG_ERR)
        g_err_n++;
}

static int mock_send(void *io, const char *line)
{
    (void)io;
    if (g_tx_n < MAX_TX)
        snprintf(g_tx[g_tx_n], sizeof g_tx[0], "%s", line);
    g_tx_n++;
    if (strstr(line, "\"mining.configure\""))
        sscanf(line, "{\"id\":%d", &g_cfg_id);
    if (g_send_fail_on && strstr(line, g_send_fail_on))
        return -1;
    return g_send_fails ? -1 : 0;
}
static const char *tx_find(const char *needle)
{
    for (int i = 0; i < g_tx_n && i < MAX_TX; i++)
        if (strstr(g_tx[i], needle))
            return g_tx[i];
    return NULL;
}
static void trace_cb(int tx, const char *line)
{
    if (g_tr_n >= 64)
        return;
    snprintf(g_tr[g_tr_n], sizeof g_tr[0], "%s", line);
    g_tr_dir[g_tr_n] = tx;
    g_tr_n++;
}
static const char *tr_find(int tx, const char *needle)
{
    for (int i = 0; i < g_tr_n; i++)
        if (g_tr_dir[i] == tx && strstr(g_tr[i], needle))
            return g_tr[i];
    return NULL;
}
static int mock_recv(void *io, char *buf, size_t cap)
{
    const char *line;
    int         id, off = 0;

    (void)io;
    if (g_pos >= g_nlines)
        return g_after_lines;
    line = g_lines[g_pos++];
    snprintf(buf, cap, "%s", line);
    if (strstr(line, "\"result\"") && sscanf(line, "{\"id\":%d,%n", &id, &off) == 1 && off)
        for (int k = 0; k < 3; k++)
            if (id == g_script_id[k])
                snprintf(buf, cap, "{\"id\":%d,%s", g_cfg_id + k, line + off);
    return (int)strlen(buf);
}

static int next_sv1_id(void)
{
    sv1_client probe;
    int        id;
    sv1_init(&probe);
    free(sv1_build_configure(&probe));
    id = probe.id_configure + 1;
    sv1_free(&probe);
    return id;
}

static int mock_recv_idle(void *io, char *buf, size_t cap)
{
    if (g_pos < g_nlines)
        return mock_recv(io, buf, cap);
    if (g_idle_n >= IDLE_LINES_MAX)
        return 0;
    g_idle_n++;
    g_mono_off_ms += IDLE_STEP_MS;
    if (g_idle_ping_every && g_idle_n % g_idle_ping_every == 0)
        snprintf(buf, cap, "{\"id\":5,\"method\":\"mining.ping\",\"params\":[]}");
    else
        snprintf(buf, cap, "%s", g_idle_line);
    return (int)strlen(buf);
}

static int run_idle(const char *line, int ping_every)
{
    g_lines[g_nlines++] = strdup("{\"id\":1,\"result\":{\"version-rolling\":true,"
                                 "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}");
    g_lines[g_nlines++] = strdup("{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"0a1b\",8],"
                                 "\"error\":null}");
    g_lines[g_nlines++] = strdup("{\"id\":3,\"result\":true,\"error\":null}");
    g_pos               = 0;
    g_tx_n              = 0;
    g_send_fails        = 0;
    g_idle_line         = line;
    g_idle_ping_every   = ping_every;
    g_idle_n            = 0;
    g_mono_off_ms       = 0;

    sv1_client c;
    sv1_init(&c);
    stratum_transport t = { mock_send, mock_recv_idle, NULL };
    int rc              = stratum_run(&c, &t, "poolworker.001", "x", "AvalonMiner/1.0", NULL, NULL);
    assert(stratum_session_established(&c));
    sv1_free(&c);
    g_mono_off_ms = 0;
    for (int i = 0; i < g_nlines; i++)
        free(g_lines[i]);
    g_nlines = g_pos = 0;
    return rc;
}

static void test_idle_deadline(void)
{
    assert(run_idle("{\"id\":", 0) == STRATUM_END_TIMEOUT);
    assert(g_idle_n == STRATUM_RECV_TIMEOUT_S * 1000 / IDLE_STEP_MS);
    assert(run_idle("<html>502 Bad Gateway</html>", 0) == STRATUM_END_TIMEOUT);
    assert(run_idle("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0]}", 0) ==
            STRATUM_END_TIMEOUT);
    assert(g_idle_n == STRATUM_RECV_TIMEOUT_S * 1000 / IDLE_STEP_MS);
    assert(run_idle("{\"id\":", 5) == STRATUM_END_EOF);
    assert(g_idle_n == IDLE_LINES_MAX);
}

static void loopback_pair(stratum_transport *t, int *peer)
{
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    int ls               = socket(AF_INET, SOCK_STREAM, 0);
    assert(ls >= 0);
    int rc = bind(ls, (struct sockaddr *)&a, sizeof a);
    assert(rc == 0);
    rc = listen(ls, 1);
    assert(rc == 0);
    socklen_t al = sizeof a;
    rc           = getsockname(ls, (struct sockaddr *)&a, &al);
    assert(rc == 0);
    rc = stratum_tcp_connect(t, "127.0.0.1", ntohs(a.sin_port));
    assert(rc == 0);
    *peer = accept(ls, NULL, NULL);
    assert(*peer >= 0);
    close(ls);
}

static void peer_write(int fd, const char *s)
{
    ssize_t w = write(fd, s, strlen(s));
    assert(w == (ssize_t)strlen(s));
}

static void peer_write_handshake(int fd, int id, const char *gap)
{
    char l[160];
    snprintf(l, sizeof l,
            "{\"id\":%d,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}\n%s",
            id, gap);
    peer_write(fd, l);
    snprintf(l, sizeof l,
            "{\"id\":%d,\"result\":[[[\"mining.notify\",\"a\"]],\"0a1b\",8],"
            "\"error\":null}\n%s",
            id + 1, gap);
    peer_write(fd, l);
    snprintf(l, sizeof l, "{\"id\":%d,\"result\":true,\"error\":null}\n", id + 2);
    peer_write(fd, l);
}

static void *skip_flood_sender(void *p)
{
    static char chunk[4096];
    int         fd   = *(int *)p;
    size_t      sent = 0;

    memset(chunk, 'z', sizeof chunk);
    while (sent < STRATUM_SKIP_MAX + 2 * STRATUM_LINE_MAX) {
        ssize_t w = send(fd, chunk, sizeof chunk, MSG_NOSIGNAL);
        if (w <= 0)
            break;
        sent += (size_t)w;
    }
    return NULL;
}

static void *trickle_sender(void *p)
{
    int fd = *(int *)p;

    for (int i = 0; i < TRICKLE_BYTES; i++) {
        if (send(fd, "x", 1, MSG_NOSIGNAL) != 1)
            break;
        usleep(20000);
        __atomic_add_fetch(&g_mono_off_ms, TRICKLE_STEP_MS, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void test_tcp_recv_line_deadline(void)
{
    stratum_transport lt;
    int               peer;
    char              got[STRATUM_LINE_MAX];
    pthread_t         th;
    struct timespec   t0, t1;

    loopback_pair(&lt, &peer);
    peer_write(peer, "{\"id\":1,\"result\":true,\"error\":null}\n");
    assert(lt.recv_line(lt.io, got, sizeof got) ==
            (int)strlen("{\"id\":1,\"result\":true,\"error\":null}"));
    assert(!strcmp(got, "{\"id\":1,\"result\":true,\"error\":null}"));

    g_mono_off_ms = 0;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t0);
    pthread_create(&th, NULL, trickle_sender, &peer);
    assert(lt.recv_line(lt.io, got, sizeof got) == -2);
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t1);
    assert(t1.tv_sec - t0.tv_sec < 5);
    assert(__atomic_load_n(&g_mono_off_ms, __ATOMIC_RELAXED) >= STRATUM_RECV_TIMEOUT_S * 1000);
    pthread_join(th, NULL);
    g_mono_off_ms = 0;
    stratum_tcp_close(&lt);
    close(peer);
}

static void *tx_race_sender(void *p)
{
    tx_race_arg *a = p;
    for (int i = 0; i < TX_RACE_LINES; i++) {
        int rc = a->t->send_line(a->t->io, a->line);
        assert(rc == 0);
    }
    return NULL;
}

static void test_tcp_send_concurrent_lines_whole(void)
{
    static const char pong[]   = "{\"id\":7,\"result\":\"pong\",\"error\":null}";
    static const char submit[] = "{\"id\":9,\"method\":\"mining.submit\",\"params\":"
                                 "[\"w\",\"J1\",\"00000000\",\"5f000000\",\"1234abcd\"]}";
    stratum_transport lt;
    int               peer;
    loopback_pair(&lt, &peer);

    tx_race_arg a0 = { &lt, pong }, a1 = { &lt, submit };
    pthread_t   th0, th1;
    int         rc = pthread_create(&th0, NULL, tx_race_sender, &a0);
    assert(rc == 0);
    rc = pthread_create(&th1, NULL, tx_race_sender, &a1);
    assert(rc == 0);

    char   buf[4096], cur[256];
    size_t cur_n = 0;
    int    npong = 0, nsubmit = 0;
    while (npong + nsubmit < 2 * TX_RACE_LINES) {
        ssize_t r = read(peer, buf, sizeof buf);
        assert(r > 0);
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] != '\n') {
                assert(cur_n < sizeof cur - 1);
                cur[cur_n++] = buf[i];
                continue;
            }
            cur[cur_n] = '\0';
            if (!strcmp(cur, pong))
                npong++;
            else if (!strcmp(cur, submit))
                nsubmit++;
            else
                assert(!"interleaved or truncated line on the wire");
            cur_n = 0;
        }
    }
    pthread_join(th0, NULL);
    pthread_join(th1, NULL);
    assert(cur_n == 0);
    assert(npong == TX_RACE_LINES && nsubmit == TX_RACE_LINES);
    close(peer);
    stratum_tcp_close(&lt);
}
static int on_event(void *ctx, sv1_client *c, const sv1_event *ev)
{
    (void)ctx;
    (void)c;
    if (ev->type == SV1_EV_NOTIFY)
        g_notify++;
    return g_stop;
}

static int run_until(int after, int send_fails, int *st)
{
    g_pos         = 0;
    g_tx_n        = 0;
    g_after_lines = after;
    g_send_fails  = send_fails;
    sv1_client c;
    sv1_init(&c);
    stratum_transport t = { mock_send, mock_recv, NULL };
    int rc = stratum_run(&c, &t, "poolworker.001", "x", "AvalonMiner/1.0", on_event, NULL);
    if (st)
        *st = c.state;
    sv1_free(&c);
    return rc;
}

static void load_conn_stats(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f && "happy.jsonl missing");
    for (int i = 0; i < MAX_CONN; i++)
        g_cs[i].first = g_cs[i].prev_rx = -1;
    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    while ((n = getline(&buf, &cap, f)) > 0) {
        json_value *o = json_parse(buf);
        if (!o)
            continue;
        long        conn = (long)json_int(json_object_get(o, "conn"));
        double      t    = json_number(json_object_get(o, "t"));
        const char *dir  = json_string(json_object_get(o, "dir"));
        const char *line = json_string(json_object_get(o, "line"));
        if (conn >= 0 && conn < MAX_CONN && dir) {
            conn_stat *s = &g_cs[conn];
            if (s->first < 0)
                s->first = t;
            s->last = t;
            if (t > g_tmax)
                g_tmax = t;
            if (!strcmp(dir, "rx")) {
                if (s->prev_rx >= 0 && t - s->prev_rx > s->max_rx_gap)
                    s->max_rx_gap = t - s->prev_rx;
                s->prev_rx = t;
                s->rx++;
            } else if (line && strstr(line, "mining.submit")) {
                s->submits++;
            }
        }
        json_free(o);
    }
    free(buf);
    fclose(f);
}

static void load_conn0_rx(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f && "happy.jsonl missing");
    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    while ((n = getline(&buf, &cap, f)) > 0) {
        json_value *o = json_parse(buf);
        if (!o)
            continue;
        long        conn = json_int(json_object_get(o, "conn"));
        const char *dir  = json_string(json_object_get(o, "dir"));
        const char *line = json_string(json_object_get(o, "line"));
        if (conn == 0 && dir && !strcmp(dir, "rx") && line && line[0] && g_nlines < MAX_LINES)
            g_lines[g_nlines++] = strdup(line);
        if (conn == 0 && dir && !strcmp(dir, "tx") && line) {
            const json_value *msg = json_object_get(o, "msg");
            const char       *m   = json_string(json_object_get(msg, "method"));
            int               tid = (int)json_int(json_object_get(msg, "id"));
            if (m && !strcmp(m, "mining.configure"))
                g_script_id[0] = tid;
            if (m && !strcmp(m, "mining.subscribe"))
                g_script_id[1] = tid;
            if (m && !strcmp(m, "mining.authorize"))
                g_script_id[2] = tid;
        }
        json_free(o);
    }
    free(buf);
    fclose(f);
}

int main(void)
{
    char host[128];
    int  port = 0;
    assert(stratum_parse_url("stratum+tcp://btc.f2pool.com:3333", host, sizeof host, &port) == 0);
    assert(!strcmp(host, "btc.f2pool.com") && port == 3333);
    assert(stratum_parse_url("h:1", host, sizeof host, &port) == 0 && port == 1);
    assert(stratum_parse_url("noport", host, sizeof host, &port) == -1);

    {
        int st0             = 0;
        g_lines[g_nlines++] = strdup("{\"id\":1,\"result\":{\"version-rolling\":true,"
                                     "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}");
        g_lines[g_nlines++] = strdup("{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"000\",8],"
                                     "\"error\":null}");
        assert(run_until(-2, 0, &st0) == STRATUM_END_REFUSED);
        assert(st0 == SV1_CONFIGURED);
        assert(tx_find("mining.subscribe"));
        assert(!tx_find("mining.authorize"));
        for (int i = 0; i < g_nlines; i++)
            free(g_lines[i]);
        g_nlines = g_pos = 0;
        g_after_lines    = 0;
    }

    {
        int st0             = 0;
        g_lines[g_nlines++] = strdup("{\"id\":1,\"result\":{\"version-rolling\":true,"
                                     "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}");
        g_lines[g_nlines++] =
                strdup("{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"0000\",8],"
                       "\"error\":null}");
        g_lines[g_nlines++] = strdup("{\"id\":3,\"result\":false,"
                                     "\"error\":[24,\"Unauthorized worker\",null]}");
        assert(run_until(-2, 0, &st0) == STRATUM_END_REFUSED);
        assert(st0 == SV1_SUBSCRIBED);
        assert(tx_find("mining.authorize"));
        for (int i = 0; i < g_nlines; i++)
            free(g_lines[i]);
        g_nlines = g_pos = 0;
        g_after_lines    = 0;
    }

    {
        int st0             = 0;
        g_lines[g_nlines++] = strdup("{\"id\":1,\"result\":{\"version-rolling\":true,"
                                     "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}");
        g_lines[g_nlines++] = strdup("<html>502 Bad Gateway</html>");
        assert(run_until(-2, 0, &st0) == STRATUM_END_REFUSED);
        assert(st0 == SV1_CONFIGURED);
        for (int i = 0; i < g_nlines; i++)
            free(g_lines[i]);
        g_nlines = g_pos = 0;
        g_after_lines    = 0;
    }

    {
        sv1_client sc;
        sv1_init(&sc);
        assert(!stratum_session_established(&sc));
        sc.state = SV1_CONFIGURED;
        assert(!stratum_session_established(&sc));
        sc.state = SV1_SUBSCRIBED;
        assert(!stratum_session_established(&sc));
        sc.state = SV1_AUTHORIZED;
        assert(stratum_session_established(&sc));
        sc.state = SV1_MINING;
        assert(stratum_session_established(&sc));
        sv1_free(&sc);
    }

    {
        pool_set ps;
        pool_init(&ps, 1000, 8000);
        sv1_client sc;
        sv1_init(&sc);
        assert(pool_backoff_next(&ps) == 1000);
        sc.state = SV1_SUBSCRIBED;
        if (stratum_session_established(&sc))
            pool_backoff_reset(&ps);
        assert(pool_backoff_next(&ps) == 2000);
        if (stratum_session_established(&sc))
            pool_backoff_reset(&ps);
        assert(pool_backoff_next(&ps) == 4000);
        sc.state = SV1_MINING;
        if (stratum_session_established(&sc))
            pool_backoff_reset(&ps);
        assert(pool_backoff_next(&ps) == 1000);
        sv1_free(&sc);
    }

    test_idle_deadline();

    load_conn0_rx(STRATUM_FIXTURES_DIR "/stratum/happy.jsonl");
    assert(g_nlines > 0);

    sv1_client c;
    sv1_init(&c);
    stratum_transport t = { mock_send, mock_recv, NULL };
    int rc = stratum_run(&c, &t, "poolworker.001", "x", "AvalonMiner/1.0", on_event, NULL);
    assert(rc == 0);
    assert(rc == STRATUM_END_EOF);
    assert(c.state == SV1_MINING);
    assert(stratum_session_established(&c));
    assert(g_notify > 0);
    sv1_free(&c);

    int st = 0;
    assert(run_until(0, 0, &st) == STRATUM_END_EOF && st == SV1_MINING);
    assert(run_until(-1, 0, &st) == STRATUM_END_ERROR && st == SV1_MINING);
    assert(run_until(-2, 0, &st) == STRATUM_END_TIMEOUT && st == SV1_MINING);
    assert(run_until(0, 1, &st) == STRATUM_END_SEND && st == SV1_INIT);
    g_stop = 1;
    assert(run_until(0, 0, &st) == STRATUM_END_STOPPED);
    g_stop              = 0;
    const char *names[] = {
        stratum_end_str(STRATUM_END_EOF),
        stratum_end_str(STRATUM_END_STOPPED),
        stratum_end_str(STRATUM_END_ERROR),
        stratum_end_str(STRATUM_END_TIMEOUT),
        stratum_end_str(STRATUM_END_SEND),
        stratum_end_str(STRATUM_END_REFUSED),
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        assert(names[i] && names[i][0]);
        for (size_t j = i + 1; j < sizeof names / sizeof names[0]; j++)
            assert(strcmp(names[i], names[j]) != 0);
    }

    assert(g_nlines + 2 <= MAX_LINES);
    g_lines[g_nlines++] = strdup("{\"id\":77,\"method\":\"mining.ping\",\"params\":[]}");
    g_lines[g_nlines++] = strdup("{\"id\":78,\"method\":\"client.get_version\",\"params\":[]}");

    assert(run_until(0, 0, &st) == STRATUM_END_EOF);
    assert(tx_find("{\"id\": 77, \"result\": \"pong\", \"error\": null}"));
    assert(tx_find("{\"id\": 78, \"result\": \"AvalonMiner/1.0\", \"error\": null}"));

    g_send_fail_on = "pong";
    assert(run_until(0, 0, &st) == STRATUM_END_SEND);
    assert(tx_find("mining.authorize"));
    g_send_fail_on = NULL;

    assert(g_nlines + 2 <= MAX_LINES);
    g_lines[g_nlines++] =
            strdup("{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0]}");
    assert(run_until(0, 0, &st) == STRATUM_END_EOF && st == SV1_MINING);
    g_lines[g_nlines++] = strdup("<html>502 Bad Gateway</html>");
    assert(run_until(0, 0, &st) == STRATUM_END_EOF && st == SV1_MINING);

    g_tr_n = 0;
    assert(run_until(0, 0, &st) == STRATUM_END_EOF);
    assert(g_tr_n == 0);
    stratum_set_trace(trace_cb);
    g_tr_n = 0;
    assert(run_until(0, 0, &st) == STRATUM_END_EOF);
    assert(tr_find(1, "mining.configure"));
    assert(tr_find(1, "\"result\": \"pong\""));
    assert(tr_find(0, "mining.notify"));
    g_tr_n = 0;
    stratum_trace_line(1, "a line sent outside the run loop");
    assert(g_tr_n == 1 && !strcmp(g_tr[0], "a line sent outside the run loop"));
    stratum_set_trace(NULL);
    g_tr_n = 0;
    stratum_trace_line(1, "after the logger is removed");
    assert(g_tr_n == 0);

    {
        stratum_transport lt;
        int               peer;
        char              lbuf[256];
        loopback_pair(&lt, &peer);
        struct timeval rto = { 0 };
        socklen_t      rl  = sizeof rto;
        assert(getsockopt(*(int *)lt.io, SOL_SOCKET, SO_RCVTIMEO, &rto, &rl) == 0);
        assert(rto.tv_sec == 90);
        peer_write(peer, "\n\n{\"id\":1}\n\n\n{\"id\":2}\n");
        int ln = lt.recv_line(lt.io, lbuf, sizeof lbuf);
        assert(ln == (int)strlen(lbuf) && !strcmp(lbuf, "{\"id\":1}"));
        ln = lt.recv_line(lt.io, lbuf, sizeof lbuf);
        assert(ln == (int)strlen(lbuf) && !strcmp(lbuf, "{\"id\":2}"));
        close(peer);
        assert(lt.recv_line(lt.io, lbuf, sizeof lbuf) == 0);
        stratum_tcp_close(&lt);
    }

    {
        stratum_transport lt;
        int               peer;
        static char       big[20001], got[STRATUM_LINE_MAX];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        loopback_pair(&lt, &peer);
        peer_write(peer, big);
        peer_write(peer, "\n");
        int ln = lt.recv_line(lt.io, got, sizeof got);
        assert(ln == (int)(sizeof big - 1) && !strcmp(got, big));
        close(peer);
        stratum_tcp_close(&lt);
    }

    {
        stratum_transport lt;
        int               peer;
        static char edge[STRATUM_LINE_MAX], huge[STRATUM_LINE_MAX + 9000], got[STRATUM_LINE_MAX];
        memset(edge, 'e', sizeof edge - 1);
        edge[sizeof edge - 1] = '\0';
        memset(huge, 'h', sizeof huge - 1);
        huge[sizeof huge - 1] = '\0';
        loopback_pair(&lt, &peer);

        g_err_n = 0;
        peer_write(peer, edge);
        peer_write(peer, "\n");
        int ln = lt.recv_line(lt.io, got, sizeof got);
        assert(ln == (int)(sizeof edge - 1) && !strcmp(got, edge));
        assert(g_err_n == 0);

        peer_write(peer, huge);
        peer_write(peer, "h\n{\"id\":7}\n");
        ln = lt.recv_line(lt.io, got, sizeof got);
        assert(ln == (int)strlen("{\"id\":7}") && !strcmp(got, "{\"id\":7}"));
        assert(g_err_n == 1);

        peer_write(peer, edge);
        peer_write(peer, "e");
        peer_write(peer, "\n{\"id\":8}\n");
        ln = lt.recv_line(lt.io, got, sizeof got);
        assert(!strcmp(got, "{\"id\":8}"));
        assert(g_err_n == 2);

        peer_write(peer, huge);
        close(peer);
        assert(lt.recv_line(lt.io, got, sizeof got) == 0);
        assert(g_err_n == 3);
        stratum_tcp_close(&lt);
    }

    {
        stratum_transport lt;
        int               peer;
        pthread_t         th;
        static char       got[STRATUM_LINE_MAX];
        loopback_pair(&lt, &peer);
        g_err_n = 0;
        pthread_create(&th, NULL, skip_flood_sender, &peer);
        assert(lt.recv_line(lt.io, got, sizeof got) == -1);
        assert(g_err_n == 2);
        stratum_tcp_close(&lt);
        pthread_join(th, NULL);
        close(peer);
    }

    {
        stratum_transport lt;
        int               peer, nbefore = g_notify;
        static char       huge[2 * STRATUM_LINE_MAX];
        memset(huge, 'x', sizeof huge - 1);
        huge[sizeof huge - 1] = '\0';
        loopback_pair(&lt, &peer);
        peer_write_handshake(peer, next_sv1_id(), "");
        peer_write(peer, huge);
        peer_write(peer, "\n");
        peer_write(peer,
                "{\"id\":null,\"method\":\"mining.notify\",\"params\":[\"J1\","
                "\"aa11223344556677889900112233445566778899001122334455667788990011\","
                "\"01\",\"02\",[],\"20000000\",\"1d00ffff\",\"5f000000\",false]}\n");
        shutdown(peer, SHUT_WR);

        g_err_n = 0;
        sv1_client lc;
        sv1_init(&lc);
        int lrc = stratum_run(&lc, &lt, "poolworker.001", "x", "AvalonMiner/1.0", on_event, NULL);
        assert(lrc == STRATUM_END_EOF);
        assert(lc.state == SV1_MINING);
        assert(g_notify == nbefore + 1);
        assert(g_err_n == 1);
        sv1_free(&lc);
        close(peer);
        stratum_tcp_close(&lt);
    }

    {
        stratum_transport lt;
        int               peer, nbefore = g_notify;
        loopback_pair(&lt, &peer);
        peer_write(peer, "\n");
        peer_write_handshake(peer, next_sv1_id(), "\n\n");
        peer_write(peer,
                "{\"id\":null,\"method\":\"mining.notify\",\"params\":[\"J1\","
                "\"aa11223344556677889900112233445566778899001122334455667788990011\","
                "\"01\",\"02\",[],\"20000000\",\"1d00ffff\",\"5f000000\",false]}\n");
        peer_write(peer, "\n");
        shutdown(peer, SHUT_WR);

        sv1_client lc;
        sv1_init(&lc);
        int lrc = stratum_run(&lc, &lt, "poolworker.001", "x", "AvalonMiner/1.0", on_event, NULL);
        assert(lrc != STRATUM_END_REFUSED);
        assert(lrc == STRATUM_END_EOF);
        assert(lc.state == SV1_MINING);
        assert(g_notify == nbefore + 1);
        sv1_free(&lc);
        close(peer);
        stratum_tcp_close(&lt);
    }

    {
        stratum_transport lt;
        int               peer;
        loopback_pair(&lt, &peer);
        peer_write(peer, "\n\n\n\n");
        shutdown(peer, SHUT_WR);
        sv1_client lc;
        sv1_init(&lc);
        int lrc = stratum_run(&lc, &lt, "poolworker.001", "x", "AvalonMiner/1.0", on_event, NULL);
        assert(lrc == STRATUM_END_EOF && lc.state == SV1_INIT);
        sv1_free(&lc);
        close(peer);
        stratum_tcp_close(&lt);
    }

    test_tcp_send_concurrent_lines_whole();
    test_tcp_recv_line_deadline();

    load_conn_stats(STRATUM_FIXTURES_DIR "/stratum/happy.jsonl");
    int active = -1, idle = -1;
    for (int i = 0; i < MAX_CONN; i++) {
        if (g_cs[i].rx == 0)
            continue;
        if (g_cs[i].submits > 0 && (active < 0 || g_cs[i].submits > g_cs[active].submits))
            active = i;
        if (g_cs[i].submits == 0 && g_cs[i].rx >= 4 && (idle < 0 || g_cs[i].last < g_cs[idle].last))
            idle = i;
    }
    assert(active >= 0 && idle >= 0);
    assert(g_cs[active].max_rx_gap > 0 && g_cs[active].max_rx_gap < 90.0);
    assert(g_cs[idle].submits == 0);
    assert(g_tmax - g_cs[idle].last > 180.0);
    assert(g_cs[active].last > g_cs[idle].last);

    for (int i = 0; i < g_nlines; i++)
        free(g_lines[i]);
    printf("test_stratum_net: all pass (%d notify; active conn %d max rx gap %.1fs, "
           "no-submit conn %d silent %.1fs)\n",
            g_notify, active, g_cs[active].max_rx_gap, idle, g_tmax - g_cs[idle].last);
    return 0;
}
