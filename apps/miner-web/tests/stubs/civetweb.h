/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2013-2024 the Civetweb developers
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * For the licensing terms, see third-party-license/MIT.txt.
 */

#ifndef CIVETWEB_H
#define CIVETWEB_H

#include <stddef.h>

struct mg_context;
struct mg_connection;

/* Server start callbacks; begin_request runs before handler dispatch */
struct mg_callbacks { int (*begin_request)(struct mg_connection *); };

/* Request details of one connection, subset of the civetweb layout */
struct mg_request_info {
    const char *request_method;
    const char *request_uri;     /* URI as in the request, URL-decoded */
    const char *local_uri;       /* URI relative to the server, URL-decoded */
    const char *http_version;    /* protocol version, e.g. "1.1" */
    const char *query_string;
    const char *remote_user;     /* authenticated user name, NULL when none */
    char        remote_addr[48];
    long long   content_length;  /* Content-Length header value, -1 when absent */
    int         num_headers;
    struct { const char *name; const char *value; } http_headers[64];
    int         remote_port;
    int         server_port;
    int         is_ssl;          /* 1 for HTTPS, 0 for HTTP */
    void       *user_data;       /* user_data passed to mg_start */
    void       *conn_data;       /* per-connection user pointer */
};

/* Callbacks driving mg_handle_form_request */
struct mg_form_data_handler {
    /* called per field; returns an MG_FORM_FIELD_STORAGE_* value */
    int (*field_found)(const char *key, const char *filename, char *path,
                       size_t pathlen, void *user_data);
    /* called with field data for MG_FORM_FIELD_STORAGE_GET */
    int (*field_get)(const char *key, const char *value, size_t valuelen,
                     void *user_data);
    /* called after a field is stored to path */
    int (*field_store)(const char *path, long long file_size, void *user_data);
    void *user_data; /* opaque argument passed to the callbacks */
};

/* field_found return values: how to handle the field */
enum {
    MG_FORM_FIELD_STORAGE_SKIP  = 0x0,
    MG_FORM_FIELD_STORAGE_GET   = 0x1, /* deliver the data to field_get */
    MG_FORM_FIELD_STORAGE_STORE = 0x2, /* store the data to a file */
    MG_FORM_FIELD_STORAGE_ABORT = 0x10
};

/* field_get and field_store return values */
enum {
    MG_FORM_FIELD_HANDLE_GET   = 0x1, /* keep receiving data for this field */
    MG_FORM_FIELD_HANDLE_NEXT  = 0x8,
    MG_FORM_FIELD_HANDLE_ABORT = 0x10
};

/* One listening port, subset of the civetweb layout */
struct mg_server_port {
    int protocol;    /* 1 IPv4, 2 IPv6, 3 both */
    int port;
    int is_ssl;      /* 1 for an HTTPS port */
    int is_redirect;
};

typedef int (*mg_request_handler)(struct mg_connection *conn, void *cbdata);

struct mg_context *mg_start(const struct mg_callbacks *, void *, const char **);
void mg_stop(struct mg_context *);
const struct mg_request_info *
mg_get_request_info(const struct mg_connection *);
int mg_get_server_ports(const struct mg_context *ctx, int size,
                        struct mg_server_port *ports);
#define MG_FEATURES_TLS 0x2u
unsigned mg_check_feature(unsigned feature);
const char *mg_get_header(const struct mg_connection *, const char *name);
const char *mg_get_response_code_text(const struct mg_connection *conn,
                                      int response_code);
int mg_get_cookie(const char *cookie, const char *var_name, char *buf,
                  size_t buf_len);
int mg_read(struct mg_connection *, void *buf, size_t len);
int mg_write(struct mg_connection *, const void *buf, size_t len);
int mg_printf(struct mg_connection *, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void mg_close_connection(struct mg_connection *conn);
int mg_handle_form_request(struct mg_connection *conn,
                           struct mg_form_data_handler *fdh);
void mg_set_request_handler(struct mg_context *ctx, const char *uri,
                            mg_request_handler handler, void *cbdata);

#endif /* CIVETWEB_H */
