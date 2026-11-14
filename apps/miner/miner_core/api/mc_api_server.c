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

#define _POSIX_C_SOURCE 200809L
#include "mc_api_server.h"
#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "mc_api_lcd.h"
#include "mc_api_command.h"
#include "mc_api_setter.h"
#include "mc_api_qrlogin.h"
#include "mc_api_time.h"
#include "api_sock.h"
#include "jwrite.h"
#include "osif.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <time.h>

#define MC_API_PORT             4028
#define MC_API_ACCEPT_BACKOFF_S 1

#define MC_API_REQ_CAP    4096 /* request buffer, bytes */
#define MC_API_PARTIAL_MS 40   /* wait for the rest of a partial request, ms */

#define JOIN_JSON_END ",\"id\":1}"

enum { JOIN_UNKNOWN, JOIN_OK, JOIN_DENIED };

typedef struct {
    int trusted;
    int recv_ms; /* receive a whole request, ms */
    int send_ms; /* send a whole reply, ms */
} api_chan;

_Atomic uint64_t g_mc_api_busy_ms[MC_API_CHAN_N];

static mc_api_snapshot_fn g_snap;
static void              *g_snap_ctx;
static api_chan           g_chan[MC_API_CHAN_N] = {
    { 0, MC_API_TCP_RECV_MS, MC_API_TCP_SEND_MS },
    { 1, MC_API_IO_DEADLINE_MS, MC_API_IO_DEADLINE_MS },
};

static int is_http_request(const char *p)
{
    static const char *const methods[] = {
        "GET ",
        "POST ",
        "PUT ",
        "HEAD ",
        "OPTIONS ",
        "DELETE ",
        "PATCH ",
        "CONNECT ",
        "TRACE ",
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++)
        if (!strncmp(p, methods[i], strlen(methods[i])))
            return 1;
    return 0;
}

size_t mc_api_parse_cmd(const char *req, char *cmd, size_t cap)
{
    const char *body = req + strspn(req, " \t\r\n");
    const char *p    = body;
    size_t      n    = 0;
    cmd[0]           = 0;
    if (is_http_request(body))
        return 0;
    if (*body == '{') {
        const char *j = strstr(body, "\"command\"");
        if (!j)
            return 0;
        p = strchr(j, ':');
        if (!p)
            return 0;
        p = strchr(p, '"');
        if (!p)
            return 0;
        p++;
    }
    while (*p && *p != '"' && *p != '|' && *p != '\n' && *p != '\r' && n + 1 < cap)
        cmd[n++] = (char)tolower((unsigned char)*p++);
    cmd[n] = 0;
    if (*p && *p != '"' && *p != '|' && *p != '\n' && *p != '\r') {
        cmd[0] = 0;
        return 0;
    }
    return n;
}

static size_t err_reply(const mc_api_stats *s, const char *msg, char *buf, size_t cap)
{
    char status[256];
    snprintf(status, sizeof status, "[{\"STATUS\":\"E\",\"When\":%lu,\"Code\":14,\"Msg\":\"%s\"}]",
            (unsigned long)s->when, msg);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

static size_t set_reply(const mc_api_stats *s, mc_set_result r, int code, const char *msg,
        char *buf, size_t cap)
{
    char status[MC_API_SET_MSG_CAP + 128 + MINER_VERSION_CAP];
    snprintf(status, sizeof status,
            "[{\"STATUS\":\"%c\",\"When\":%lu,\"Code\":%d,\"Msg\":\"%s\","
            "\"Description\":\"AvalonMiner %s\"}]",
            mc_set_letter(r), (unsigned long)s->when, code, msg, AM_VER);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

static size_t dispatch_one(const char *req, const char *cmd, const mc_api_stats *s, char *buf,
        size_t cap)
{
    char canon[MC_API_CMD_CANON_CAP];
    cmd = mc_api_cmd_canon(cmd, canon, sizeof canon);
    if (!strcmp(cmd, "version"))
        return mc_api_version(s, buf, cap);
    if (!strcmp(cmd, "summary"))
        return mc_api_summary(s, buf, cap);
    if (!strcmp(cmd, "devs") || !strcmp(cmd, "edevs"))
        return mc_api_devs(s, buf, cap);
    if (!strcmp(cmd, "pools"))
        return mc_api_pools(s, buf, cap);
    if (!strcmp(cmd, "coin"))
        return mc_api_coin(s, buf, cap);
    if (!strcmp(cmd, "config"))
        return mc_api_config(s, buf, cap);
    if (!strcmp(cmd, "devdetails"))
        return mc_api_devdetails(s, buf, cap);
    if (!strcmp(cmd, "lcd"))
        return mc_api_lcd(s, buf, cap);
    if (!strcmp(cmd, "check"))
        return mc_api_cmd_check(req, s, buf, cap);
    if (!strcmp(cmd, "litestats"))
        return mc_api_litestats_(s, buf, cap);
    if (!strcmp(cmd, "stats") || !strcmp(cmd, "estats"))
        return mc_api_stats_(s, buf, cap, mc_api_estats_all(req), !strcmp(cmd, "stats"));
    if (!strcmp(cmd, "lockstats"))
        return mc_api_lockstats(1, s, buf, cap);
    if (!strcmp(cmd, "qrlogin")) {
        if (!mc_api_peer_trusted())
            return err_reply(s, "Invalid command", buf, cap);
        const char *p = strstr(req, "\"parameter\"");
        p             = p ? strchr(p, ':') : NULL;
        p             = p ? strchr(p, '"') : NULL;
        if (!p)
            return err_reply(s, "Invalid parameter", buf, cap);
        p++;
        const char *end = strchr(p, '"');
        if (!end || end - p != 38)
            return err_reply(s, "Invalid parameter", buf, cap);
        char token[33];
        memcpy(token, p + 6, 32);
        token[32]    = 0;
        int approved = 0, started = 0;
        if (!strncmp(p, "begin,", 6))
            started = mc_api_qr_login_begin(token);
        else if (!strncmp(p, "claim,", 6))
            approved = mc_api_qr_login_take(token);
        else
            return err_reply(s, "Invalid parameter", buf, cap);
        char status[256];
        snprintf(status, sizeof status,
                "[{\"STATUS\":\"S\",\"When\":%lu,\"Code\":70,\"Msg\":\"QR login\","
                "\"Description\":\"AvalonMiner %s\"}]",
                (unsigned long)s->when, AM_VER);
        jwriter w;
        jw_init(&w, buf, cap);
        jw_obj_open(&w);
        jw_kv_raw(&w, "STATUS", status);
        jw_kv_raw(&w, "Started", started ? "1" : "0");
        jw_kv_raw(&w, "QRLOGIN", approved ? "[{\"Approved\":1}]" : "[{\"Approved\":0}]");
        jw_kv_raw(&w, "id", "1");
        jw_obj_close(&w);
        return jw_len(&w);
    }
    if (!strcmp(cmd, "ascset")) {
        char          msg[MC_API_SET_MSG_CAP];
        mc_set_result r = mc_api_setter_run(req, s, msg, sizeof msg);
        return set_reply(s, r, mc_set_code(r), msg, buf, cap);
    }
    if (!strcmp(cmd, "time")) {
        char          msg[128];
        int           code;
        mc_set_result r = mc_api_time_run(req, msg, sizeof msg, &code);
        return set_reply(s, r, code, msg, buf, cap);
    }
    void         *cctx = NULL;
    mc_api_cmd_fn f    = mc_api_command_lookup(cmd, &cctx);
    if (f)
        return mc_api_command_run(f, cmd, req, s, 1, buf, cap, cctx);
    return err_reply(s, "Invalid command", buf, cap);
}

static int join_kind(const char *cmd)
{
    static const char *const joinable[] = {
        "version",
        "config",
        "devs",
        "edevs",
        "pools",
        "summary",
        "devdetails",
        "stats",
        "estats",
        "litestats",
        "coin",
        "lcd",
        "lockstats",
    };
    static const char *const unjoinable[] = { "ascset", "time", "check" };

    for (size_t i = 0; i < sizeof joinable / sizeof joinable[0]; i++)
        if (!strcmp(cmd, joinable[i]))
            return JOIN_OK;
    for (size_t i = 0; i < sizeof unjoinable / sizeof unjoinable[0]; i++)
        if (!strcmp(cmd, unjoinable[i]))
            return JOIN_DENIED;
    return mc_api_command_lookup(cmd, NULL) ? JOIN_DENIED : JOIN_UNKNOWN;
}

static int join_seen(const char *cmd, const char *p, size_t len)
{
    for (const char *q = cmd; q < p;) {
        const char *plus = strchr(q, '+');
        size_t      qlen = plus ? (size_t)(plus - q) : strlen(q);
        if (qlen == len && !strncasecmp(q, p, len))
            return 1;
        if (!plus)
            break;
        q = plus + 1;
    }
    return 0;
}

static void join_name(const char *name, int is_json, char *out, size_t cap)
{
    size_t n = 0;
    for (; *name && n + 3 <= cap; name++) {
        if (*name == '\\' || (is_json ? *name == '"' : (*name == ',' || *name == '=')))
            out[n++] = '\\';
        out[n++] = *name;
    }
    out[n] = '\0';
}

static size_t denied_reply(const char *cmd, int is_json, const mc_api_stats *s, char *buf,
        size_t cap)
{
    char canon[MC_API_CMD_CANON_CAP], msg[MC_API_CMD_CANON_CAP + 32];
    snprintf(msg, sizeof msg, "Access denied to '%s' command",
            mc_api_cmd_canon(cmd, canon, sizeof canon));
    return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 45, msg);
}

static size_t join_one(const char *sub, int kind, int is_json, const mc_api_stats *s, char *buf,
        size_t cap)
{
    if (kind == JOIN_OK)
        return is_json ? dispatch_one(sub, sub, s, buf, cap) :
                         mc_api_dispatch_text(sub, sub, s, buf, cap);
    if (kind == JOIN_DENIED)
        return denied_reply(sub, is_json, s, buf, cap);
    if (is_json)
        return err_reply(s, "Invalid command", buf, cap);
    return (size_t)snprintf(buf, cap, "STATUS=E,When=%lu,Code=14,Msg=Invalid command|",
            (unsigned long)s->when);
}

size_t mc_api_dispatch_join(const char *cmd, int is_json, const mc_api_stats *s, char *buf,
        size_t cap)
{
    const char *tail = is_json ? JOIN_JSON_END : "";
    if (cap <= strlen(tail))
        return 0;
    size_t lim = cap - strlen(tail), off = 0;
    int    first = 1, full = 0;
    buf[0] = '\0';
    for (const char *p = cmd;;) {
        const char *plus = strchr(p, '+');
        size_t      len  = plus ? (size_t)(plus - p) : strlen(p);
        char        sub[MC_API_CMD_CANON_CAP], name[2 * MC_API_CMD_CANON_CAP];
        snprintf(sub, sizeof sub, "%.*s", (int)len, p);
        mc_api_cmd_canon(sub, sub, sizeof sub);
        int kind = join_kind(sub);
        if (len > 0 && !(kind != JOIN_UNKNOWN && join_seen(cmd, p, len))) {
            join_name(sub, is_json, name, sizeof name);
            int n = is_json ? snprintf(buf + off, lim - off, "%s\"%s\":", first ? "{" : ",", name) :
                              snprintf(buf + off, lim - off, "CMD=%s|", name);
            if (n < 0 || (size_t)n >= lim - off) {
                full = 1;
                break;
            }
            off += (size_t)n;
            off += join_one(sub, kind, is_json, s, buf + off, lim - off);
            first = 0;
            if (off >= lim) {
                off  = lim - 1;
                full = 1;
                break;
            }
        }
        if (!plus)
            break;
        p = plus + 1;
    }
    memcpy(buf + off, tail, strlen(tail) + 1);
    return full ? cap : off + strlen(tail);
}

size_t mc_api_dispatch(const char *req, const char *cmd, const mc_api_stats *s, char *buf,
        size_t cap)
{
    if (strchr(cmd, '+'))
        return mc_api_dispatch_join(cmd, 1, s, buf, cap);
    return dispatch_one(req, cmd, s, buf, cap);
}

static int req_complete(const char *req, size_t len)
{
    const char *p = req;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '{') {
        int depth = 0, seen = 0, in_str = 0;
        for (size_t i = 0; i < len; i++) {
            if (in_str) {
                if (req[i] == '\\')
                    i++;
                else if (req[i] == '"')
                    in_str = 0;
            } else if (req[i] == '"')
                in_str = 1;
            else if (req[i] == '{') {
                depth++;
                seen = 1;
            } else if (req[i] == '}') {
                if (--depth <= 0 && seen)
                    return 1;
            }
        }
        return 0;
    }
    if (memchr(req, '\n', len))
        return 1;
    return strncmp(p, "ascset", 6) != 0;
}

static uint64_t api_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int api_poll(int c, short events, uint64_t end, int cap_ms)
{
    uint64_t now = api_now_ms();
    if (now >= end)
        return 0;
    int ms = (int)(end - now);
    if (cap_ms >= 0 && ms > cap_ms)
        ms = cap_ms;
    struct pollfd pfd = { .fd = c, .events = events };
    return poll(&pfd, 1, ms);
}

static size_t api_read_request(int c, char *req, size_t cap, int *truncated, const api_chan *ch)
{
    uint64_t end     = api_now_ms() + (uint64_t)ch->recv_ms;
    int      wait_ms = -1;
    size_t   total   = 0;
    *truncated       = 0;
    for (;;) {
        int rc = api_poll(c, POLLIN, end, wait_ms);
        if (rc == 0 && api_now_ms() >= end) {
            if (ch->trusted)
                osif_log(OSIF_LOG_WARN,
                        "trusted API request not received within %d ms; connection closed",
                        ch->recv_ms);
            else
                osif_log(OSIF_LOG_DBG, "4028 request not received within %d ms; connection closed",
                        ch->recv_ms);
            return 0;
        }
        if (rc <= 0)
            break;
        ssize_t r = read(c, req + total, cap - 1 - total);
        if (r <= 0)
            break;
        total += (size_t)r;
        req[total] = 0;
        if (total >= cap - 1) {
            osif_log(OSIF_LOG_WARN, "API request of %zu bytes or more refused", cap - 1);
            *truncated = 1;
            break;
        }
        if (req_complete(req, total))
            break;
        wait_ms = MC_API_PARTIAL_MS;
    }
    return total;
}

static void api_write_reply(int c, const char *buf, size_t len, int deadline_ms)
{
    uint64_t end = api_now_ms() + (uint64_t)deadline_ms;
    for (size_t off = 0; off < len;) {
        ssize_t w = send(c, buf + off, len - off, MSG_DONTWAIT);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return;
        if (api_poll(c, POLLOUT, end, -1) <= 0)
            return;
    }
}

static size_t truncated_reply(int is_json, const mc_api_stats *s, char *buf, size_t cap)
{
    return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 14,
            "Request too long: truncated and refused");
}

static size_t reply_fit(size_t len, const char *cmd, int is_json, const mc_api_stats *s, char *buf,
        size_t cap)
{
    if (len < cap)
        return len;
    osif_log(OSIF_LOG_ERR, "API reply to %s is %zu bytes or more, at most %zu fit: not sent", cmd,
            len, cap - 1);
    return mc_api_status_reply(is_json, buf, cap, s->when, 'E', 14, "Reply too long: not sent");
}

static int api_listen_tcp(void)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        osif_log(OSIF_LOG_ERR, "API server (port %d) not started: %s", MC_API_PORT,
                strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons(MC_API_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) {
        osif_log(OSIF_LOG_ERR, "API server cannot use port %d: %s", MC_API_PORT, strerror(errno));
        close(srv);
        return -1;
    }
    if (listen(srv, 8) < 0) {
        osif_log(OSIF_LOG_ERR, "API server cannot listen on port %d: %s", MC_API_PORT,
                strerror(errno));
        close(srv);
        return -1;
    }
    osif_log(OSIF_LOG_INFO, "API server listening on port %d", MC_API_PORT);
    return srv;
}

static int api_listen_unix(void)
{
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        osif_log(OSIF_LOG_ERR, "API server (trusted socket) not started: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", MC_API_SOCK_PATH);
    unlink(MC_API_SOCK_PATH);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || chmod(MC_API_SOCK_PATH, 0600) < 0 ||
            listen(srv, 8) < 0) {
        osif_log(OSIF_LOG_ERR, "API server cannot listen on %s: %s", MC_API_SOCK_PATH,
                strerror(errno));
        close(srv);
        return -1;
    }
    osif_log(OSIF_LOG_INFO, "API server listening on %s", MC_API_SOCK_PATH);
    return srv;
}

static size_t api_reply(const char *req, const api_chan *ch, int truncated, char *reply, size_t cap)
{
    char              cmd[MC_API_CMD_CAP];
    mc_api_stats      s;
    size_t            len;
    int               is_json = (req[strspn(req, " \t\r\n")] == '{');
    _Atomic uint64_t *busy    = &g_mc_api_busy_ms[ch - g_chan];

    mc_api_parse_cmd(req, cmd, sizeof cmd);
    atomic_store(busy, api_now_ms());
    mc_api_set_peer_trusted(ch->trusted);
    memset(&s, 0, sizeof s);
    if (g_snap)
        g_snap(&s, g_snap_ctx);
    if (truncated)
        len = truncated_reply(is_json, &s, reply, cap);
    else if (!ch->trusted && !strchr(cmd, '+') && mc_api_cmd_writes(req, cmd))
        len = denied_reply(cmd, is_json, &s, reply, cap);
    else if (is_json)
        len = mc_api_dispatch(req, cmd, &s, reply, cap);
    else if (strchr(cmd, '+'))
        len = mc_api_dispatch_join(cmd, 0, &s, reply, cap);
    else
        len = mc_api_dispatch_text(req, cmd, &s, reply, cap);
    len        = reply_fit(len, cmd, is_json, &s, reply, cap);
    reply[len] = '\0';
    atomic_store(busy, 0);
    return len;
}

static void api_serve(int c, const api_chan *ch)
{
    char req[MC_API_REQ_CAP];
    int  truncated = 0;

    if (api_read_request(c, req, sizeof req, &truncated, ch) > 0) {
        char *reply = malloc(MC_API_REPLY_CAP);
        if (reply) {
            size_t len = api_reply(req, ch, truncated, reply, MC_API_REPLY_CAP);
            api_write_reply(c, reply, len + 1, ch->send_ms);
            free(reply);
        }
    }
    close(c);
}

static void *api_thread(void *arg)
{
    const api_chan *ch  = arg;
    int             srv = ch->trusted ? api_listen_unix() : api_listen_tcp();

    if (srv < 0)
        return NULL;
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) {
            /* EMFILE/ENFILE persist until an fd frees: wait, do not spin */
            if (errno != EINTR) {
                osif_log(OSIF_LOG_ERR, "API connection failed (%s); retrying", strerror(errno));
                sleep(MC_API_ACCEPT_BACKOFF_S);
            }
            continue;
        }
        api_serve(c, ch);
    }
    return NULL;
}

int mc_api_server_start(mc_api_snapshot_fn snap, void *ctx)
{
    int ret = 0;

    g_snap     = snap;
    g_snap_ctx = ctx;
    for (size_t i = 0; i < MC_API_CHAN_N; i++) {
        pthread_t t;
        int       rc = pthread_create(&t, NULL, api_thread, &g_chan[i]);
        if (rc) {
            if (g_chan[i].trusted)
                osif_log(OSIF_LOG_ERR, "API server (trusted socket) not started: %s", strerror(rc));
            else
                osif_log(OSIF_LOG_ERR, "API server (port %d) not started: %s", MC_API_PORT,
                        strerror(rc));
            ret = -1;
            continue;
        }
        pthread_detach(t);
    }
    return ret;
}
