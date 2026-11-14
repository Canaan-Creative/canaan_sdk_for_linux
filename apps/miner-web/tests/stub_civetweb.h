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

#ifndef STUB_CIVETWEB_H
#define STUB_CIVETWEB_H

#include <stddef.h>

struct mg_connection;

/* Scripted HTTP request fed to a registered handler */
typedef struct {
    const char *method;
    const char *uri;
    const char *query;
    const char *content_type;   /* Content-Type header value, NULL when absent */
    const char *cookie;         /* Cookie header value, NULL when absent */
    const char *authorization;  /* Authorization header value, NULL when absent */
    const char *token;          /* Token header value, NULL when absent */
    const char *host;           /* Host header value, NULL when absent */
    const char *origin;         /* Origin header value, NULL when absent */
    const char *referer;        /* Referer header value, NULL when absent */
    long long   content_length; /* Content-Length reported in the request info */
    const char *filename;       /* upload file name passed to the form field_found callback */
    const char *body;           /* request body bytes, NULL makes reads return 'x' filler */
    size_t      body_len;
    size_t      unread;     /* bytes the handler can read from the body */
    size_t      read_chunk; /* max bytes per mg_read call, 0 = no limit */
    int         form_rc;    /* return value of mg_handle_form_request */
    int         is_ssl;     /* 1 for a request that arrived over HTTPS */
    int         server_port;
    const char *remote_addr; /* client address, NULL for 127.0.0.1 */
} stub_req;

typedef struct {
    int    status;    /* HTTP status parsed from the status line, 0 when none sent */
    char   head[512]; /* last mg_printf output carrying a status line, truncated to fit */
    char   body[512]; /* bytes written by mg_write, NUL-terminated, truncated to fit */
    size_t body_len;
    int    closed; /* number of mg_close_connection calls */
    int    form_calls;
    int    reads;
    size_t drained; /* total body bytes consumed by mg_read */
} stub_rsp;

void        stub_reset(void);
int         stub_handler_count(void);
const char *stub_handler_uri(int i);
int         stub_invoke(const char *uri, const stub_req *req, stub_rsp *rsp);
int         stub_call(int (*fn)(struct mg_connection *), const stub_req *req, stub_rsp *rsp);
const char *stub_route(const char *uri);

#endif /* STUB_CIVETWEB_H */
