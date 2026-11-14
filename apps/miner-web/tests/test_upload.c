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
#include "aup.h"
#include <stdio.h>
#include <string.h>

/* Counts a failed cond in fails and prints msg as FAIL or ok */
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

static int status_is(const char *out, size_t len, const char *st, const char *tail)
{
    char   head[64];
    size_t n = strlen(out), t = strlen(tail);

    snprintf(head, sizeof head, "{\"STATUS\":[{\"STATUS\":\"%s\",\"When\":", st);
    return len == n && !strncmp(out, head, strlen(head)) && n > t && !strcmp(out + n - t, tail);
}

int main(void)
{
    web_upload_status st;
    char              out[512];
    size_t            len;

    memset(&st, 0, sizeof st);
    st.valid   = 1;
    st.percent = 100;
    st.error   = UPGRADE_SUCCEED;
    len        = web_upload_reply(&st, out, sizeof out);
    CHECK(status_is(out, len, "S",
                  ",\"Code\":200,\"Msg\":\"Upgrade ok(1), percent(100)\"}],\"id\":1}"),
            "a completed upgrade is STATUS S Code 200 with the Msg the PC tool parses");

    st.percent = 37;
    st.error   = UPGRADE_VERIFY_ERR;
    len        = web_upload_reply(&st, out, sizeof out);
    CHECK(status_is(out, len, "E",
                  ",\"Code\":500,\"Msg\":\"Upgrade failed(-4), percent(37)\"}],\"id\":1}"),
            "a rejected image is STATUS E Code 500 with its own error code and percent");
    CHECK(strstr(out, "UNVERIFIED") == NULL, "no verification-stubbed suffix is appended");

    memset(&st, 0, sizeof st);
    len = web_upload_reply(&st, out, sizeof out);
    CHECK(status_is(out, len, "S",
                  ",\"Code\":200,\"Msg\":\"Upgrade status unknown, device reported no upgrade "
                  "result\"}],\"id\":1}"),
            "a status that never arrived is STATUS S Code 200, not rendered as error 0");
    CHECK(strstr(out, "failed(0)") == NULL && strstr(out, "ok(0)") == NULL,
            "the no-status reply carries no parseable error code");

    memset(&st, 0, sizeof st);
    st.valid   = 1;
    st.percent = 0;
    st.error   = UPGRADE_SW_ERR;
    len        = web_upload_timeout_reply(&st, out, sizeof out);
    CHECK(status_is(out, len, "E",
                  ",\"Code\":500,\"Msg\":\"Upgrade failed(-2), percent(0)\"}],\"id\":1}"),
            "the write-timeout reply reports the status read back, not 0");

    st.error   = UPGRADE_PARTITION_ERR;
    st.percent = 66;
    len        = web_upload_timeout_reply(&st, out, sizeof out);
    CHECK(status_is(out, len, "E",
                  ",\"Code\":500,\"Msg\":\"Upgrade failed(-5), percent(66)\"}],\"id\":1}"),
            "...including the percent the device had reached");
    {
        char normal[128];

        web_upload_reply(&st, normal, sizeof normal);
        CHECK(!strcmp(strstr(out, ",\"Code\""), strstr(normal, ",\"Code\"")),
                "a write-timeout reply with a status carries the same Code and Msg as the normal "
                "failure reply mmupgrade and upgrade.html parse");
    }

    memset(&st, 0, sizeof st);
    len = web_upload_timeout_reply(&st, out, sizeof out);
    CHECK(status_is(out, len, "E",
                  ",\"Code\":500,\"Msg\":\"Upgrade status unknown, the device stopped receiving "
                  "the image\"}],\"id\":1}"),
            "an unreadable status after a write timeout is not rendered as error 0");
    CHECK(strstr(out, "failed(0)") == NULL && strstr(out, "ok(0)") == NULL,
            "the no-status timeout reply carries no parseable error code");

    CHECK(web_upload_reply(&st, out, 0) == 0 && web_upload_timeout_reply(&st, out, 0) == 0,
            "a zero-capacity buffer writes nothing");

    CHECK(UPLOAD_STATUS_WAIT_S >= MM_OTA_PEER_STALL_S,
            "the wait for the final status is at least the peer-stall budget: "
            "mm_ota flushes the u-boot env before it replies, and a shorter wait "
            "reports a SUCCEEDED upgrade as 'status unknown'");
    CHECK(UPLOAD_WRITE_STALL_S >= MM_OTA_PEER_STALL_S,
            "the write-stall budget is at least the peer-stall budget: mm_ota stops "
            "reading for a whole partition erase, and a shorter budget aborts a "
            "healthy flash");
    CHECK(UPLOAD_STATUS_WAIT_S == UPLOAD_WRITE_STALL_S,
            "both budgets are the same constant, so neither can drift alone");
    CHECK(UPLOAD_BODY_STALL_S == UPLOAD_WRITE_STALL_S,
            "the client-side body budget is that same constant: mm_ota stops reading "
            "for a whole partition erase, and the web end must not give up first");

    CHECK(!web_upload_body_stalled(1000, 1000 + UPLOAD_BODY_STALL_S - 1),
            "a gap one second short of the budget is not a stall");
    CHECK(web_upload_body_stalled(1000, 1000 + UPLOAD_BODY_STALL_S),
            "a gap of exactly the budget is a stall");

    {
        const int gap     = UPLOAD_BODY_STALL_S / 2;
        time_t    last    = 1000;
        int       refused = 0;

        for (int i = 0; i < 100; i++) {
            time_t now = last + gap;

            refused += web_upload_body_stalled(last, now) != 0;
            last = now;
        }
        CHECK(refused == 0,
                "100 body chunks, each one half-budget after the last, are ALL "
                "accepted: the bound is measured from the last progress, so it caps "
                "the gap between bytes and imposes no minimum transfer rate");
        CHECK(web_upload_body_stalled(1000, last),
                "...while that same elapsed time measured from the START of the body "
                "is a stall by this predicate — which is what a total bound did to a "
                "slow but perfectly healthy client on phone WiFi");
    }

    CHECK(!strcmp(WEB_UPLOAD_BUSY_MSG, "Another firmware upgrade is in progress"),
            "a second concurrent upgrade is refused with the API Msg");
    CHECK(!strcmp(WEB_UPLOAD_NOSOCK_MSG, "Device not ready for an upgrade, try again shortly"),
            "an unreachable upgrade socket answers the API Msg");

    CHECK(web_upload_ct_is_multipart("multipart/form-data; boundary=----x"),
            "the browser's FormData Content-Type is accepted");
    CHECK(web_upload_ct_is_multipart("multipart/form-data"),
            "curl -F without an explicit boundary is accepted");
    CHECK(web_upload_ct_is_multipart("MULTIPART/FORM-DATA; boundary=x"),
            "the media type is matched case-insensitively, as HTTP requires");
    CHECK(web_upload_ct_is_multipart("multipart/mixed; boundary=x"),
            "the gate is the 9-byte multipart prefix, not form-data only");

    CHECK(!web_upload_ct_is_multipart(NULL),
            "a request with no Content-Type is refused, not read as a raw body");
    CHECK(!web_upload_ct_is_multipart(""), "an empty Content-Type is refused");
    CHECK(!web_upload_ct_is_multipart("application/octet-stream"),
            "a raw .aup body is refused: the upload path is multipart-only");
    CHECK(!web_upload_ct_is_multipart("text/plain"), "text/plain is refused");
    CHECK(!web_upload_ct_is_multipart("multi"),
            "a Content-Type shorter than the prefix is refused, not over-read");

    printf(fails ? "FAILED (%d)\n" : "upload OK\n", fails);
    return fails ? 1 : 0;
}
