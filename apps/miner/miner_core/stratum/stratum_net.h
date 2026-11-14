/* SPDX-License-Identifier: MIT */
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

#ifndef STRATUM_NET_H
#define STRATUM_NET_H

#include "sv1.h"

#define STRATUM_LINE_MAX       32768         /* line buffer size, bytes */
#define STRATUM_SKIP_MAX       (1024 * 1024) /* most bytes without a line break, bytes */
#define STRATUM_RECV_TIMEOUT_S 90
#define STRATUM_SEND_TIMEOUT_S 30

#define STRATUM_CONNECT_ADDR_MS  10000 /* connect timeout per address, ms */
#define STRATUM_CONNECT_TOTAL_MS 30000 /* connect timeout over all addresses, ms */

#define STRATUM_RESOLVE_TIMEOUT_S    2
#define STRATUM_RESOLVE_ATTEMPTS     2 /* resolver attempts per nameserver, count */
#define STRATUM_RESOLVE_NS_MAX       3 /* nameservers used, count */
#define STRATUM_RESOLVE_QUERY_ROUNDS 2 /* resolver query rounds per lookup, count */
/* worst-case name resolution time, ms */
#define STRATUM_RESOLVE_TOTAL_MS \
    (STRATUM_RESOLVE_TIMEOUT_S * STRATUM_RESOLVE_ATTEMPTS * STRATUM_RESOLVE_NS_MAX * \
            STRATUM_RESOLVE_QUERY_ROUNDS * 1000)

#define STRATUM_RESOLVE_STR_(x) #x
#define STRATUM_RESOLVE_STR(x)  STRATUM_RESOLVE_STR_(x) /* stringify expanded x */
/* RES_OPTIONS value bounding the resolver */
#define STRATUM_RESOLVE_RES_OPTIONS \
    "timeout:" STRATUM_RESOLVE_STR(STRATUM_RESOLVE_TIMEOUT_S) " attempts:" STRATUM_RESOLVE_STR( \
            STRATUM_RESOLVE_ATTEMPTS)

/*
 * Line transport. recv_line returns the length without newline, 0 on EOF,
 * -2 on read timeout, -1 on other errors, and never a zero-length line.
 * send_line returns 0 on success.
 */
typedef struct {
    int (*send_line)(void *io, const char *line);
    int (*recv_line)(void *io, char *buf, size_t cap);
    void *io;
} stratum_transport;

/* Why a session ended: 0 = EOF, 1 = stopped by the caller, < 0 = failure. */
enum {
    STRATUM_END_EOF     = 0,  /* pool closed the session (clean FIN)        */
    STRATUM_END_STOPPED = 1,  /* on_event asked the loop to stop            */
    STRATUM_END_ERROR   = -1, /* transport error (not a timeout)            */
    STRATUM_END_TIMEOUT = -2, /* no usable line within the recv timeout     */
    STRATUM_END_SEND    = -3, /* an outbound line failed to send            */
    STRATUM_END_REFUSED = -4, /* pool refused or garbled the handshake      */
};

enum {
    STRATUM_CONNECT_OK      = 0,
    STRATUM_CONNECT_ERR     = -1, /* socket or connect error */
    STRATUM_CONNECT_RESOLVE = -2, /* host name not resolved */
    STRATUM_CONNECT_TIMEOUT = -3,
};

/* Line trace hook; tx is 1 for a sent line, 0 for a received one. */
typedef void (*stratum_trace_fn)(int tx, const char *line);

/* Per-event callback; a non-zero return stops the session. */
typedef int (*stratum_on_event)(void *ctx, sv1_client *c, const sv1_event *ev);

const char *stratum_end_str(int reason);
int         stratum_session_established(const sv1_client *c);
void        stratum_set_trace(stratum_trace_fn fn);
void        stratum_trace_line(int tx, const char *line);
int         stratum_run(sv1_client *c, stratum_transport *t, const char *user, const char *pass,
                const char *user_agent, stratum_on_event on_event, void *ctx);
int         stratum_parse_url(const char *url, char *host, size_t host_cap, int *port);
int         stratum_tcp_connect(stratum_transport *t, const char *host, int port);
void        stratum_tcp_close(stratum_transport *t);
void        stratum_tcp_shutdown(stratum_transport *t);

#endif /* STRATUM_NET_H */
