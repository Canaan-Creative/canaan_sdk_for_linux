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

#include "stub_civetweb.h"
#include "web_logfmt.h"

#include <civetweb.h>
#include <ctype.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define STUB_MAX_HANDLERS 16

/* Opaque server context, never dereferenced */
struct mg_context {
    int unused;
};

struct mg_connection {
    struct mg_request_info ri; /* request info returned by mg_get_request_info */
    const stub_req        *req;
    stub_rsp              *rsp;
    size_t                 unread; /* body bytes still readable by mg_read */
};

/* Registered request handlers, in registration order */
static struct {
    const char        *uri; /* URI matched exactly by stub_invoke */
    mg_request_handler fn;
    void              *cbdata; /* opaque argument passed to fn */
} g_handlers[STUB_MAX_HANDLERS];

static int g_nhandlers;

void stub_reset(void)
{
    memset(g_handlers, 0, sizeof g_handlers);
    g_nhandlers = 0;
}

int stub_handler_count(void)
{
    return g_nhandlers;
}

const char *stub_handler_uri(int i)
{
    return (i >= 0 && i < g_nhandlers) ? g_handlers[i].uri : NULL;
}

static ptrdiff_t match_prefix(const char *pattern, size_t pattern_len, const char *str)
{
    const char *or_str = memchr(pattern, '|', pattern_len);
    ptrdiff_t   i, j, len, res;

    if (or_str) {
        res = match_prefix(pattern, (size_t)(or_str - pattern), str);
        return res > 0 ?
                res :
                match_prefix(or_str + 1, (size_t)(pattern + pattern_len - or_str - 1), str);
    }
    for (i = 0, j = 0; i < (ptrdiff_t)pattern_len; i++, j++) {
        if (pattern[i] == '?' && str[j] != '\0') {
            continue;
        } else if (pattern[i] == '$') {
            return str[j] == '\0' ? j : -1;
        } else if (pattern[i] == '*') {
            i++;
            if (pattern[i] == '*') {
                i++;
                len = (ptrdiff_t)strlen(str + j);
            } else {
                len = (ptrdiff_t)strcspn(str + j, "/");
            }
            if (i == (ptrdiff_t)pattern_len)
                return j + len;
            do {
                res = match_prefix(pattern + i, pattern_len - i, str + j + len);
            } while (res == -1 && len-- > 0);
            return res == -1 ? -1 : j + res + len;
        } else if (tolower((unsigned char)pattern[i]) != tolower((unsigned char)str[j])) {
            return -1;
        }
    }
    return j;
}

const char *stub_route(const char *uri)
{
    size_t urilen = strlen(uri);

    for (int i = 0; i < g_nhandlers; i++)
        if (!strcmp(g_handlers[i].uri, uri))
            return g_handlers[i].uri;
    for (int i = 0; i < g_nhandlers; i++) {
        size_t n = strlen(g_handlers[i].uri);

        if (n < urilen && uri[n] == '/' && !memcmp(g_handlers[i].uri, uri, n))
            return g_handlers[i].uri;
    }
    for (int i = 0; i < g_nhandlers; i++)
        if (match_prefix(g_handlers[i].uri, strlen(g_handlers[i].uri), uri) > 0)
            return g_handlers[i].uri;
    return NULL;
}

static void conn_init(struct mg_connection *c, const stub_req *req, stub_rsp *rsp)
{
    memset(rsp, 0, sizeof *rsp);
    memset(c, 0, sizeof *c);
    c->req               = req;
    c->rsp               = rsp;
    c->unread            = req->unread;
    c->ri.request_method = req->method;
    c->ri.request_uri    = req->uri;
    c->ri.local_uri      = req->uri;
    c->ri.query_string   = req->query;
    c->ri.content_length = req->content_length;
    c->ri.is_ssl         = req->is_ssl;
    c->ri.server_port    = req->server_port;
    snprintf(c->ri.remote_addr, sizeof c->ri.remote_addr, "%s",
            req->remote_addr ? req->remote_addr : "127.0.0.1");
}

int stub_invoke(const char *uri, const stub_req *req, stub_rsp *rsp)
{
    struct mg_connection c;

    conn_init(&c, req, rsp);
    for (int i = 0; i < g_nhandlers; i++)
        if (!strcmp(g_handlers[i].uri, uri))
            return g_handlers[i].fn(&c, g_handlers[i].cbdata);
    return -1;
}

int stub_call(int (*fn)(struct mg_connection *), const stub_req *req, stub_rsp *rsp)
{
    struct mg_connection c;

    conn_init(&c, req, rsp);
    return fn(&c);
}

void web_log(const char *lvl, const char *fmt, ...)
{
    char    line[512];
    va_list ap;
    int     n;

    if (web_log_web_file.fd < 0 || web_log_rank(lvl) > web_log_threshold())
        return;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof line - 2)
        n = sizeof line - 2;
    line[n++] = '\n';
    web_log_append(web_log_web_file.fd, line, (size_t)n);
}

void mg_set_request_handler(struct mg_context *ctx, const char *uri, mg_request_handler handler,
        void *cbdata)
{
    (void)ctx;
    for (int i = 0; i < g_nhandlers; i++) {
        if (!strcmp(g_handlers[i].uri, uri)) {
            g_handlers[i].fn     = handler;
            g_handlers[i].cbdata = cbdata;
            return;
        }
    }
    if (g_nhandlers == STUB_MAX_HANDLERS)
        return;
    g_handlers[g_nhandlers].uri    = uri;
    g_handlers[g_nhandlers].fn     = handler;
    g_handlers[g_nhandlers].cbdata = cbdata;
    g_nhandlers++;
}

const struct mg_request_info *mg_get_request_info(const struct mg_connection *c)
{
    return c ? &c->ri : NULL;
}

const char *mg_get_header(const struct mg_connection *c, const char *name)
{
    if (!c || !name)
        return NULL;
    if (!strcasecmp(name, "Content-Type"))
        return c->req->content_type;
    if (!strcasecmp(name, "Cookie"))
        return c->req->cookie;
    if (!strcasecmp(name, "Authorization"))
        return c->req->authorization;
    if (!strcasecmp(name, "Token"))
        return c->req->token;
    if (!strcasecmp(name, "Host"))
        return c->req->host;
    if (!strcasecmp(name, "Origin"))
        return c->req->origin;
    if (!strcasecmp(name, "Referer"))
        return c->req->referer;
    return NULL;
}

const char *mg_get_response_code_text(const struct mg_connection *conn, int response_code)
{
    (void)conn;
    switch (response_code) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 302:
        return "Found";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 411:
        return "Length Required";
    case 413:
        return "Request Entity Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    }
    if (response_code >= 100 && response_code < 200)
        return "Information";
    if (response_code >= 200 && response_code < 300)
        return "Success";
    if (response_code >= 300 && response_code < 400)
        return "Redirection";
    if (response_code >= 400 && response_code < 500)
        return "Client Error";
    if (response_code >= 500 && response_code < 600)
        return "Server Error";
    return "";
}

int mg_get_cookie(const char *cookie, const char *var_name, char *buf, size_t buf_len)
{
    size_t namelen;

    if (!buf || !buf_len)
        return -2;
    buf[0] = '\0';
    if (!cookie || !var_name)
        return -1;

    namelen = strlen(var_name);
    for (const char *p = cookie; (p = strstr(p, var_name)) != NULL; p += namelen) {
        const char *v, *end;
        size_t      n;

        if (p[namelen] != '=')
            continue;
        v   = p + namelen + 1;
        end = strchr(v, ';');
        n   = end ? (size_t)(end - v) : strlen(v);
        if (n >= buf_len)
            return -2;
        memcpy(buf, v, n);
        buf[n] = '\0';
        return (int)n;
    }
    return -1;
}

int mg_read(struct mg_connection *c, void *buf, size_t len)
{
    size_t n = c->unread < len ? c->unread : len;

    c->rsp->reads++;
    if (!n)
        return 0;
    if (c->req->read_chunk && n > c->req->read_chunk)
        n = c->req->read_chunk;
    size_t offset = c->req->unread - c->unread;
    if (c->req->body && offset + n <= c->req->body_len)
        memcpy(buf, c->req->body + offset, n);
    else
        memset(buf, 'x', n);
    c->unread -= n;
    c->rsp->drained += n;
    return (int)n;
}

int mg_write(struct mg_connection *c, const void *buf, size_t len)
{
    size_t room = sizeof c->rsp->body - 1 - c->rsp->body_len;
    size_t n    = len < room ? len : room;

    memcpy(c->rsp->body + c->rsp->body_len, buf, n);
    c->rsp->body_len += n;
    c->rsp->body[c->rsp->body_len] = '\0';
    return (int)len;
}

int mg_printf(struct mg_connection *c, const char *fmt, ...)
{
    char    line[512];
    va_list ap;
    int     n, status = 0;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (sscanf(line, "HTTP/1.1 %d", &status) == 1) {
        c->rsp->status = status;
        snprintf(c->rsp->head, sizeof c->rsp->head, "%s", line);
    }
    return n;
}

int mg_get_server_ports(const struct mg_context *ctx, int size, struct mg_server_port *ports)
{
    (void)ctx;
    (void)size;
    (void)ports;
    return 0;
}

unsigned mg_check_feature(unsigned feature)
{
    return feature & MG_FEATURES_TLS;
}

void mg_close_connection(struct mg_connection *c)
{
    c->rsp->closed++;
}

int mg_handle_form_request(struct mg_connection *c, struct mg_form_data_handler *fdh)
{
    const stub_req *r         = c->req;
    char            path[256] = { 0 };

    c->rsp->form_calls++;
    if (r->filename && fdh->field_found) {
        int rc = fdh->field_found("firmware", r->filename, path, sizeof path, fdh->user_data);

        if (rc == MG_FORM_FIELD_STORAGE_GET && fdh->field_get && r->body_len)
            fdh->field_get("firmware", r->body, r->body_len, fdh->user_data);
    }
    return r->form_rc;
}
