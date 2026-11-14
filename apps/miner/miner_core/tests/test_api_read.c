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
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>

#include "../../tests/include/tst_check.h"

#include "mc_api_server.c"

#define SLOW_READ_GAP_MS 20 /* pause between the slow reader's reads, ms */
#define RC(str)          req_complete((str), strlen(str))

/* Writer-thread argument: socket fd, request bytes, lengths of the first and second write */
struct wr {
    int         fd;
    const char *buf;
    size_t      half, rest;
};

static size_t make_wp_json(char *out, size_t cap, size_t hexlen)
{
    size_t w =
            (size_t)snprintf(out, cap, "{\"command\":\"ascset\",\"parameter\":\"0,wallpaper,png,");
    for (size_t i = 0; i < hexlen && w < cap - 4; i++)
        out[w++] = "0123456789abcdef"[i & 15];
    w += (size_t)snprintf(out + w, cap - w, "\"}");
    return w;
}

static void *writer(void *a)
{
    struct wr      *w  = a;
    ssize_t         rc = write(w->fd, w->buf, w->half);
    struct timespec ts = { .tv_nsec = 5 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    rc = write(w->fd, w->buf + w->half, w->rest);
    (void)rc;
    return NULL;
}

static void *slow_reader(void *a)
{
    int             fd = *(int *)a;
    char            b[16];
    struct timespec ts = { .tv_nsec = SLOW_READ_GAP_MS * 1000L * 1000L };
    while (read(fd, b, sizeof b) > 0)
        nanosleep(&ts, NULL);
    return NULL;
}

int main(void)
{
    CHECK(RC("{\"command\":\"version\"}"), "balanced JSON is complete");
    CHECK(!RC("{\"command\":\"asc"), "JSON with an unclosed brace is incomplete");
    CHECK(RC("{\"a\":{\"b\":1}}"), "nested JSON completes only when the outer brace closes");
    CHECK(!RC("{\"a\":{\"b\":1}"), "...and stays incomplete while the outer brace is still open");
    CHECK(RC("version"), "a bare text command (no ascset) is complete on the first read");
    CHECK(RC("version\n"), "a newline-terminated text line is complete");
    CHECK(!RC("ascset|0,wallpaper,png,ab"),
            "an unterminated text ascset is held open (its arg can span segments)");
    CHECK(RC("ascset|0,ledmode,1\n"), "...but a newline closes even an ascset");
    CHECK(!RC("  {\"command\":\"x\""), "leading whitespace does not hide an unbalanced brace");
    CHECK(!RC("{\"command\":\"ascset\",\"parameter\":\"0,x,a}"),
            "a brace inside a string does not close the request");
    CHECK(!RC("{\"command\":\"ascset\",\"parameter\":\"0,x,\\\"}"),
            "an escaped quote does not end the string");
    CHECK(RC("{\"command\":\"ascset\",\"parameter\":\"0,x,\\\"}{\"}"),
            "the brace after the string closes the request");

    char req[MC_API_REQ_CAP];

    char   big[MC_API_REQ_CAP];
    size_t biglen = make_wp_json(big, sizeof big, 2000);
    CHECK(biglen > 1024 && biglen < MC_API_REQ_CAP - 1,
            "the test wallpaper request is bigger than 1024 B (%zu B)", biglen);
    {
        int sp[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair up");
        CHECK(write(sp[1], big, biglen) == (ssize_t)biglen, "wrote the full request");
        int    trunc = -1;
        size_t n     = api_read_request(sp[0], req, sizeof req, &trunc, &g_chan[1]);
        CHECK(n == biglen && memcmp(req, big, biglen) == 0 && req[n] == 0,
                "a full %zu B wallpaper request is read whole and NUL-terminated", biglen);
        CHECK(trunc == 0, "...and is not flagged truncated");
        close(sp[0]);
        close(sp[1]);
    }

    {
        char   maxwp[MC_API_REQ_CAP];
        size_t maxlen = make_wp_json(maxwp, sizeof maxwp, 2104);
        size_t margin = (MC_API_REQ_CAP - 1) - maxlen;
        CHECK(maxlen < MC_API_REQ_CAP - 1 && margin > 1024,
                "the real WP_PKG_LEN=2104 wallpaper request (%zu B) fits the %d-byte "
                "cap with %zu B to spare",
                maxlen, MC_API_REQ_CAP - 1, margin);
        int sp[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair up (max wallpaper)");
        CHECK(write(sp[1], maxwp, maxlen) == (ssize_t)maxlen, "wrote the max wallpaper request");
        int    trunc = -1;
        size_t n     = api_read_request(sp[0], req, sizeof req, &trunc, &g_chan[1]);
        CHECK(n == maxlen && trunc == 0, "...and it is read whole, not flagged truncated");
        close(sp[0]);
        close(sp[1]);
    }

    {
        int sp[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
        const char *small = "summary\n";
        CHECK(write(sp[1], small, strlen(small)) == (ssize_t)strlen(small), "wrote small cmd");
        int    trunc = -1;
        size_t n     = api_read_request(sp[0], req, sizeof req, &trunc, &g_chan[1]);
        CHECK(n == strlen(small) && !strcmp(req, small), "a small command is returned verbatim");
        CHECK(trunc == 0, "...and is not flagged truncated");
        close(sp[0]);
        close(sp[1]);
    }

    {
        int sp[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
        char      msg[MC_API_REQ_CAP];
        size_t    mlen = make_wp_json(msg, sizeof msg, 600);
        struct wr w    = { sp[1], msg, mlen / 2, mlen - mlen / 2 };
        pthread_t th;
        pthread_create(&th, NULL, writer, &w);
        int    trunc = -1;
        size_t n     = api_read_request(sp[0], req, sizeof req, &trunc, &g_chan[1]);
        pthread_join(th, NULL);
        CHECK(n == mlen && memcmp(req, msg, mlen) == 0,
                "a request split across two segments is reassembled (%zu B)", mlen);
        CHECK(trunc == 0, "...and is not flagged truncated");
        close(sp[0]);
        close(sp[1]);
    }

    {
        int sp[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
        const char *msg  = "{\"command\":\"ascset\",\"parameter\":\"0,x,a}b\\\"}c\"}";
        size_t      mlen = strlen(msg);
        size_t      half = (size_t)(strchr(msg, '}') - msg) + 1;
        struct wr   w    = { sp[1], msg, half, mlen - half };
        pthread_t   th;
        pthread_create(&th, NULL, writer, &w);
        int    trunc = -1;
        size_t n     = api_read_request(sp[0], req, sizeof req, &trunc, &g_chan[1]);
        pthread_join(th, NULL);
        CHECK(n == mlen && !strcmp(req, msg),
                "a request whose first segment ends in a brace inside a string is reassembled");
        close(sp[0]);
        close(sp[1]);
    }

    {
        int sp[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair up (oversized)");
        size_t hcap = MC_API_REQ_CAP + 512;
        char  *huge = malloc(hcap);
        size_t hn   = (size_t)snprintf(huge, hcap, "ascset|0,wallpaper,png,");
        for (; hn + 1 < hcap; hn++)
            huge[hn] = 'a';
        CHECK(write(sp[1], huge, hn) == (ssize_t)hn, "wrote an oversized request (%zu B)", hn);
        int    trunc = -1;
        size_t n     = api_read_request(sp[0], req, sizeof req, &trunc, &g_chan[1]);
        CHECK(trunc == 1, "an oversized request is flagged truncated");
        CHECK(n == MC_API_REQ_CAP - 1, "...and the read stops at the %d-byte cap boundary (%zu)",
                MC_API_REQ_CAP - 1, n);
        free(huge);
        close(sp[0]);
        close(sp[1]);
    }

    {
        static const size_t sp_lens[] = { MC_API_REQ_CAP - 2, MC_API_REQ_CAP - 5 };
        for (size_t k = 0; k < sizeof sp_lens / sizeof sp_lens[0]; k++) {
            size_t slen = sp_lens[k];
            memset(req, 'x', sizeof req);
            memset(req, ' ', slen);
            req[slen] = 0;
            CHECK(req_complete(req, slen) == 1,
                    "%zu spaces at the buffer tail are a complete request", slen);

            char spaces[MC_API_REQ_CAP];
            memset(spaces, ' ', slen);
            int sp[2];
            CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair up (%zu spaces)", slen);
            CHECK(write(sp[1], spaces, slen) == (ssize_t)slen, "wrote %zu spaces", slen);
            int    trunc = -1;
            size_t n     = api_read_request(sp[0], req, sizeof req, &trunc, &g_chan[1]);
            CHECK(n == slen && trunc == 0 && req[n] == 0,
                    "...and they are read whole, not flagged truncated (%zu)", n);
            close(sp[0]);
            close(sp[1]);
        }
    }

    {
        int    sp[2], small = 4096;
        size_t blen = 1u << 20;
        char  *blob = malloc(blen);
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair up (unread reply)");
        setsockopt(sp[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
        memset(blob, 'r', blen);
        uint64_t t0 = api_now_ms();
        api_write_reply(sp[0], blob, blen, MC_API_TCP_SEND_MS);
        uint64_t dt = api_now_ms() - t0;
        CHECK(dt >= MC_API_TCP_SEND_MS - 100 && dt < MC_API_TCP_SEND_MS + 500,
                "a reply the client never reads is abandoned at the %d ms deadline (%llu ms)",
                MC_API_TCP_SEND_MS, (unsigned long long)dt);
        free(blob);
        close(sp[0]);
        close(sp[1]);
    }

    {
        int    sp[2], small = 4096;
        size_t blen = 1u << 20;
        char  *blob = malloc(blen);
        CHECK(g_chan[0].send_ms == MC_API_TCP_SEND_MS && MC_API_TCP_SEND_MS == 500,
                "port 4028 has %d ms to send a reply", g_chan[0].send_ms);
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair up (slow reader)");
        setsockopt(sp[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
        memset(blob, 's', blen);
        pthread_t th;
        int       up = pthread_create(&th, NULL, slow_reader, &sp[1]) == 0;
        uint64_t  t0 = api_now_ms();
        api_write_reply(sp[0], blob, blen, g_chan[0].send_ms);
        uint64_t dt = api_now_ms() - t0;
        CHECK(up && dt >= MC_API_TCP_SEND_MS - 100 && dt < MC_API_TCP_SEND_MS + 500,
                "a client reading %d B every %d ms is cut off at the %d ms deadline (%llu ms)", 16,
                SLOW_READ_GAP_MS, MC_API_TCP_SEND_MS, (unsigned long long)dt);
        close(sp[0]);
        if (up)
            pthread_join(th, NULL);
        close(sp[1]);
        free(blob);
    }

    {
        int sp[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair up (read reply)");
        api_write_reply(sp[0], "summary", 8, MC_API_TCP_SEND_MS);
        char got[16] = { 0 };
        CHECK(read(sp[1], got, sizeof got) == 8 && !strcmp(got, "summary"),
                "a reply to a reading client is sent whole, NUL included");
        close(sp[0]);
        close(sp[1]);
    }

    {
        mc_api_stats s = { 0 };
        char         out[512];
        s.when    = 7;
        s.prod    = "p";
        s.model   = "m";
        s.hwtype  = "h";
        s.swtype  = "sw";
        s.version = "v";
        s.dna     = "d";
        s.mac     = "mac";
        CHECK(reply_fit(12, "summary", 1, &s, out, sizeof out) == 12,
                "a reply that fits is sent as built");
        size_t n = reply_fit(sizeof out, "estats", 1, &s, out, sizeof out);
        CHECK(n < sizeof out && strstr(out, "\"STATUS\":\"E\"") && strstr(out, "\"Code\":14") &&
                        strstr(out, "\"Msg\":\"Reply too long: not sent\""),
                "a json reply that does not fit is replaced by an E record (got %s)", out);
        n = reply_fit(sizeof out + 100, "estats", 0, &s, out, sizeof out);
        CHECK(n < sizeof out && strstr(out, "STATUS=E,When=7,Code=14,Msg=Reply too long: not sent"),
                "a text reply that does not fit is replaced by an E record (got %s)", out);

        static char url[8][200];
        for (int i = 0; i < 8; i++) {
            snprintf(url[i], sizeof url[i], "stratum+tcp://pool%d.example.com:3333/%0150d", i, 0);
            s.pool[i] = (mc_api_pool){ .id = i, .prio = i, .url = url[i], .user = "u" };
        }
        s.npools    = 8;
        char  *big  = malloc(MC_API_REPLY_CAP);
        size_t plen = mc_api_dispatch_text("pools", "pools", &s, big, MC_API_REPLY_CAP);
        size_t elen = mc_api_dispatch_text("estats", "estats", &s, big, MC_API_REPLY_CAP);
        size_t jcap = elen + plen / 2;
        CHECK(plen > 1024 && plen < MC_API_REPLY_CAP && elen < MC_API_REPLY_CAP,
                "8 long pools and estats each fit the reply buffer (%zu B, %zu B)", plen, elen);
        size_t n2 = mc_api_dispatch_text("pools", "pools", &s, big, plen / 2);
        CHECK(n2 >= plen / 2,
                "a text pools reply that overruns its buffer reports its length (%zu)", n2);
        n2 = reply_fit(n2, "pools", 0, &s, big, plen / 2);
        CHECK(n2 < plen / 2 && strstr(big, "STATUS=E,When=7,Code=14,Msg=Reply too long: not sent"),
                "...and is replaced by the E/14 record (got %.80s)", big);
        n2 = mc_api_dispatch_join("estats+pools", 0, &s, big, jcap);
        CHECK(n2 == jcap, "a text estats+pools batch that overruns the buffer reports it (%zu)",
                n2);
        n2 = reply_fit(n2, "estats+pools", 0, &s, big, jcap);
        CHECK(n2 < jcap && strstr(big, "STATUS=E,When=7,Code=14,Msg=Reply too long: not sent"),
                "...and is replaced by the E/14 record (got %.80s)", big);
        free(big);
        s.npools = 0;

        char small[96];
        CHECK(mc_api_dispatch_join("version+summary", 1, &s, small, sizeof small) == sizeof small,
                "a json batch that overruns the buffer reports it does not fit");
        CHECK(mc_api_dispatch_join("version+summary", 0, &s, small, sizeof small) == sizeof small,
                "a text batch that overruns the buffer reports it does not fit");
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
