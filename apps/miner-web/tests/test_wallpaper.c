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

#include "upload.h"
#include "router.h"
#include "core_api.h"
#include "aup.h"
#include "hex.h"
#include "logexport.h"
#include "ota_proto.h"
#include "stub_civetweb.h"
#include "wp_proto.h"

#include <civetweb.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

#define WEBUSER    "miner"
#define WEBPASS    "upload-pass"
#define BASIC_AUTH "Basic bWluZXI6dXBsb2FkLXBhc3M="
#define NO_TOKEN \
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.e30.AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#define PREFIX_PNG    "0,wallpaper,png,"
#define MAX_CALLS     512
#define STATUS_HEAD   "{\"STATUS\":[{\"STATUS\":\"E\",\"When\":"
#define STALE_TAIL    "\"Code\":602,\"Msg\":\"Token expired, request a new one\"}],\"id\":1}"
#define NO_LOGIN_TAIL "\"Code\":602,\"Msg\":\"Not logged in\"}],\"id\":1}"

typedef struct {
    size_t   size;
    stub_rsp rsp;
    int      rc;
} post_job;

typedef struct {
    int listen_fd;
    int eof;
    int release;
} fake_ota;

typedef struct {
    stub_rsp rsp;
    int      rc;
} ota_job;

static int             fails;
static char            g_ini[96];
static char            g_cookie[96];
static char            g_sock[96];
static char           *g_params[MAX_CALLS];
static int             g_ncalls;
static int             g_fail_at = -1;
static int             g_dead;
static int             g_trunc;
static int             g_hold;
static int             g_in_call;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static uint8_t         g_file[WP_SIZE_MAX];
static uint8_t         g_sha[WP_SHA_LEN];
static uint8_t         g_sign[WP_SIGN_LEN];
static char            g_query[64 + 2 * (WP_SHA_LEN + WP_SIGN_LEN)];

int core_api_call(const char *cmd, const char *param, char *out, size_t cap)
{
    int idx;

    pthread_mutex_lock(&g_lock);
    idx = g_ncalls;
    if (g_ncalls < MAX_CALLS && !strcmp(cmd, "ascset"))
        g_params[g_ncalls++] = strdup(param ? param : "");
    g_in_call = 1;
    pthread_cond_broadcast(&g_cond);
    while (g_hold)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
    if (g_dead)
        return -1;
    if (g_trunc)
        return CORE_API_TRUNCATED;
    snprintf(out, cap,
            "{\"STATUS\":[{\"STATUS\":\"%s\",\"When\":1,\"Code\":%d,\"Msg\":\"chunk %d\","
            "\"Description\":\"core\"}],\"id\":1}",
            idx == g_fail_at ? "E" : "I", idx == g_fail_at ? 120 : 119, idx);
    return (int)strlen(out);
}

static void calls_reset(void)
{
    for (int i = 0; i < g_ncalls; i++)
        free(g_params[i]);
    g_ncalls  = 0;
    g_fail_at = -1;
    g_dead    = 0;
    g_trunc   = 0;
    g_in_call = 0;
}

static int creds_write(void)
{
    char  digest[65];
    FILE *f;

    snprintf(g_ini, sizeof g_ini, "/tmp/test_wallpaper.%d.ini", (int)getpid());
    logexport_pass_digest(WEBPASS, digest);
    snprintf(g_cookie, sizeof g_cookie, "auth=deadbeef%.24s", digest);
    f = fopen(g_ini, "w");
    if (!f)
        return -1;
    fprintf(f, "[syscfg]\nwebuser = %s\nwebpass = %s\n", WEBUSER, digest);
    fclose(f);
    return 0;
}

static void query_build(const char *fmt)
{
    char sha[2 * WP_SHA_LEN + 1], sign[2 * WP_SIGN_LEN + 1];

    hex_encode(sha, g_sha, WP_SHA_LEN);
    hex_encode(sign, g_sign, WP_SIGN_LEN);
    snprintf(g_query, sizeof g_query, "format=%s&sha256=%s&signature=%s", fmt, sha, sign);
}

static stub_req post_req(size_t size)
{
    stub_req r;

    memset(&r, 0, sizeof r);
    r.method         = "POST";
    r.uri            = WEB_WALLPAPER_URI;
    r.query          = g_query;
    r.cookie         = g_cookie;
    r.content_type   = "application/octet-stream";
    r.content_length = (long long)size;
    r.body           = (const char *)g_file;
    r.body_len       = size;
    r.unread         = size;
    r.read_chunk     = 4000;
    r.is_ssl         = 1;
    return r;
}

static int status_reply(const stub_rsp *rsp, const char *tail)
{
    size_t n = strlen(rsp->body), t = strlen(tail);

    return !strncmp(rsp->body, STATUS_HEAD, sizeof STATUS_HEAD - 1) && n > t &&
            !strcmp(rsp->body + n - t, tail) && strstr(rsp->head, "application/json");
}

static int post(size_t size, stub_rsp *rsp)
{
    stub_req r = post_req(size);

    return stub_invoke(WEB_WALLPAPER_URI, &r, rsp);
}

static int chunk_decode(int i, const char *prefix, struct wp_header *hdr, uint8_t *payload)
{
    const char *p    = g_params[i];
    size_t      plen = strlen(prefix);

    if (strncmp(p, prefix, plen))
        return -1;
    p += plen;
    if (strlen(p) > WP_PKG_LEN || strlen(p) < 2 * (size_t)WP_HDR_LEN ||
            hex_decode((uint8_t *)hdr, p, WP_HDR_LEN) != 0)
        return -1;
    if (strlen(p) != 2 * ((size_t)WP_HDR_LEN + hdr->payload_len) ||
            hex_decode(payload, p + 2 * WP_HDR_LEN, hdr->payload_len) != 0)
        return -1;
    return 0;
}

static int chunks_match(size_t size, const char *prefix)
{
    struct wp_header hdr;
    uint8_t          payload[WP_PKG_LEN / 2];
    uint16_t         uid;
    size_t           off = 0;

    if (g_ncalls != 1 + (int)((size + WP_PAYLOAD_MAX - 1) / WP_PAYLOAD_MAX))
        return 0;
    if (chunk_decode(0, prefix, &hdr, payload) != 0 || memcmp(hdr.magic, "nano\0\0\0\0", 8) ||
            hdr.format_ver != WP_VER || hdr.header_len != WP_HDR_LEN || hdr.idx != 0 ||
            hdr.offset != 0 || hdr.file_size != size || hdr.uid == 0 ||
            hdr.payload_len != WP_SHA_LEN + WP_SIGN_LEN || memcmp(payload, g_sha, WP_SHA_LEN) ||
            memcmp(payload + WP_SHA_LEN, g_sign, WP_SIGN_LEN))
        return 0;
    uid = hdr.uid;
    for (int i = 1; i < g_ncalls; i++) {
        if (chunk_decode(i, prefix, &hdr, payload) != 0 || memcmp(hdr.magic, "nano", 4) ||
                hdr.format_ver != WP_VER || hdr.header_len != WP_HDR_LEN || hdr.uid != uid ||
                hdr.idx != i || hdr.offset != off || hdr.file_size != size ||
                hdr.payload_len == 0 || hdr.payload_len > WP_PAYLOAD_MAX ||
                (i < g_ncalls - 1 && hdr.payload_len != WP_PAYLOAD_MAX) ||
                memcmp(payload, g_file + off, hdr.payload_len))
            return 0;
        off += hdr.payload_len;
    }
    return off == size;
}

static uint16_t first_uid(void)
{
    struct wp_header hdr;
    uint8_t          payload[WP_PKG_LEN / 2];

    return g_ncalls && chunk_decode(0, PREFIX_PNG, &hdr, payload) == 0 ? hdr.uid : 0;
}

static void *post_run(void *arg)
{
    post_job *j = arg;

    j->rc = post(j->size, &j->rsp);
    return NULL;
}

static int dummy_handler(struct mg_connection *c, void *cbdata)
{
    (void)c;
    (void)cbdata;
    return 200;
}

static void *fake_ota_run(void *arg)
{
    fake_ota     *p   = arg;
    struct pollfd pfd = { p->listen_fd, POLLIN, 0 };
    mm_ota_msg    msg;
    char          buf[256];
    int           fd;

    if (poll(&pfd, 1, 10000) != 1)
        return NULL;
    fd = accept(p->listen_fd, NULL, NULL);
    if (fd < 0)
        return NULL;
    while (read(fd, buf, sizeof buf) > 0)
        ;
    pthread_mutex_lock(&g_lock);
    p->eof = 1;
    pthread_cond_broadcast(&g_cond);
    while (!p->release)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
    memset(&msg, 0, sizeof msg);
    msg.magic         = (int)MM_OTA_MAGIC;
    msg.type          = MM_OTA_MSG_TYPE;
    msg.upgrade.error = UPGRADE_SUCCEED;
    send(fd, &msg, sizeof msg, MSG_NOSIGNAL);
    close(fd);
    return NULL;
}

static int ota_listen(void)
{
    struct sockaddr_un sa;
    int                fd;

    snprintf(g_sock, sizeof g_sock, "/tmp/test_wallpaper.%d.sock", (int)getpid());
    unlink(g_sock);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, g_sock, sizeof sa.sun_path - 1);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *ota_post_run(void *arg)
{
    static const char body[] = "AUP-PAYLOAD-BYTES";
    ota_job          *j      = arg;
    stub_req          r;

    memset(&r, 0, sizeof r);
    r.method         = "POST";
    r.uri            = WEB_UPGRADE_URI;
    r.cookie         = g_cookie;
    r.content_type   = "multipart/form-data; boundary=----x";
    r.filename       = "firmware.aup";
    r.body           = body;
    r.body_len       = sizeof body - 1;
    r.content_length = (long long)(sizeof body - 1);
    r.is_ssl         = 1;
    j->rc            = stub_invoke(WEB_UPGRADE_URI, &r, &j->rsp);
    return NULL;
}

int main(void)
{
    stub_req r;
    stub_rsp rsp;
    int      rc;

    if (creds_write() != 0) {
        fprintf(stderr, "  FAIL: could not write the credentials ini\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof g_file; i++)
        g_file[i] = (uint8_t)(i * 7 + 3);
    for (size_t i = 0; i < sizeof g_sha; i++)
        g_sha[i] = (uint8_t)(0xa0 + i);
    for (size_t i = 0; i < sizeof g_sign; i++)
        g_sign[i] = (uint8_t)(0x11 * i);
    query_build("png");

    stub_reset();
    mg_set_request_handler(NULL, "/api/v1/", dummy_handler, NULL);
    web_wallpaper_register(NULL, g_ini);
    CHECK(stub_route(WEB_WALLPAPER_URI) &&
                    !strcmp(stub_route(WEB_WALLPAPER_URI), WEB_WALLPAPER_URI),
            "the wallpaper URI matches its own handler before the /api/v1/ prefix handler");
    CHECK(stub_route("/api/v1/summary") && !strcmp(stub_route("/api/v1/summary"), "/api/v1/"),
            "other /api/v1/ resources still reach the prefix handler");

    r        = post_req(2500);
    r.method = "GET";
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 405 && rsp.status == 405 &&
                    strstr(rsp.body, "\"Code\":505,\"Msg\":\"Method not allowed\"") &&
                    g_ncalls == 0,
            "a GET is refused 405");

    r        = post_req(2500);
    r.is_ssl = 0;
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 403 && strstr(rsp.body, "\"Code\":503,\"Msg\":\"HTTPS required\"") && g_ncalls == 0,
            "a POST over plain HTTP is refused 403");

    r        = post_req(2500);
    r.host   = "192.168.1.5";
    r.origin = "http://evil.example";
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 403 && strstr(rsp.body, "\"STATUS\":\"E\"") &&
                    strstr(rsp.body, "\"Code\":504,\"Msg\":\"Request refused\"}") && g_ncalls == 0,
            "a cross-origin POST is refused 403");

    r        = post_req(2500);
    r.cookie = NULL;
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 401 && status_reply(&rsp, NO_LOGIN_TAIL) && g_ncalls == 0,
            "a POST with no credentials is refused 401 with the Code 602 STATUS reply");

    r        = post_req(2500);
    r.cookie = NULL;
    r.token  = NO_TOKEN;
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 401 && status_reply(&rsp, STALE_TAIL) && g_ncalls == 0,
            "a forged Token header is refused 401 with the Code 602 STATUS reply");

    r        = post_req(2500);
    r.method = "DELETE";
    r.cookie = NULL;
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 401 && status_reply(&rsp, NO_LOGIN_TAIL) && g_ncalls == 0,
            "a DELETE with no credentials is refused 401 with the Code 602 STATUS reply");

    r        = post_req(2500);
    r.method = "DELETE";
    r.cookie = NULL;
    r.token  = NO_TOKEN;
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 401 && status_reply(&rsp, STALE_TAIL) && g_ncalls == 0,
            "a DELETE with a forged Token header is refused 401 with the Code 602 STATUS reply");

    {
        static const char *const bad[] = {
            NULL,
            "sha256=00&signature=00",
            "format=jpg",
            "format=png",
            "format=png&sha256=abc",
            "format=png&sha256=zz",
        };

        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            r       = post_req(2500);
            r.query = bad[i];
            rc      = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
            CHECK(rc == 400 && g_ncalls == 0,
                    "a missing or malformed format or sha256 is refused 400");
        }
    }
    {
        char q[sizeof g_query];

        snprintf(q, sizeof q, "%s", g_query);
        q[strlen(q) - 1] = '\0';
        r                = post_req(2500);
        r.query          = q;
        rc               = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
        CHECK(rc == 400 && strstr(rsp.body, "\"Code\":501") && g_ncalls == 0,
                "a signature one hex digit short is refused 400");
        q[strlen(q) - 1] = 'g';
        r.query          = q;
        rc               = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
        CHECK(rc == 400 && g_ncalls == 0, "a signature with a non-hex digit is refused 400");
    }

    rc = post(WP_SIZE_MIN - 1, &rsp);
    CHECK(rc == 413 && rsp.status == 413 && strstr(rsp.body, "\"Code\":501") &&
                    rsp.drained == WP_SIZE_MIN - 1 && g_ncalls == 0,
            "a body under 1 KiB is refused 413 after the body is discarded");
    {
        stub_req big = post_req(WP_SIZE_MAX);

        big.content_length = WP_SIZE_MAX + 1;
        big.unread         = WP_SIZE_MAX;
        rc                 = stub_invoke(WEB_WALLPAPER_URI, &big, &rsp);
        CHECK(rc == 413 && rsp.drained == 65536 && g_ncalls == 0,
                "a body over 300 KiB is refused 413 after at most 64 KiB is discarded");
        big.content_length = -1;
        rc                 = stub_invoke(WEB_WALLPAPER_URI, &big, &rsp);
        CHECK(rc == 411 && strstr(rsp.body, "\"Code\":501") && g_ncalls == 0,
                "a body without Content-Length is refused 411");
    }

    r        = post_req(2500);
    r.unread = 2000;
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 400 && strstr(rsp.body, "\"Code\":501") && g_ncalls == 0,
            "a body shorter than its Content-Length is refused 400 and nothing is forwarded");

    rc = post(2500, &rsp);
    CHECK(rc == 200 && rsp.status == 200 && strstr(rsp.head, "application/json"),
            "a 2500-byte png is answered 200 as JSON");
    CHECK(chunks_match(2500, PREFIX_PNG),
            "it is sent as the signature chunk and three data chunks: one uid, idx 0..3, "
            "offsets 0, 1024, 2048, the file bytes unchanged");
    CHECK(strstr(rsp.body, "\"Msg\":\"chunk 3\"") && strstr(rsp.body, "\"id\":1"),
            "the reply is the core reply to the last chunk, unchanged");
    CHECK(first_uid() != 0, "the upload carries a non-zero uid");
    calls_reset();

    r               = post_req(2500);
    r.cookie        = NULL;
    r.authorization = BASIC_AUTH;
    rc              = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 401 && strstr(rsp.body, "\"Code\":602,\"Msg\":\"Not logged in\"") && g_ncalls == 0,
            "HTTP Basic credentials are not a login: 401 Code 602, nothing forwarded");
    calls_reset();

    r = post_req(WP_SIZE_MIN);
    query_build("gif");
    r.query = g_query;
    rc      = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 200 && chunks_match(WP_SIZE_MIN, "0,wallpaper,gif,"),
            "a 1 KiB gif is the signature chunk and one full data chunk");
    calls_reset();
    query_build("png");

    rc = post(WP_SIZE_MAX, &rsp);
    CHECK(rc == 200 && chunks_match(WP_SIZE_MAX, PREFIX_PNG),
            "a 300 KiB file is the signature chunk and 300 data chunks of 1024 bytes");
    calls_reset();

    g_fail_at = 2;
    rc        = post(5000, &rsp);
    CHECK(rc == 400 && rsp.status == 400 && g_ncalls == 3,
            "a chunk the core answers E stops the upload: no later chunk is sent");
    CHECK(strstr(rsp.body, "\"STATUS\":\"E\"") && strstr(rsp.body, "chunk 2"),
            "the reply is the core E reply of the failing chunk");
    calls_reset();

    g_fail_at = 0;
    rc        = post(5000, &rsp);
    CHECK(rc == 400 && g_ncalls == 1, "a refused signature chunk sends no data chunk");
    calls_reset();

    g_dead = 1;
    rc     = post(5000, &rsp);
    CHECK(rc == 504 && rsp.status == 504 && strstr(rsp.body, "\"STATUS\":\"E\",\"When\":") &&
                    strstr(rsp.body, "\"Code\":500,\"Msg\":\"Command timeout\"}],\"id\":1}") &&
                    g_ncalls == 1,
            "an unreachable core is 504 Code 500 and the upload stops");
    calls_reset();

    g_trunc = 1;
    rc      = post(5000, &rsp);
    CHECK(rc == 500 && rsp.status == 500 &&
                    strstr(rsp.body, "\"Code\":500,\"Msg\":\"Reply too long: not sent\"") &&
                    g_ncalls == 1,
            "a core reply too long for the buffer is 500 Code 500 and the upload stops");
    calls_reset();

    r        = post_req(0);
    r.method = "DELETE";
    r.query  = NULL;
    rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 200 && g_ncalls == 1 && !strcmp(g_params[0], "0,wallpaper,none") &&
                    strstr(rsp.body, "chunk 0"),
            "DELETE sends ascset 0,wallpaper,none and forwards the core reply");
    calls_reset();
    g_fail_at = 0;
    rc        = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
    CHECK(rc == 400 && strstr(rsp.body, "\"STATUS\":\"E\""), "a DELETE the core refuses is 400");
    calls_reset();

    {
        post_job  job = { .size = 3000 };
        pthread_t th;
        int       started;

        g_hold  = 1;
        started = pthread_create(&th, NULL, post_run, &job) == 0;
        pthread_mutex_lock(&g_lock);
        while (started && !g_in_call)
            pthread_cond_wait(&g_cond, &g_lock);
        pthread_mutex_unlock(&g_lock);
        rc = post(3000, &rsp);
        CHECK(started && rc == 409 && strstr(rsp.body, "\"Code\":500") && g_ncalls == 1,
                "a second upload while one is in progress is refused 409");
        r        = post_req(0);
        r.method = "DELETE";
        rc       = stub_invoke(WEB_WALLPAPER_URI, &r, &rsp);
        CHECK(rc == 409 && g_ncalls == 1, "a DELETE while an upload is in progress is 409");
        pthread_mutex_lock(&g_lock);
        g_hold = 0;
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
        if (started)
            pthread_join(th, NULL);
        CHECK(job.rc == 200 && chunks_match(3000, PREFIX_PNG),
                "the first upload completes untouched");
        calls_reset();
        rc = post(3000, &rsp);
        CHECK(rc == 200, "the finished upload released the wallpaper lock");
        calls_reset();
    }

    {
        fake_ota  ota = { .listen_fd = ota_listen() };
        ota_job   job;
        pthread_t peer, poster;
        int       started;

        memset(&job, 0, sizeof job);
        web_upload_register(NULL, g_sock, g_ini);
        started = ota.listen_fd >= 0 && pthread_create(&peer, NULL, fake_ota_run, &ota) == 0;
        if (started && pthread_create(&poster, NULL, ota_post_run, &job) != 0) {
            pthread_mutex_lock(&g_lock);
            ota.release = 1;
            pthread_cond_broadcast(&g_cond);
            pthread_mutex_unlock(&g_lock);
            pthread_join(peer, NULL);
            started = 0;
        }
        pthread_mutex_lock(&g_lock);
        while (started && !ota.eof)
            pthread_cond_wait(&g_cond, &g_lock);
        pthread_mutex_unlock(&g_lock);
        rc = post(3000, &rsp);
        CHECK(started && rc == 409 && strstr(rsp.body, "\"Code\":500") && g_ncalls == 0,
                "a wallpaper upload while a firmware upload is in progress is refused 409");
        {
            ota_job second;

            memset(&second, 0, sizeof second);
            ota_post_run(&second);
            CHECK(started && second.rc == 409 && second.rsp.status == 409 &&
                            strstr(second.rsp.body,
                                    "\"Code\":500,\"Msg\":\"" WEB_UPLOAD_BUSY_MSG "\"}]"),
                    "a second firmware upload while one is in progress is refused 409 with "
                    "the busy reply");
        }
        pthread_mutex_lock(&g_lock);
        ota.release = 1;
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
        if (started) {
            pthread_join(poster, NULL);
            pthread_join(peer, NULL);
        }
        CHECK(job.rc == 200, "the firmware upload finishes");
        rc = post(3000, &rsp);
        CHECK(rc == 200, "after the firmware upload a wallpaper upload is accepted again");
        calls_reset();
        if (ota.listen_fd >= 0)
            close(ota.listen_fd);
        unlink(g_sock);
    }

    unlink(g_ini);
    printf(fails ? "FAILED (%d)\n" : "wallpaper OK\n", fails);
    return fails ? 1 : 0;
}
