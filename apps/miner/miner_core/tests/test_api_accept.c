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
#include "api_sock.h"
#include "mc_api_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "core_ctrl.h"
#include "mc_api_command.h"
#include "mc_api_setter.h"
#include "../../tests/include/tst_check.h"

#undef MC_API_SOCK_PATH
#define MC_API_SOCK_PATH "test_api_accept.sock"
#undef MC_API_TCP_RECV_MS
#define MC_API_TCP_RECV_MS 250
#undef MC_API_TCP_SEND_MS
#define MC_API_TCP_SEND_MS 250
#include "mc_api_server.c"

#define API_PORT    4028
#define CPU_MAX_MS  500                /* over 1 s with every accept() failing; a spin takes 1 s */
#define DEADLINE_MS MC_API_TCP_RECV_MS /* port 4028 request deadline, ms */
#define SLACK_MS    500                /* scheduling margin on top of the deadline, ms */
#define TRICKLE_MS  10                 /* gap between trickled bytes, under MC_API_PARTIAL_MS */
#define LOAD_ROUNDS 50                 /* requests per load client */
#define LOAD_PAIRS  2                  /* load clients per channel */
#define ACK_WAIT_MS 1000

struct load {
    int trusted;
    int bad; /* replies not matching the channel */
};

struct slow {
    char buf[256];
    long ms;
};

osif_ipc_t *g_ipc_core_mm;

static _Atomic int g_stub_runs;
static _Atomic int g_ctrl_waiting;
static char        g_echo_args[256]; /* arguments the echo setting last received */

static void snap(mc_api_stats *out, void *ctx)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
}

static size_t stub_write(const char *req, const mc_api_stats *s, int is_json, char *buf, size_t cap,
        void *ctx)
{
    (void)req;
    (void)s;
    (void)is_json;
    (void)ctx;
    g_stub_runs++;
    return (size_t)snprintf(buf, cap, "STUB trusted=%d", mc_api_peer_trusted());
}

static mc_set_result echo_set(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)op;
    (void)s;
    (void)msgcap;
    (void)ctx;
    (void)msg;
    snprintf(g_echo_args, sizeof g_echo_args, "%s", args);
    return MC_SET_OK;
}

static void *ack_later(void *arg)
{
    mc_ctrl_ack ack = { .result = MC_SET_OK, .seq = (uint32_t)(uintptr_t)arg };

    nanosleep(&(struct timespec){ .tv_sec = ACK_WAIT_MS / 1000,
                      .tv_nsec            = (long)(ACK_WAIT_MS % 1000) * 1000000L },
            NULL);
    core_ctrl_ack(&ack, (int)sizeof ack);
    return NULL;
}

osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    const mc_ctrl_cmd *cmd = data;
    pthread_t          t;

    (void)h;
    (void)len;
    if (type != MSG_CTRL_CMD ||
            pthread_create(&t, NULL, ack_later, (void *)(uintptr_t)cmd->seq) != 0)
        return OSIF_ERR;
    pthread_detach(t);
    return OSIF_OK;
}

static size_t stub_ctrl(const char *req, const mc_api_stats *s, int is_json, char *buf, size_t cap,
        void *ctx)
{
    mc_ctrl_cmd cmd = { .op = CTRL_OP_NONE };
    mc_ctrl_ack ack;

    (void)req;
    (void)s;
    (void)is_json;
    (void)ctx;
    atomic_store(&g_ctrl_waiting, 1);
    int rc = core_ctrl_send(&cmd, &ack);
    atomic_store(&g_ctrl_waiting, 0);
    return (size_t)snprintf(buf, cap, "CTRL rc=%d", rc);
}

static int port_free(void)
{
    struct sockaddr_in a   = { .sin_family = AF_INET, .sin_port = htons(API_PORT) };
    int                one = 1, ok, fd = socket(AF_INET, SOCK_STREAM, 0);

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    ok = bind(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

static int lowest_free_fd(void)
{
    unsigned char  used[256] = { 0 };
    DIR           *d         = opendir("/proc/self/fd");
    struct dirent *e;
    int            n;

    while ((e = readdir(d)) != NULL) {
        n = atoi(e->d_name);
        if (e->d_name[0] != '.' && n != dirfd(d) && n < (int)sizeof used)
            used[n] = 1;
    }
    closedir(d);
    for (n = 0; n < (int)sizeof used && used[n]; n++)
        ;
    return n;
}

static long mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int api_connect(void)
{
    struct sockaddr_in a  = { .sin_family = AF_INET, .sin_port = htons(API_PORT) };
    int                fd = socket(AF_INET, SOCK_STREAM, 0);

    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static int unix_connect(void)
{
    struct sockaddr_un a  = { .sun_family = AF_UNIX };
    int                fd = socket(AF_UNIX, SOCK_STREAM, 0);

    snprintf(a.sun_path, sizeof a.sun_path, "%s", MC_API_SOCK_PATH);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static size_t read_all(int fd, char *buf, size_t cap)
{
    size_t  n = 0;
    ssize_t r;

    while (n + 1 < cap && (r = read(fd, buf + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = 0;
    return n;
}

static size_t ask(int fd, const char *req, char *buf, size_t cap)
{
    size_t n = 0;

    buf[0] = 0;
    if (fd < 0)
        return 0;
    if (write(fd, req, strlen(req)) == (ssize_t)strlen(req))
        n = read_all(fd, buf, cap);
    close(fd);
    return n;
}

static int trickle_until_closed(int fd, const char *head, long limit_ms)
{
    long t0 = mono_ms();
    char b;

    if (send(fd, head, strlen(head), MSG_NOSIGNAL) != (ssize_t)strlen(head))
        return -1;
    while (mono_ms() - t0 < limit_ms) {
        struct pollfd p = { .fd = fd, .events = POLLIN };

        if (poll(&p, 1, TRICKLE_MS) > 0)
            return recv(fd, &b, 1, 0) <= 0 ? 0 : 1;
        if (send(fd, "a", 1, MSG_NOSIGNAL) != 1)
            return 0;
    }
    return -1;
}

static void *load_run(void *arg)
{
    struct load *l = arg;
    char         buf[8192];

    for (int i = 0; i < LOAD_ROUNDS; i++) {
        if (l->trusted) {
            ask(unix_connect(), "stubw|x\n", buf, sizeof buf);
            l->bad += strcmp(buf, "STUB trusted=1") != 0;
            ask(unix_connect(), "{\"command\":\"version\"}", buf, sizeof buf);
            l->bad += strstr(buf, "\"VERSION\"") == NULL;
        } else {
            ask(api_connect(), "stubw|x\n", buf, sizeof buf);
            l->bad += strstr(buf, "Access denied to 'stubw' command") == NULL;
            ask(api_connect(), "{\"command\":\"version\"}", buf, sizeof buf);
            l->bad += strstr(buf, "\"VERSION\"") == NULL;
        }
    }
    return NULL;
}

static void *slow_run(void *arg)
{
    struct slow *w  = arg;
    long         t0 = mono_ms();

    ask(unix_connect(), "stubctrl|x\n", w->buf, sizeof w->buf);
    w->ms = mono_ms() - t0;
    return NULL;
}

static long cpu_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void)
{
    struct rlimit old, low;
    int           c = -1;
    long          t0;

    printf("api server: a failing accept() backs off instead of spinning\n");
    if (!port_free()) {
        printf("  SKIP: port %d is in use on this host\n", API_PORT);
        return TST_SKIP;
    }

    CHECK(mc_api_command_register("stubw", stub_write, NULL) == 0 &&
                    mc_api_command_register("stubctrl", stub_ctrl, NULL) == 0,
            "1.0 write commands registered");
    CHECK(mc_api_setter_register("echo", 0, echo_set, NULL) == 0, "1.0 echo setting registered");
    CHECK(mc_api_server_start(snap, NULL) == 0, "1.1 server thread starts");
    int up = 0;
    for (int i = 0; i < 200 && !up; i++) {
        nanosleep(&(struct timespec){ .tv_nsec = 10000000 }, NULL);
        up = !port_free();
    }
    CHECK(up, "1.2 server listens on %d", API_PORT);

    printf("api server: a request must arrive within %d ms\n", DEADLINE_MS);
    {
        char buf[8192];
        long t0 = mono_ms();
        int  s  = api_connect();
        int  n  = api_connect();

        CHECK(s >= 0 && n >= 0, "2.1 a silent and a normal client connect");
        CHECK(n >= 0 && write(n, "version", 7) == 7, "2.2 the normal client sends at once");
        size_t got    = n >= 0 ? read_all(n, buf, sizeof buf) : 0;
        long   t_norm = mono_ms() - t0;
        size_t silent = s >= 0 ? read_all(s, buf + got, sizeof buf - got) : 1;
        long   t_sil  = mono_ms() - t0;
        CHECK(silent == 0 && t_sil >= DEADLINE_MS - 100 && t_sil < DEADLINE_MS + SLACK_MS,
                "2.3 the silent client is closed at the deadline without a reply (%ld ms)", t_sil);
        CHECK(got > 0 && strstr(buf, "STATUS") && t_norm < DEADLINE_MS + SLACK_MS,
                "2.4 the request queued behind it is answered (%ld ms)", t_norm);
        if (s >= 0)
            close(s);
        if (n >= 0)
            close(n);
    }
    {
        long t0 = mono_ms();
        int  t  = api_connect();
        int  r  = t >= 0 ? trickle_until_closed(t, "ascset|0,wallpaper,png,", DEADLINE_MS + 3000) :
                           -1;
        long dt = mono_ms() - t0;

        CHECK(r == 0 && dt < DEADLINE_MS + SLACK_MS,
                "2.5 a client trickling one byte per %d ms is closed at the deadline "
                "without a reply (%ld ms)",
                TRICKLE_MS, dt);
        if (t >= 0)
            close(t);
    }
    {
        char buf[8192];
        long t0 = mono_ms();
        int  n  = api_connect();

        CHECK(n >= 0 && write(n, "{\"command\":\"version\"}", 21) == 21,
                "2.6 a normal JSON request is sent");
        size_t got = n >= 0 ? read_all(n, buf, sizeof buf) : 0;
        long   dt  = mono_ms() - t0;
        CHECK(got > 0 && strstr(buf, "STATUS") && dt < SLACK_MS,
                "2.7 ...and answered without waiting for the deadline (%ld ms)", dt);
        if (n >= 0)
            close(n);
    }

    printf("api server: port %d only reads\n", API_PORT);
    {
        char buf[8192];

        ask(api_connect(), "stubw|x\n", buf, sizeof buf);
        CHECK(!strcmp(buf,
                      "STATUS=E,When=0,Code=45,Msg=Access denied to 'stubw' command,"
                      "Description=AvalonMiner " AM_VER "|") &&
                        g_stub_runs == 0,
                "3.1 a text write command is refused E/45 and does not run");
        ask(api_connect(), "{\"command\":\"StubW\"}", buf, sizeof buf);
        CHECK(strstr(buf, "\"STATUS\":\"E\"") && strstr(buf, "\"Code\":45") &&
                        strstr(buf, "\"Msg\":\"Access denied to 'stubw' command\"") &&
                        g_stub_runs == 0,
                "3.2 a json write command is refused E/45 and does not run");
        ask(api_connect(), "{\"command\":\"ascset\",\"parameter\":\"0,reboot,0\"}", buf,
                sizeof buf);
        CHECK(strstr(buf, "\"Msg\":\"Access denied to 'ascset' command\""),
                "3.3 a json ascset setting is refused");
        ask(api_connect(), "ascset|0,wallpaper,none\n", buf, sizeof buf);
        CHECK(strstr(buf, "Code=45,Msg=Access denied to 'ascset' command"),
                "3.4 a text ascset setting is refused");
        ask(api_connect(), "ascset|0,help\n", buf, sizeof buf);
        CHECK(strstr(buf, "ASC 0 set info: help") && !strstr(buf, "Access denied"),
                "3.5 ascset help still answers");
        ask(api_connect(), "time|set,t:1787102966,UTC\n", buf, sizeof buf);
        CHECK(strstr(buf, "Code=45,Msg=Access denied to 'time' command"),
                "3.6 time set is refused");
        ask(api_connect(), "{\"command\":\"time\",\"parameter\":\"get\"}", buf, sizeof buf);
        CHECK(strstr(buf, "Time set info: time t:") && !strstr(buf, "Access denied"),
                "3.7 time get still answers");
        ask(api_connect(),
                "{\"command\":\"qrlogin\",\"parameter\":"
                "\"begin,12345678abcdef0123456789abcdef01\"}",
                buf, sizeof buf);
        CHECK(strstr(buf, "\"Msg\":\"Access denied to 'qrlogin' command\"") &&
                        !strstr(buf, "Started"),
                "3.8 qrlogin is refused");
        ask(api_connect(), "check|stubw", buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=N"), "3.9 check reports no access to a write command");
        ask(api_connect(), "summary", buf, sizeof buf);
        CHECK(strstr(buf, "STATUS=S") && strstr(buf, "|SUMMARY,"), "3.10 a read command answers");
        ask(api_connect(), "check|qrlogin", buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=N"), "3.11 check reports qrlogin, without access");
        ask(api_connect(), "time", buf, sizeof buf);
        CHECK(strstr(buf, "STATUS=I") && strstr(buf, "Code=130"),
                "3.12 time without a parameter answers I/130 (%s)", buf);
        ask(api_connect(), "time|sync\n", buf, sizeof buf);
        CHECK(strstr(buf, "STATUS=I") && strstr(buf, "Code=131") &&
                        strstr(buf, "time unknown argument"),
                "3.13 time with an unknown parameter answers I/131 (%s)", buf);
    }

    printf("api server: %s is trusted\n", MC_API_SOCK_PATH);
    {
        char        buf[8192];
        struct stat st;

        CHECK(stat(MC_API_SOCK_PATH, &st) == 0 && S_ISSOCK(st.st_mode) &&
                        (st.st_mode & 0777) == 0600,
                "4.1 the trusted socket exists with mode 0600");
        ask(unix_connect(), "stubw|x\n", buf, sizeof buf);
        CHECK(!strcmp(buf, "STUB trusted=1") && g_stub_runs == 1,
                "4.2 a text write command runs, on the trusted channel");
        ask(unix_connect(), "{\"command\":\"stubw\"}", buf, sizeof buf);
        CHECK(!strcmp(buf, "STUB trusted=1") && g_stub_runs == 2, "4.3 a json write command runs");
        ask(unix_connect(),
                "{\"command\":\"qrlogin\",\"parameter\":"
                "\"begin,12345678abcdef0123456789abcdef01\"}",
                buf, sizeof buf);
        CHECK(strstr(buf, "\"Started\":1"), "4.4 qrlogin answers");
        ask(unix_connect(), "check|stubw", buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=Y"), "4.5 check reports access to a write command");
        ask(unix_connect(), "{\"command\":\"version\"}", buf, sizeof buf);
        CHECK(strstr(buf, "\"VERSION\""), "4.6 a read command answers");
        ask(api_connect(), "stubw|x\n", buf, sizeof buf);
        CHECK(strstr(buf, "Access denied") && g_stub_runs == 2,
                "4.7 port %d stays read-only after a trusted request", API_PORT);
        ask(unix_connect(), "check|qrlogin", buf, sizeof buf);
        CHECK(strstr(buf, "Exists=Y,Access=Y"), "4.8 check reports qrlogin, with access");
        ask(unix_connect(), "{\"command\":\"check\",\"parameter\":\"qrlogin\"}", buf, sizeof buf);
        CHECK(strstr(buf, "\"Exists\":\"Y\",\"Access\":\"Y\""), "4.9 ...in json too");
        ask(unix_connect(),
                "{\"command\":\"ascset\",\"parameter\":"
                "\"0,echo,a\\\\b\\\"c\\nd\\u00e9\\u4e2d,e\"}",
                buf, sizeof buf);
        CHECK(strstr(buf, "\"STATUS\":\"S\"") &&
                        !strcmp(g_echo_args, "a\\b\"c\nd\xc3\xa9\xe4\xb8\xad,e"),
                "4.10 a json-escaped ascset parameter reaches the setting decoded (%s)", buf);
        g_echo_args[0] = 0;
        ask(unix_connect(), "ascset|0,echo,a\\b\"c\n", buf, sizeof buf);
        CHECK(strstr(buf, "STATUS=S") && !strcmp(g_echo_args, "a\\b\"c"),
                "4.11 a text ascset parameter reaches the setting as sent (%s)", g_echo_args);
    }

    printf("api server: a silent client on port %d does not hold up %s\n", API_PORT,
            MC_API_SOCK_PATH);
    {
        char buf[8192], b;
        int  s = api_connect();

        nanosleep(&(struct timespec){ .tv_nsec = 20000000 }, NULL);
        long t0 = mono_ms();
        ask(unix_connect(), "{\"command\":\"version\"}", buf, sizeof buf);
        long dt    = mono_ms() - t0;
        int  alive = s >= 0 && recv(s, &b, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN;
        CHECK(strstr(buf, "\"VERSION\"") && dt < DEADLINE_MS,
                "5.1 a trusted request is answered before the silent client times out (%ld ms)",
                dt);
        CHECK(alive, "5.2 ...while the silent port %d client is still connected", API_PORT);
        size_t got = s >= 0 ? read_all(s, buf, sizeof buf) : 1;
        long   ts  = mono_ms() - t0;
        CHECK(got == 0 && ts < DEADLINE_MS + SLACK_MS,
                "5.3 the silent client is closed within the %d ms deadline without a reply "
                "(%ld ms)",
                DEADLINE_MS, ts);
        if (s >= 0)
            close(s);
    }

    printf("api server: both channels under concurrent load\n");
    {
        struct load l[2 * LOAD_PAIRS];
        pthread_t   th[2 * LOAD_PAIRS];
        int         runs = g_stub_runs, bad = 0, up = 0;

        for (int i = 0; i < 2 * LOAD_PAIRS; i++) {
            l[i] = (struct load){ .trusted = i & 1 };
            if (pthread_create(&th[up], NULL, load_run, &l[i]) == 0)
                up++;
        }
        for (int i = 0; i < up; i++)
            pthread_join(th[i], NULL);
        for (int i = 0; i < 2 * LOAD_PAIRS; i++)
            bad += l[i].bad;
        CHECK(up == 2 * LOAD_PAIRS, "6.1 %d load clients start", 2 * LOAD_PAIRS);
        CHECK(bad == 0, "6.2 every reply matches its channel (%d wrong)", bad);
        CHECK(g_stub_runs - runs == LOAD_PAIRS * LOAD_ROUNDS,
                "6.3 only the trusted write requests ran (%d)", g_stub_runs - runs);
    }

    printf("api server: a trusted write waiting %d ms for mm_miner does not hold up port %d\n",
            ACK_WAIT_MS, API_PORT);
    {
        struct slow w = { .ms = -1 };
        pthread_t   th;
        char        buf[8192];
        int         started = pthread_create(&th, NULL, slow_run, &w) == 0;

        for (int i = 0; i < 100 && started && !atomic_load(&g_ctrl_waiting); i++)
            nanosleep(&(struct timespec){ .tv_nsec = 10000000 }, NULL);
        CHECK(started && atomic_load(&g_ctrl_waiting),
                "7.1 the trusted write waits for its control answer");
        long t0 = mono_ms();
        ask(api_connect(), "{\"command\":\"version\"}", buf, sizeof buf);
        long dt = mono_ms() - t0;
        CHECK(strstr(buf, "\"VERSION\"") && dt < ACK_WAIT_MS / 2,
                "7.2 port %d version is answered while the trusted write waits (%ld ms)", API_PORT,
                dt);
        t0 = mono_ms();
        ask(api_connect(), "summary", buf, sizeof buf);
        dt = mono_ms() - t0;
        CHECK(strstr(buf, "|SUMMARY,") && dt < ACK_WAIT_MS / 2,
                "7.3 port %d summary is answered while the trusted write waits (%ld ms)", API_PORT,
                dt);
        CHECK(atomic_load(&g_ctrl_waiting) && atomic_load(&g_mc_api_busy_ms[1]) != 0 &&
                        atomic_load(&g_mc_api_busy_ms[0]) == 0,
                "7.4 ...while the trusted write still waits and stays marked busy");
        if (started)
            pthread_join(th, NULL);
        CHECK(!strcmp(w.buf, "CTRL rc=0") && w.ms >= ACK_WAIT_MS - 50,
                "7.5 the trusted write is answered once mm_miner answers (%s, %ld ms)", w.buf,
                w.ms);
        CHECK(atomic_load(&g_mc_api_busy_ms[1]) == 0, "7.6 ...and the trusted channel is idle");
    }

    /*
     * The server sits in accept() holding the lowest free fd number. Cap the
     * limit at that number: the client connection is served on it, and once
     * the server closes it no fd number is left for the next accept().
     */
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(API_PORT) };
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    c                    = socket(AF_INET, SOCK_STREAM, 0);
    getrlimit(RLIMIT_NOFILE, &old);
    low          = old;
    low.rlim_cur = (rlim_t)lowest_free_fd();
    CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0, "1.3 fd limit lowered to %d", (int)low.rlim_cur);
    CHECK(c >= 0 && connect(c, (struct sockaddr *)&a, sizeof a) == 0 && write(c, "version", 7) == 7,
            "1.4 one request is sent");
    nanosleep(&(struct timespec){ .tv_nsec = 200000000 }, NULL);

    t0 = cpu_ms();
    sleep(1);
    long used = cpu_ms() - t0;
    CHECK(used < CPU_MAX_MS, "1.5 1 s of failing accept() costs little CPU (%ld ms)", used);

    setrlimit(RLIMIT_NOFILE, &old);
    if (c >= 0)
        close(c);
    unlink(MC_API_SOCK_PATH);
    return tst_done();
}
