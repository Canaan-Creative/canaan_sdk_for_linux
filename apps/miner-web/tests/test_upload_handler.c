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
#include "logexport.h"
#include "webcred.h"
#include "webtoken.h"
#include "aup.h"
#include "ota_proto.h"
#include "stub_civetweb.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "osif_logpath.h"

/* Counts a failed cond in fails and prints msg as FAIL or ok */
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

#define MULTIPART_CT "multipart/form-data; boundary=----x"
#define WEBUSER      "miner"
#define WEBPASS      "upload-pass" /* web password, stored as its digest */
/* HTTP Basic credentials for WEBUSER:WEBPASS */
#define BASIC_AUTH "Basic bWluZXI6dXBsb2FkLXBhc3M="
#define NO_TOKEN \
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.e30.AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#define STATUS_HEAD   "{\"STATUS\":[{\"STATUS\":\"E\",\"When\":"
#define STALE_TAIL    "\"Code\":602,\"Msg\":\"Token expired, request a new one\"}],\"id\":1}"
#define NO_LOGIN_TAIL "\"Code\":602,\"Msg\":\"Not logged in\"}],\"id\":1}"
#define REFUSED_TAIL  "\"Code\":504,\"Msg\":\"Request refused\"}],\"id\":1}"
#define METHOD_TAIL   "\"Code\":505,\"Msg\":\"Method not allowed\"}],\"id\":1}"
#define NOSOCK_TAIL   "\"Code\":500,\"Msg\":\"" WEB_UPLOAD_NOSOCK_MSG "\"}],\"id\":1}"

#define PEER_ACCEPT_WAIT_MS 10000 /* max wait for the uploader to connect, ms */
#define PEER_READ_WAIT_S    10    /* receive timeout of the stand-in, s */

/* mm_ota stand-in: reads the upload, replies with a status */
typedef struct {
    int    connected;   /* 1 once a client was accepted */
    int    replied;     /* 1 once the whole status reply was sent */
    size_t got;         /* payload bytes received */
    char   payload[64]; /* first bytes received from the client */
    int8_t error;       /* error code returned in the status reply */
} fake_mm_ota;

static char g_weblog[] = "/tmp/test_upload_handler_web_XXXXXX";

#undef MINER_WEB_LOG_PATH
#define MINER_WEB_LOG_PATH g_weblog
#include "web_logfmt.h"

static int fails;

static char g_sock[96];      /* path of the listening mm_ota stand-in socket */
static char g_dead_sock[96]; /* socket path nothing listens on */
static char g_ini[96];
static char g_cookie[96];  /* auth cookie matching the stored password digest */
static int  g_listen = -1; /* listening socket fd, -1 when not open */

static int listener_open(void)
{
    struct sockaddr_un sa;
    int                fd;

    snprintf(g_sock, sizeof g_sock, "/tmp/test_upload_handler.%d.sock", (int)getpid());
    snprintf(g_dead_sock, sizeof g_dead_sock, "/tmp/test_upload_handler.%d.absent", (int)getpid());
    unlink(g_sock);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, g_sock, sizeof sa.sun_path - 1);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 4) != 0 ||
            fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    g_listen = fd;
    return 0;
}

static int listener_accepted(void)
{
    return accept(g_listen, NULL, NULL);
}

static int nothing_connected(void)
{
    int fd = listener_accepted();

    if (fd < 0)
        return 1;
    close(fd);
    return 0;
}

static void *fake_mm_ota_run(void *arg)
{
    fake_mm_ota   *p   = arg;
    struct pollfd  pfd = { g_listen, POLLIN, 0 };
    struct timeval tv  = { PEER_READ_WAIT_S, 0 };
    mm_ota_msg     msg;
    int            fd;

    if (poll(&pfd, 1, PEER_ACCEPT_WAIT_MS) != 1)
        return NULL;
    fd = listener_accepted();
    if (fd < 0)
        return NULL;
    p->connected = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (p->got < sizeof p->payload) {
        ssize_t n = read(fd, p->payload + p->got, sizeof p->payload - p->got);

        if (n <= 0)
            break;
        p->got += (size_t)n;
    }
    msg.magic           = (int)MM_OTA_MAGIC;
    msg.type            = MM_OTA_MSG_TYPE;
    msg.upgrade.data    = 0;
    msg.upgrade.stat    = 0;
    msg.upgrade.percent = 100;
    msg.upgrade.error   = p->error;
    p->replied          = send(fd, &msg, sizeof msg, MSG_NOSIGNAL) == (ssize_t)sizeof msg;
    close(fd);
    return NULL;
}

static int creds_write(void)
{
    char  digest[65];
    FILE *f;

    snprintf(g_ini, sizeof g_ini, "/tmp/test_upload_handler.%d.ini", (int)getpid());
    logexport_pass_digest(WEBPASS, digest);
    snprintf(g_cookie, sizeof g_cookie, "auth=deadbeef%.24s", digest);

    f = fopen(g_ini, "w");
    if (!f)
        return -1;
    fprintf(f, "[syscfg]\nwebuser = %s\nwebpass = %s\n", WEBUSER, digest);
    fclose(f);
    return 0;
}

static stub_req post_req(void)
{
    stub_req r;

    memset(&r, 0, sizeof r);
    r.method         = "POST";
    r.uri            = WEB_UPGRADE_URI;
    r.cookie         = g_cookie;
    r.content_length = 8;
    r.form_rc        = -1;
    r.is_ssl         = 1;
    return r;
}

static int status_reply(const stub_rsp *rsp, const char *tail)
{
    size_t n = strlen(rsp->body), t = strlen(tail);

    return !strncmp(rsp->body, STATUS_HEAD, sizeof STATUS_HEAD - 1) && n > t &&
            !strcmp(rsp->body + n - t, tail) && strstr(rsp->head, "application/json");
}

static int lock_is_free(void)
{
    stub_req r = post_req();
    stub_rsp rsp;
    int      free_now;

    r.content_type = MULTIPART_CT;
    web_upload_register(NULL, g_dead_sock, g_ini);
    stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    web_upload_register(NULL, g_sock, g_ini);

    free_now = rsp.status == 503 && status_reply(&rsp, NOSOCK_TAIL);
    return free_now && nothing_connected();
}

static int weblog_has(const char *s)
{
    char   wl[16384];
    FILE  *f  = fopen(g_weblog, "r");
    size_t wn = f ? fread(wl, 1, sizeof wl - 1, f) : 0;

    if (f)
        fclose(f);
    wl[wn] = '\0';
    return strstr(wl, s) != NULL;
}

static int warn_level_refusal_logged(void)
{
    pid_t pid = fork();
    int   st;

    if (pid < 0)
        return 0;
    if (pid == 0) {
        stub_req r = post_req();
        stub_rsp rsp;

        setenv("MINER_LOG_LEVEL", "WARN", 1);
        r.cookie       = NULL;
        r.content_type = MULTIPART_CT;
        web_upload_register(NULL, g_sock, g_ini);
        _exit(stub_invoke(WEB_UPGRADE_URI, &r, &rsp) == 401 ? 0 : 1);
    }
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
            weblog_has("upload refused (HTTP 401): not logged in\n") &&
            !weblog_has("upload started");
}

int main(void)
{
    stub_req r;
    stub_rsp rsp;
    char     buf[64];
    ssize_t  n;
    int      rc, peer, accepted, ok;
    int      wfd = mkstemp(g_weblog);

    if (wfd < 0 || close(wfd) != 0 || web_log_web_fd() < 0) {
        fprintf(stderr, "  FAIL: could not create the web.log stand-in\n");
        return 1;
    }
    if (creds_write() != 0 || listener_open() != 0) {
        fprintf(stderr, "  FAIL: could not set up the stand-in mm_ota listener\n");
        return 1;
    }

    stub_reset();
    CHECK(warn_level_refusal_logged(),
            "with MINER_LOG_LEVEL=WARN an upload refused 401 still reaches web.log, "
            "and the INFO \"upload started\" line does not");

    web_upload_register(NULL, g_sock, g_ini);

    ok = stub_handler_count() == 1 && stub_handler_uri(0) &&
            !strcmp(stub_handler_uri(0), WEB_UPGRADE_URI);
    CHECK(ok, "the upload handler is registered on " WEB_UPGRADE_URI " only");

    r              = post_req();
    r.method       = "GET";
    r.content_type = MULTIPART_CT;
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 405 && rc == 405,
            "a GET is refused 405, and the code returned to civetweb is the code "
            "written on the wire");
    CHECK(status_reply(&rsp, METHOD_TAIL), "the 405 body is the Code 505 STATUS reply");
    CHECK(rsp.closed == 1, "the refused connection is closed exactly once");
    CHECK(rsp.form_calls == 0, "the method refusal never hands the body to the form parser");
    CHECK(nothing_connected(),
            "the method refusal opens no upload socket: nothing connected to the "
            "stand-in mm_ota listener");
    CHECK(lock_is_free(),
            "the method refusal took no upgrade lock: the next multipart upload "
            "still reaches the socket open and is refused for the socket, not as "
            "'Another firmware upgrade is in progress'");

    r              = post_req();
    r.content_type = MULTIPART_CT;
    r.host         = "evil.example";
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 403 && rc == 403 && status_reply(&rsp, REFUSED_TAIL),
            "a POST with a foreign Host is refused 403 with the Code 504 STATUS reply");
    r.host   = "192.168.1.5";
    r.origin = "http://evil.example";
    rc       = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 403 && rc == 403 && status_reply(&rsp, REFUSED_TAIL) &&
                    !strstr(rsp.body, "origin"),
            "a POST whose Origin is not the Host is refused 403, the reason kept out of the "
            "reply");
    CHECK(rsp.form_calls == 0 && nothing_connected() && lock_is_free(),
            "the 403 opens no upload socket and takes no upgrade lock");

    r              = post_req();
    r.content_type = MULTIPART_CT;
    r.is_ssl       = 0;
    r.unread       = 4096;
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 403 && rc == 403 &&
                    strstr(rsp.body, "\"Code\":503,\"Msg\":\"HTTPS required\""),
            "an authenticated multipart POST that did not arrive over HTTPS is refused 403");
    CHECK(rsp.drained == 4096 && rsp.form_calls == 0 && nothing_connected() && lock_is_free(),
            "the plain-HTTP refusal drains the body, opens no upload socket and takes no "
            "upgrade lock");

    r              = post_req();
    r.method       = "GET";
    r.cookie       = NULL;
    r.content_type = "application/octet-stream";
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 405 && rc == 405,
            "the method gate runs first: an unauthenticated GET carrying a raw "
            "body is 405, not 401 and not 400");

    r              = post_req();
    r.cookie       = NULL;
    r.content_type = MULTIPART_CT;
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 401 && rc == 401,
            "a POST with no cookie and no Authorization header is refused 401");
    CHECK(status_reply(&rsp, NO_LOGIN_TAIL),
            "a refusal without a Token header is the Code 602 Not logged in STATUS reply");
    CHECK(rsp.form_calls == 0, "the auth refusal never hands the body to the form parser");
    CHECK(nothing_connected(), "the auth refusal opens no upload socket");
    CHECK(lock_is_free(), "the auth refusal took no upgrade lock");

    r               = post_req();
    r.cookie        = NULL;
    r.authorization = BASIC_AUTH;
    r.content_type  = MULTIPART_CT;
    rc              = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 401 && rc == 401 && status_reply(&rsp, NO_LOGIN_TAIL),
            "HTTP Basic credentials are not a login: 401 Code 602 Not logged in");
    CHECK(rsp.form_calls == 0 && nothing_connected() && lock_is_free(),
            "the Basic refusal reaches neither the form parser nor mm_miner");

    r              = post_req();
    r.cookie       = "auth=deadbeef000000000000000000000000";
    r.content_type = MULTIPART_CT;
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 401 && rc == 401,
            "a POST whose cookie proof does not match webpass is refused 401");

    r              = post_req();
    r.cookie       = NULL;
    r.content_type = "application/octet-stream";
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 401 && rc == 401,
            "the auth gate runs before the Content-Type gate: an unauthenticated "
            "raw body is 401, not 400");

    r              = post_req();
    r.content_type = NULL;
    r.unread       = 4096;
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 400 && rc == 400,
            "an authenticated POST with no Content-Type at all is refused 400");
    CHECK(strstr(rsp.body, "\"Code\":501,") && strstr(rsp.body, "multipart") != NULL,
            "the 400 body is Code 501 and says a multipart body is required");
    CHECK(rsp.drained == 4096, "the refusal drains the unread body before closing the connection");

    r              = post_req();
    r.content_type = NULL;
    r.unread       = 200000;
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 400 && rsp.drained == 65536,
            "a refusal discards at most 64 KiB of a larger unread body");
    CHECK(rsp.form_calls == 0, "the Content-Type refusal never hands the body to the form parser");
    CHECK(nothing_connected(),
            "the Content-Type refusal opens no upload socket: the refusal is "
            "decided before web_upload_open, so no byte can reach mm_miner");
    CHECK(lock_is_free(),
            "the Content-Type refusal is taken BEFORE the upgrade lock: a refusal "
            "after it would latch the upgrade path shut until reboot");

    r              = post_req();
    r.content_type = "application/octet-stream";
    rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    CHECK(rsp.status == 400 && rc == 400,
            "a raw application/octet-stream body from an authenticated client is "
            "refused 400, not ingested");
    CHECK(nothing_connected(), "...and no byte of that raw body reaches mm_miner");
    CHECK(lock_is_free(), "...and the upgrade lock was never taken");

    {
        mc_webcred creds;
        char       token[WEB_TOKEN_MAX] = "";

        CHECK(web_creds_load(g_ini, &creds) == 0 &&
                        web_token_issue(&creds, token, sizeof token) == 0,
                "a token is issued for the stored credentials");
        r              = post_req();
        r.cookie       = NULL;
        r.token        = token;
        r.content_type = "application/octet-stream";
        rc             = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
        CHECK(rsp.status == 400 && rc == 400,
                "a valid Token header authenticates an upload: the raw body is refused 400, "
                "not 401");
        r.token = NO_TOKEN;
        rc      = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
        CHECK(rsp.status == 401 && rc == 401, "a forged token is refused 401");
        CHECK(status_reply(&rsp, STALE_TAIL),
                "a forged token is answered with the router's Code 602 STATUS reply");
        r.token = "abc";
        rc      = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
        CHECK(rsp.status == 401 && rc == 401 && status_reply(&rsp, STALE_TAIL),
                "a malformed Token header is refused 401 with the Code 602 STATUS reply");
        r       = post_req();
        r.token = NO_TOKEN;
        rc      = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
        CHECK(rsp.status == 401 && rc == 401 && status_reply(&rsp, STALE_TAIL),
                "a Token header decides alone: a valid cookie next to a forged token is "
                "refused 401 with the Code 602 STATUS reply");
        CHECK(nothing_connected(), "the token refusals open no upload socket");
        CHECK(lock_is_free(), "the token refusals took no upgrade lock");
        memset(&creds, 0, sizeof creds);
    }

    static const char body[] = "AUP-PAYLOAD-BYTES";

    r                = post_req();
    r.content_type   = MULTIPART_CT;
    r.filename       = "firmware.aup";
    r.body           = body;
    r.body_len       = sizeof body - 1;
    r.content_length = (long long)(2 * (sizeof body - 1));
    rc               = stub_invoke(WEB_UPGRADE_URI, &r, &rsp);
    peer             = listener_accepted();
    accepted         = peer >= 0;
    CHECK(accepted,
            "a multipart POST from an authenticated client passes every gate and "
            "opens the upload socket to mm_miner");
    CHECK(rsp.form_calls == 1,
            "...and the multipart body is handed to the form parser exactly once");
    n = accepted ? read(peer, buf, sizeof buf) : -1;
    CHECK(n == (ssize_t)r.body_len && !memcmp(buf, body, r.body_len),
            "...and the firmware bytes reach mm_miner unchanged");
    CHECK(rsp.status == 400 && rc == 400 && strstr(rsp.body, "\"Code\":501,") &&
                    strstr(rsp.body, "truncated") != NULL,
            "a body the form parser could not finish is reported as truncated, "
            "not as an upgrade result");
    if (accepted)
        close(peer);
    CHECK(lock_is_free(), "the truncated upload released the upgrade lock");

    fake_mm_ota peer_ota;
    pthread_t   peer_thread;
    int         peer_started;

    memset(&peer_ota, 0, sizeof peer_ota);
    peer_ota.error = UPGRADE_SUCCEED;
    peer_started   = pthread_create(&peer_thread, NULL, fake_mm_ota_run, &peer_ota) == 0;
    CHECK(peer_started, "a stand-in mm_ota peer is accepting for the success path");

    r                = post_req();
    r.content_type   = MULTIPART_CT;
    r.filename       = "firmware.aup";
    r.body           = body;
    r.body_len       = sizeof body - 1;
    r.content_length = (long long)(sizeof body - 1);
    r.form_rc        = 0;
    rc               = peer_started ? stub_invoke(WEB_UPGRADE_URI, &r, &rsp) : -1;
    if (peer_started)
        pthread_join(peer_thread, NULL);

    CHECK(peer_ota.connected && peer_ota.got == r.body_len &&
                    !memcmp(peer_ota.payload, body, r.body_len),
            "the whole body reaches mm_miner before the device is asked for its "
            "verdict");
    CHECK(peer_ota.replied, "...and the stand-in device answers with a status message");
    CHECK(rsp.status == 200 && rc == 200,
            "a readable device verdict is answered 200, not the 202 that means "
            "the device reported no status");
    CHECK(!strncmp(rsp.body, "{\"STATUS\":[{\"STATUS\":\"S\",\"When\":", 32) &&
                    strstr(rsp.body, ",\"Code\":200,\"Msg\":\"Upgrade ok(1), percent(100)\"}]") &&
                    strstr(rsp.head, "application/json"),
            "...and the body is the STATUS S Code 200 reply whose Msg upgrade.html and "
            "mmupgrade parse");
    CHECK(rsp.closed == 1, "the answered connection is closed exactly once");
    CHECK(lock_is_free(), "the completed upload released the upgrade lock");

    {
        char   wl[16384];
        FILE  *f  = fopen(g_weblog, "r");
        size_t wn = f ? fread(wl, 1, sizeof wl - 1, f) : 0;

        if (f)
            fclose(f);
        wl[wn] = '\0';
        CHECK(strstr(wl, "upload refused (HTTP 401): not logged in\n") != NULL,
                "web.log records the upload refused for no login");
        CHECK(strstr(wl, "upload refused (HTTP 405): GET request") != NULL,
                "web.log records the upload refused for the method");
        CHECK(strstr(wl, "upload refused (HTTP 403): HTTPS required\n") != NULL,
                "web.log records the upload refused for plain HTTP");
        CHECK(strstr(wl, "upload finished: upgrade service answered") != NULL,
                "web.log records the finished upload");
        CHECK(strstr(wl, "firmware upload refused") == NULL &&
                        strstr(wl, "firmware upload finished") == NULL,
                "an upload event web_log() records is not written to web.log a second time");
    }

    close(g_listen);
    unlink(g_sock);
    unlink(g_ini);
    unlink(g_weblog);

    printf(fails ? "FAILED (%d)\n" : "upload_handler OK\n", fails);
    return fails ? 1 : 0;
}
