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
#define _DEFAULT_SOURCE

#include "upload.h"
#include "web_logfmt.h"
#include "aup.h"
#include "ota_proto.h"
#include "webauth.h"
#include "webcred.h"
#include "webtoken.h"
#include "webstatus.h"
#include "router.h"
#include "core_api.h"
#include "hex.h"
#include "jread.h"
#include "webform.h"
#include "wp_proto.h"
#include <civetweb.h>
#include <errno.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#define UPLOAD_IO_TIMEOUT_S 5                  /* OTA socket send/receive timeout, s */
#define UPLOAD_SNDBUF_BYTES (10 * 1024 * 1024) /* OTA socket send buffer, bytes */

/* Discard buffer for a rejected body; the bytes are thrown away, so keep it small. */
#define UPLOAD_DRAIN_CHUNK 4096

#define UPLOAD_REJECT_DRAIN_MAX_BYTES 65536 /* rejected body discard limit, bytes */
#define UPLOAD_REJECT_DRAIN_MAX_S     2     /* rejected body discard limit, s */

#define WP_REPLY_CAP     4096
#define WP_ASCSET_PREFIX "0,wallpaper,"
#define CT_JSON          "application/json"

typedef struct {
    const char *sock;
    const char *systemcfg; /* credentials ini path */
} upload_cfg;

typedef struct {
    web_upload up;           /* OTA socket connection */
    int        started;      /* 1 once the file field was found */
    int        sock_error;   /* 1 when a write to the OTA socket failed */
    int        client_error; /* 1 when the request body was truncated or stalled */
    time_t     progress_at;  /* monotonic time body bytes last arrived, s */
} upload_job;

static int        g_upgrading;
static int        g_wallpaper_busy;
static upload_cfg g_cfg;
static upload_cfg g_wp_cfg;

static time_t mono_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

int web_upload_open(web_upload *u, const char *sock_path)
{
    struct sockaddr_un sa;
    struct timeval     tv;
    int                fd, sndbuf;

    u->fd   = -1;
    u->sent = 0;
    if (strlen(sock_path) >= sizeof sa.sun_path)
        return -1;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sock_path, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }

    tv.tv_sec  = UPLOAD_IO_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sndbuf = UPLOAD_SNDBUF_BYTES;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);

    u->fd = fd;
    return 0;
}

int web_upload_write(web_upload *u, const void *buf, size_t len)
{
    const char *p   = buf;
    size_t      off = 0;
    time_t      stall;

    if (!u || u->fd < 0)
        return -1;

    stall = mono_s() + UPLOAD_WRITE_STALL_S;
    while (off < len) {
        ssize_t n = send(u->fd, p + off, len - off, MSG_NOSIGNAL);

        if (n > 0) {
            off += (size_t)n;
            u->sent += (unsigned long long)n;
            stall = mono_s() + UPLOAD_WRITE_STALL_S;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && mono_s() < stall)
            continue;
        return -1;
    }
    return 0;
}

void web_upload_abort(web_upload *u)
{
    if (u && u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
}

int web_upload_finish(web_upload *u, web_upload_status *st)
{
    unsigned char buf[sizeof(mm_ota_msg)];
    size_t        readn = 0;
    time_t        deadline;
    mm_ota_msg    msg;

    memset(st, 0, sizeof *st);
    if (!u || u->fd < 0)
        return -1;

    shutdown(u->fd, SHUT_WR);
    deadline = mono_s() + UPLOAD_STATUS_WAIT_S;
    while (readn < sizeof buf) {
        ssize_t n = read(u->fd, buf + readn, sizeof buf - readn);

        if (n > 0) {
            readn += (size_t)n;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && mono_s() < deadline)
            continue;
        break;
    }
    close(u->fd);
    u->fd = -1;

    if (readn != sizeof buf)
        return -1;
    memcpy(&msg, buf, sizeof msg);
    if ((unsigned)msg.magic != (unsigned)MM_OTA_MAGIC || msg.type != MM_OTA_MSG_TYPE)
        return -1;

    st->valid   = 1;
    st->percent = msg.upgrade.percent;
    st->error   = msg.upgrade.error;
    return 0;
}

size_t web_upload_reply(const web_upload_status *st, char *out, size_t cap)
{
    char msg[96];

    if (!st->valid)
        return web_status_json("S", WEB_STATUS_OK_CODE,
                "Upgrade status unknown, device reported no upgrade result", out, cap);
    snprintf(msg, sizeof msg, "Upgrade %s(%d), percent(%d)", st->error > 0 ? "ok" : "failed",
            st->error, st->percent);
    if (st->error > 0)
        return web_status_json("S", WEB_STATUS_OK_CODE, msg, out, cap);
    return web_status_json("E", WEB_STATUS_FAULT_CODE, msg, out, cap);
}

size_t web_upload_timeout_reply(const web_upload_status *st, char *out, size_t cap)
{
    char msg[96];

    if (!st->valid)
        return web_status_json("E", WEB_STATUS_FAULT_CODE,
                "Upgrade status unknown, the device stopped receiving the image", out, cap);
    snprintf(msg, sizeof msg, "Upgrade failed(%d), percent(%d)", st->error, st->percent);
    return web_status_json("E", WEB_STATUS_FAULT_CODE, msg, out, cap);
}

int web_upload_ct_is_multipart(const char *ct)
{
    return ct && strncasecmp(ct, "multipart", 9) == 0;
}

int web_upload_body_stalled(time_t last_progress, time_t now)
{
    return now - last_progress >= UPLOAD_BODY_STALL_S;
}

static int upgrading_take(void)
{
    return __atomic_exchange_n(&g_upgrading, 1, __ATOMIC_SEQ_CST);
}

static void upgrading_release(void)
{
    __atomic_store_n(&g_upgrading, 0, __ATOMIC_SEQ_CST);
}

static int send_reply(struct mg_connection *c, int status, const char *ct, const char *body,
        size_t len)
{
    mg_printf(c,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            status, mg_get_response_code_text(c, status), ct, len);
    mg_write(c, body, len);
    mg_close_connection(c);
    return status;
}

static void drain_pending_body(struct mg_connection *c)
{
    char               buf[UPLOAD_DRAIN_CHUNK];
    unsigned long long drained  = 0;
    time_t             deadline = mono_s() + UPLOAD_REJECT_DRAIN_MAX_S;
    int                limited  = 0;

    for (;;) {
        unsigned long long remain = UPLOAD_REJECT_DRAIN_MAX_BYTES - drained;
        size_t             want   = sizeof buf;
        int                n;

        if (remain == 0 || mono_s() >= deadline) {
            limited = 1;
            break;
        }
        if (want > remain)
            want = (size_t)remain;
        n = mg_read(c, buf, want);
        if (n <= 0)
            break;
        drained += (unsigned long long)n;
    }
    web_log("INFO",
            "upload: discarded %llu bytes the client was still sending, then "
            "closed the connection%s",
            drained, limited ? " (discard limit reached)" : "");
}

static int send_reject(struct mg_connection *c, int status, const char *body, size_t len)
{
    drain_pending_body(c);
    return send_reply(c, status, CT_JSON, body, len);
}

static int send_status(struct mg_connection *c, int status, int code, const char *msg)
{
    char   out[512];
    size_t len = web_status_json("E", code, msg, out, sizeof out);

    return send_reject(c, status, out, len);
}

static int web_upload_authed(const struct mg_connection *c, const char *systemcfg)
{
    mc_webcred  creds;
    char        cookie_auth[64] = { 0 };
    const char *cookie, *token;
    int         ok = 0;

    if (!systemcfg || web_creds_load(systemcfg, &creds) != 0)
        return 0;

    token = mg_get_header(c, "Token");
    if (token) {
        ok = !mc_api_webpass_unset(&creds) && web_token_check(&creds, token) == WEB_TOKEN_OK;
        goto out;
    }

    cookie = mg_get_header(c, "Cookie");
    if (cookie)
        mg_get_cookie(cookie, "auth", cookie_auth, sizeof cookie_auth);
    ok = cookie_auth[0] && mc_api_check_cookie(&creds, cookie_auth);
out:
    explicit_bzero(cookie_auth, sizeof cookie_auth);
    explicit_bzero(&creds, sizeof creds);
    return ok;
}

static int field_found(const char *key, const char *filename, char *path, size_t pathlen,
        void *user)
{
    upload_job *j = user;

    (void)key;
    (void)path;
    (void)pathlen;
    if (j->started || !filename || !*filename)
        return MG_FORM_FIELD_STORAGE_SKIP;
    j->started = 1;
    return MG_FORM_FIELD_STORAGE_GET;
}

static int field_get(const char *key, const char *value, size_t valuelen, void *user)
{
    upload_job *j = user;

    (void)key;
    if (web_upload_body_stalled(j->progress_at, mono_s())) {
        web_log("WARN",
                "upload refused: the client stopped sending for %d s "
                "(%llu bytes received)",
                UPLOAD_BODY_STALL_S, j->up.sent);
        j->client_error = 1;
        return MG_FORM_FIELD_HANDLE_ABORT;
    }
    if (web_upload_write(&j->up, value, valuelen) != 0) {
        j->sock_error = 1;
        return MG_FORM_FIELD_HANDLE_ABORT;
    }
    if (valuelen)
        j->progress_at = mono_s();
    return MG_FORM_FIELD_HANDLE_GET;
}

static int upload_handler(struct mg_connection *c, void *cbdata)
{
    const upload_cfg             *cfg = cbdata;
    const struct mg_request_info *ri  = mg_get_request_info(c);
    const char                   *ct, *refused;
    struct mg_form_data_handler   fdh;
    upload_job                    j;
    web_upload_status             st;
    char                          out[512];
    size_t                        len;

    refused = web_request_refused(mg_get_header(c, "Host"), mg_get_header(c, "Origin"),
            mg_get_header(c, "Referer"), 1);
    if (refused) {
        web_log("WARN", "upload refused (HTTP 403): %s", refused);
        return send_status(c, 403, WEB_STATUS_REFUSED_CODE, WEB_STATUS_REFUSED_MSG);
    }

    if (!ri || !ri->request_method || strcmp(ri->request_method, "POST") != 0) {
        web_log("WARN", "upload refused (HTTP 405): %s request, only POST is accepted",
                (ri && ri->request_method) ? ri->request_method : "(none)");
        return send_status(c, 405, WEB_STATUS_METHOD_CODE, WEB_STATUS_METHOD_MSG);
    }

    if (!ri->is_ssl) {
        web_log("WARN", "upload refused (HTTP 403): HTTPS required");
        return send_status(c, 403, WEB_STATUS_HTTPS_CODE, WEB_STATUS_HTTPS_MSG);
    }

    web_log("INFO", "upload started: %s from %s, %lld bytes", ri->local_uri ? ri->local_uri : "-",
            ri->remote_addr, (long long)ri->content_length);

    if (!web_upload_authed(c, cfg->systemcfg)) {
        web_log("WARN", "upload refused (HTTP 401): not logged in");
        web_logf_noweb(stderr, "WARN", "firmware upload refused: not logged in\n");
        if (mg_get_header(c, "Token"))
            return send_status(c, 401, WEB_STATUS_AUTH_CODE, WEB_STATUS_TOKEN_STALE_MSG);
        return send_status(c, 401, WEB_STATUS_AUTH_CODE, WEB_STATUS_NO_LOGIN_MSG);
    }

    ct = mg_get_header(c, "Content-Type");
    if (!web_upload_ct_is_multipart(ct)) {
        web_log("WARN", "upload refused (HTTP 400): not a file upload (Content-Type %s)",
                ct ? ct : "(none)");
        return send_status(c, 400, WEB_STATUS_BAD_REQ_CODE,
                "firmware upload requires a multipart body");
    }

    if (upgrading_take()) {
        web_log("ERR", "upload refused (HTTP 409): another firmware upload is in progress");
        web_logf_noweb(stderr, "WARN",
                "firmware upload refused: another upload "
                "is in progress\n");
        return send_status(c, 409, WEB_STATUS_FAULT_CODE, WEB_UPLOAD_BUSY_MSG);
    }

    memset(&j, 0, sizeof j);
    if (web_upload_open(&j.up, cfg->sock) != 0) {
        web_log("ERR",
                "upload refused (HTTP 503): the upgrade service is not "
                "reachable (%s)",
                strerror(errno));
        web_logf_noweb(stderr, "ERR",
                "firmware upload refused: the upgrade "
                "service is not reachable\n");
        upgrading_release();
        return send_status(c, 503, WEB_STATUS_FAULT_CODE, WEB_UPLOAD_NOSOCK_MSG);
    }
    web_log("INFO", "upload: passing the firmware to the upgrade service");
    web_logf_noweb(stderr, "INFO",
            "firmware upload received from the web page "
            "(%lld bytes), passing it to the upgrade service\n",
            (long long)ri->content_length);
    j.progress_at = mono_s();

    fdh.field_found = field_found;
    fdh.field_get   = field_get;
    fdh.field_store = NULL;
    fdh.user_data   = &j;
    if (mg_handle_form_request(c, &fdh) < 0 && !j.sock_error) {
        web_log("ERR", "upload: the request body could not be read after %llu bytes", j.up.sent);
        j.client_error = 1;
    }

    if (j.sock_error) {
        web_log("ERR",
                "upload: the upgrade service stopped accepting data after "
                "%llu bytes",
                j.up.sent);
        web_upload_finish(&j.up, &st);
        upgrading_release();
        web_log("ERR",
                "upload failed (HTTP 500): upgrade service %s, result code %d "
                "at %d%%",
                st.valid ? "answered" : "gave no result", st.error, st.percent);
        web_logf_noweb(stderr, "ERR",
                "firmware upload failed: the upgrade "
                "service stopped accepting data (result code %d)\n",
                st.error);
        len = web_upload_timeout_reply(&st, out, sizeof out);
        return send_reject(c, 500, out, len);
    }

    if (j.client_error || !j.started) {
        web_log("WARN", "upload refused (HTTP 400): %s (%llu bytes received)",
                j.started ? "the upload was truncated" : "no firmware image in the request",
                j.up.sent);
        web_logf_noweb(stderr, "WARN", "firmware upload refused: %s\n",
                j.started ? "the upload was truncated" : "no firmware image in the request");
        web_upload_abort(&j.up);
        upgrading_release();
        return send_status(c, 400, WEB_STATUS_BAD_REQ_CODE,
                j.started ? "the upload was truncated" : "no firmware image in the request");
    }

    web_log("INFO",
            "upload: %llu bytes received; the upgrade service is checking "
            "and writing the firmware",
            j.up.sent);
    web_logf_noweb(stderr, "INFO",
            "firmware upload complete; the upgrade service "
            "is verifying and writing flash\n");
    web_upload_finish(&j.up, &st);
    upgrading_release();

    web_log("INFO", "upload finished: upgrade service %s, result code %d, %d%% done",
            st.valid ? "answered" : "gave no result", st.error, st.percent);
    web_logf_noweb(stderr, "INFO",
            "firmware upload finished: upgrade service %s, "
            "result code %d\n",
            st.valid ? "answered" : "gave no result", st.error);
    if (!st.valid)
        web_log("INFO",
                "upload: no result from the upgrade service, answered HTTP 202 "
                "(accepted, outcome unknown)");
    len = web_upload_reply(&st, out, sizeof out);
    return send_reply(c, st.valid ? 200 : 202, CT_JSON, out, len);
}

static int wp_core_call(const char *param, char *out, size_t cap)
{
    int               rc = core_api_call("ascset", param, out, cap);
    json_value       *v;
    const json_value *st;
    int               status = 200;

    if (rc == CORE_API_TRUNCATED) {
        web_status_json("E", WEB_STATUS_FAULT_CODE, WEB_STATUS_TOO_LONG_MSG, out, cap);
        return 500;
    }
    if (rc < 0) {
        web_status_json("E", WEB_STATUS_FAULT_CODE, WEB_STATUS_NO_CORE_MSG, out, cap);
        return 504;
    }
    v  = json_parse(out);
    st = json_object_get(json_array_get(json_object_get(v, "STATUS"), 0), "STATUS");
    if (!strcmp(json_string(st), "E"))
        status = 400;
    json_free(v);
    return status;
}

static int wp_chunk(const struct wp_header *hdr, const char *fmt, const uint8_t *payload, char *out,
        size_t cap)
{
    uint8_t pkg[WP_PKG_LEN / 2];
    char    param[sizeof WP_ASCSET_PREFIX + sizeof "png," + WP_PKG_LEN];
    int     n = snprintf(param, sizeof param, WP_ASCSET_PREFIX "%s,", fmt);
    int     status;

    memcpy(pkg, hdr, WP_HDR_LEN);
    memcpy(pkg + WP_HDR_LEN, payload, hdr->payload_len);
    hex_encode(param + n, pkg, (size_t)WP_HDR_LEN + hdr->payload_len);
    status = wp_core_call(param, out, cap);
    explicit_bzero(pkg, sizeof pkg);
    explicit_bzero(param, sizeof param);
    return status;
}

static int wp_forward(const char *fmt, const uint8_t *file, uint32_t size, const uint8_t *sig,
        char *out, size_t cap)
{
    struct wp_header hdr = { .format_ver = WP_VER, .header_len = WP_HDR_LEN, .file_size = size };
    uint16_t         uid = 0;
    int              status;

    while (!uid) {
        if (RAND_bytes((unsigned char *)&uid, sizeof uid) != 1) {
            web_status_json("E", WEB_STATUS_FAULT_CODE, WEB_STATUS_INTERNAL_MSG, out, cap);
            return 500;
        }
    }
    memcpy(hdr.magic, WP_MAGIC, strlen(WP_MAGIC));
    hdr.uid         = uid;
    hdr.payload_len = WP_SHA_LEN + WP_SIGN_LEN;
    status          = wp_chunk(&hdr, fmt, sig, out, cap);
    for (uint32_t off = 0; status == 200 && off < size; off += hdr.payload_len) {
        hdr.idx++;
        hdr.offset      = off;
        hdr.payload_len = (uint16_t)(size - off < WP_PAYLOAD_MAX ? size - off : WP_PAYLOAD_MAX);
        status          = wp_chunk(&hdr, fmt, file + off, out, cap);
    }
    return status;
}

static int wp_hex_arg(const char *query, const char *key, uint8_t *dst, size_t nbytes)
{
    char hex[2 * WP_SIGN_LEN + 2];

    if (web_form_get(query, key, hex, sizeof hex) != 1 || strlen(hex) != 2 * nbytes ||
            hex_decode(dst, hex, nbytes) != 0)
        return -1;
    return 0;
}

static int wp_refused(struct mg_connection *c, int status, const char *why, int code,
        const char *msg)
{
    web_log("WARN", "wallpaper refused (HTTP %d): %s", status, why);
    return send_status(c, status, code, msg);
}

static int wp_reply(struct mg_connection *c, int status, const char *body)
{
    if (status == 200)
        web_log("INFO", "wallpaper request finished (HTTP 200)");
    else
        web_log("WARN", "wallpaper request failed (HTTP %d)", status);
    return send_reply(c, status, CT_JSON, body, strlen(body));
}

static int wp_busy_take(void)
{
    return __atomic_exchange_n(&g_wallpaper_busy, 1, __ATOMIC_SEQ_CST);
}

static void wp_busy_release(void)
{
    __atomic_store_n(&g_wallpaper_busy, 0, __ATOMIC_SEQ_CST);
}

static int wp_upload(struct mg_connection *c, const struct mg_request_info *ri, char *out,
        size_t cap)
{
    const char *q = ri->query_string;
    char        fmt[8];
    uint8_t     sig[WP_SHA_LEN + WP_SIGN_LEN];
    uint8_t    *file;
    long long   size = ri->content_length;
    size_t      got  = 0;
    time_t      progress_at;
    int         status;

    if (web_form_get(q, "format", fmt, sizeof fmt) != 1 ||
            (strcmp(fmt, "png") && strcmp(fmt, "gif")))
        return wp_refused(c, 400, "format is not png or gif", WEB_STATUS_BAD_REQ_CODE,
                "Invalid format, png or gif required");
    if (wp_hex_arg(q, "sha256", sig, WP_SHA_LEN) != 0)
        return wp_refused(c, 400, "sha256 is not 64 hex digits", WEB_STATUS_BAD_REQ_CODE,
                "Invalid sha256, 64 hex digits required");
    if (wp_hex_arg(q, "signature", sig + WP_SHA_LEN, WP_SIGN_LEN) != 0)
        return wp_refused(c, 400, "signature is not 256 hex digits", WEB_STATUS_BAD_REQ_CODE,
                "Invalid signature, 256 hex digits required");
    if (size < 0)
        return wp_refused(c, 411, "no Content-Length", WEB_STATUS_BAD_REQ_CODE,
                "Content-Length required");
    if (size < WP_SIZE_MIN || size > WP_SIZE_MAX) {
        snprintf(out, cap, "%lld bytes", size);
        return wp_refused(c, 413, out, WEB_STATUS_BAD_REQ_CODE, "Wallpaper size out of range");
    }
    if (__atomic_load_n(&g_upgrading, __ATOMIC_SEQ_CST))
        return wp_refused(c, 409, "a firmware upload is in progress", WEB_STATUS_FAULT_CODE,
                "A firmware upgrade is in progress");
    if (wp_busy_take())
        return wp_refused(c, 409, "another wallpaper upload is in progress", WEB_STATUS_FAULT_CODE,
                "Another wallpaper upload is in progress");

    file = malloc((size_t)size);
    if (!file) {
        wp_busy_release();
        return wp_refused(c, 500, "out of memory", WEB_STATUS_FAULT_CODE, WEB_STATUS_INTERNAL_MSG);
    }
    web_log("INFO", "wallpaper upload started: %s, %lld bytes from %s", fmt, size, ri->remote_addr);
    progress_at = mono_s();
    while (got < (size_t)size && !web_upload_body_stalled(progress_at, mono_s())) {
        int n = mg_read(c, file + got, (size_t)size - got);

        if (n <= 0)
            break;
        got += (size_t)n;
        progress_at = mono_s();
    }
    if (got != (size_t)size) {
        explicit_bzero(file, got);
        free(file);
        wp_busy_release();
        snprintf(out, cap, "the upload was truncated after %zu of %lld bytes", got, size);
        return wp_refused(c, 400, out, WEB_STATUS_BAD_REQ_CODE, "Upload truncated");
    }
    status = wp_forward(fmt, file, (uint32_t)size, sig, out, cap);
    explicit_bzero(file, (size_t)size);
    free(file);
    explicit_bzero(sig, sizeof sig);
    wp_busy_release();
    return wp_reply(c, status, out);
}

static int wallpaper_handler(struct mg_connection *c, void *cbdata)
{
    const upload_cfg             *cfg = cbdata;
    const struct mg_request_info *ri  = mg_get_request_info(c);
    const char                   *refused;
    char                          out[WP_REPLY_CAP];
    int                           status;

    refused = web_request_refused(mg_get_header(c, "Host"), mg_get_header(c, "Origin"),
            mg_get_header(c, "Referer"), 1);
    if (refused)
        return wp_refused(c, 403, refused, WEB_STATUS_REFUSED_CODE, WEB_STATUS_REFUSED_MSG);
    if (!ri || !ri->request_method ||
            (strcmp(ri->request_method, "POST") && strcmp(ri->request_method, "DELETE")))
        return wp_refused(c, 405, "only POST and DELETE are accepted", WEB_STATUS_METHOD_CODE,
                WEB_STATUS_METHOD_MSG);
    if (!ri->is_ssl)
        return wp_refused(c, 403, "HTTPS required", WEB_STATUS_HTTPS_CODE, WEB_STATUS_HTTPS_MSG);
    if (!web_upload_authed(c, cfg->systemcfg)) {
        if (mg_get_header(c, "Token"))
            return wp_refused(c, 401, "invalid token", WEB_STATUS_AUTH_CODE,
                    WEB_STATUS_TOKEN_STALE_MSG);
        return wp_refused(c, 401, "not logged in", WEB_STATUS_AUTH_CODE, WEB_STATUS_NO_LOGIN_MSG);
    }
    if (!strcmp(ri->request_method, "POST"))
        return wp_upload(c, ri, out, sizeof out);

    if (wp_busy_take())
        return wp_refused(c, 409, "a wallpaper upload is in progress", WEB_STATUS_FAULT_CODE,
                "Another wallpaper upload is in progress");
    status = wp_core_call(WP_ASCSET_PREFIX "none", out, sizeof out);
    wp_busy_release();
    return wp_reply(c, status, out);
}

void web_upload_register(struct mg_context *mgc, const char *sock_path, const char *systemcfg_ini)
{
    g_cfg.sock      = sock_path;
    g_cfg.systemcfg = systemcfg_ini;
    mg_set_request_handler(mgc, WEB_UPGRADE_URI, upload_handler, &g_cfg);
}

void web_wallpaper_register(struct mg_context *mgc, const char *systemcfg_ini)
{
    g_wp_cfg.systemcfg = systemcfg_ini;
    mg_set_request_handler(mgc, WEB_WALLPAPER_URI, wallpaper_handler, &g_wp_cfg);
}
