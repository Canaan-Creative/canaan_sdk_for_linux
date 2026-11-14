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

#define _GNU_SOURCE
#include "sv1.h"
#include "jread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef STRATUM_FIXTURES_DIR
/* Directory holding the stratum/ fixtures when the build does not set it */
#define STRATUM_FIXTURES_DIR "."
#endif
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

#define NOTIFY_FMT \
    "{\"id\":null,\"method\":\"mining.notify\",\"params\":" \
    "[\"J1\",\"%s\",\"01\",\"02\",[\"%064x\"],\"20000000\"," \
    "\"1d00ffff\",\"5f000000\",%s]}"

static size_t g_fail_alloc_size; /* Allocation size malloc() fails on, 0 for none */
static time_t g_wall_jump;       /* offset added to CLOCK_REALTIME and time(), s */
static time_t g_mono_step;       /* offset added to CLOCK_MONOTONIC, s */

static int fails;

extern void *__libc_malloc(size_t n); /* glibc allocator the malloc() override forwards to */

void *malloc(size_t n)
{
    if (g_fail_alloc_size && n == g_fail_alloc_size)
        return NULL;
    return __libc_malloc(n);
}

int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (syscall(SYS_clock_gettime, id, ts) != 0)
        return -1;
    if (id == CLOCK_REALTIME)
        ts->tv_sec += g_wall_jump;
    if (id == CLOCK_MONOTONIC)
        ts->tv_sec += g_mono_step;
    return 0;
}

time_t time(time_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (t)
        *t = ts.tv_sec;
    return ts.tv_sec;
}

static void test_builders(void)
{
    sv1_client c;
    sv1_init(&c);

    char       *l = sv1_build_configure(&c);
    json_value *v = json_parse(l);
    CHECK(v && !strcmp(json_string(json_object_get(v, "method")), "mining.configure"),
            "configure method");
    const json_value *p = json_object_get(v, "params");
    CHECK(!strcmp(json_string(json_array_get(json_array_get(p, 0), 0)), "version-rolling"),
            "configure requests version-rolling");
    CHECK(!strcmp(json_string(json_object_get(json_array_get(p, 1), "version-rolling.mask")),
                  "1fffe000"),
            "configure mask is the BIP310 window");
    json_free(v);
    free(l);

    l = sv1_build_subscribe(&c, "miner_core/1.0");
    v = json_parse(l);
    CHECK(!strcmp(json_string(json_array_get(json_object_get(v, "params"), 0)), "miner_core/1.0"),
            "subscribe carries user-agent");
    json_free(v);
    free(l);

    l = sv1_build_authorize(&c, "worker.1", "x");
    v = json_parse(l);
    p = json_object_get(v, "params");
    CHECK(!strcmp(json_string(json_array_get(p, 0)), "worker.1") &&
                    !strcmp(json_string(json_array_get(p, 1)), "x"),
            "authorize user/pass");
    json_free(v);
    free(l);

    l = sv1_build_authorize(&c, "a\\b\"c", "p\"],\"x\\\n");
    v = json_parse(l);
    p = json_object_get(v, "params");
    CHECK(v && json_array_size(p) == 2 && !strcmp(json_string(json_array_get(p, 0)), "a\\b\"c") &&
                    !strcmp(json_string(json_array_get(p, 1)), "p\"],\"x\\\n"),
            "authorize escapes quotes, backslashes and control characters");
    json_free(v);
    free(l);

    l = sv1_build_submit(&c, "w\"1\\", "J\"\\", "00", "6a4ae54b", "0f2ccd65", NULL);
    v = json_parse(l);
    p = json_object_get(v, "params");
    CHECK(v && json_array_size(p) == 5 && !strcmp(json_string(json_array_get(p, 0)), "w\"1\\") &&
                    !strcmp(json_string(json_array_get(p, 1)), "J\"\\"),
            "submit escapes the worker and job id");
    json_free(v);
    free(l);

    l = sv1_build_submit(&c, "worker.1", "JOB", "0400000000000000", "6a4ae54b", "0f2ccd65",
            "1fffe000");
    v = json_parse(l);
    p = json_object_get(v, "params");
    CHECK(json_array_size(p) == 6, "version-rolling submit has 6 params");
    CHECK(!strcmp(json_string(json_array_get(p, 1)), "JOB"), "submit job_id");
    CHECK(!strcmp(json_string(json_array_get(p, 5)), "1fffe000"), "submit version_bits");
    json_free(v);
    free(l);

    l = sv1_build_submit(&c, "w", "J", "xx", "tt", "nn", NULL);
    v = json_parse(l);
    CHECK(json_array_size(json_object_get(v, "params")) == 5, "plain submit has 5 params");
    json_free(v);
    free(l);

    sv1_free(&c);
}

static void test_lifecycle(void)
{
    FILE *f = fopen(STRATUM_FIXTURES_DIR "/stratum/happy.jsonl", "rb");
    if (!f) {
        fprintf(stderr, "  FAIL: happy.jsonl not found\n");
        fails++;
        return;
    }

    sv1_client c;
    sv1_init(&c);

    {
        sv1_client probe;
        sv1_init(&probe);
        free(sv1_build_extranonce_subscribe(&probe));
        CHECK(probe.next_id - 1 < 15, "no in-flight id can collide with the fixture's 15..27");
        sv1_free(&probe);
    }

    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     notifies = 0;
    while ((n = getline(&buf, &cap, f)) > 0) {
        json_value *w = json_parse(buf);
        if (!w) {
            json_free(w);
            continue;
        }
        const char *dir   = json_string(json_object_get(w, "dir"));
        long long   conn  = json_int(json_object_get(w, "conn"));
        const char *inner = json_string(json_object_get(w, "line"));
        if (!strcmp(dir, "tx") && conn == 1) {
            const json_value *m      = json_object_get(w, "msg");
            const char       *method = json_string(json_object_get(m, "method"));
            if (method && !strcmp(method, "mining.authorize"))
                c.id_authorize = (int)json_int(json_object_get(m, "id"));
        }
        if (!strcmp(dir, "rx") && conn == 1 && inner[0]) {
            sv1_event ev;
            sv1_feed_line(&c, inner, &ev);
            if (ev.type == SV1_EV_NOTIFY)
                notifies++;
        }
        json_free(w);
    }
    free(buf);
    fclose(f);

    CHECK(c.state == SV1_MINING, "reached MINING after configure/subscribe/authorize/notify");
    CHECK(!strcmp(c.xnonce1, "00"), "xnonce1 negotiated == 00");
    CHECK(c.xnonce2_size == 8, "xnonce2_size negotiated == 8");
    CHECK(c.version_rolling && c.version_mask == 0x1fffe000u, "version mask 0x1fffe000");
    CHECK(c.difficulty > 0, "difficulty set from set_difficulty");
    CHECK(c.has_job && c.job.job_id[0], "a job was parsed");
    CHECK(c.job.n_merkle > 0 && c.job.prevhash && strlen(c.job.prevhash) == 64,
            "job carries prevhash + merkle branches");
    CHECK(notifies >= 1, "at least one notify processed");
    CHECK(c.untracked >= 1 && c.accepted == 0,
            "replayed submit results are untracked, not accounted");

    char *l = sv1_build_submit(&c, "poolworker.001", c.job.job_id, "0400000000000000", c.job.ntime,
            "0f2ccd65", c.job.version);
    json_value *v = json_parse(l);
    CHECK(v && json_array_size(json_object_get(v, "params")) == 6,
            "submit for live job is well-formed");
    json_free(v);
    free(l);

    sv1_free(&c);
}

static void test_xnsub(void)
{
    sv1_client c;
    sv1_init(&c);

    char       *l = sv1_build_extranonce_subscribe(&c);
    json_value *v = json_parse(l);
    CHECK(v && !strcmp(json_string(json_object_get(v, "method")), "mining.extranonce.subscribe"),
            "extranonce.subscribe method");
    CHECK(v && json_array_size(json_object_get(v, "params")) == 0,
            "extranonce.subscribe has empty params");
    json_free(v);
    free(l);

    sv1_event ev;
    sv1_feed_line(&c,
            "{\"id\":null,\"method\":\"mining.set_extranonce\","
            "\"params\":[\"abcd\",6]}",
            &ev);
    CHECK(ev.type == SV1_EV_SET_EXTRANONCE, "set_extranonce -> SET_EXTRANONCE event");
    CHECK(!strcmp(c.xnonce1, "abcd"), "set_extranonce updates xnonce1");
    CHECK(c.xnonce2_size == 6, "set_extranonce updates xnonce2_size");

    sv1_feed_line(&c, "{\"method\":\"mining.set_extranonce\",\"params\":[\"ef\",1]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && c.xnonce2_size == 6 && !strcmp(c.xnonce1, "abcd"),
            "set_extranonce with n2size<2 rejected");

    sv1_feed_line(&c, "{\"method\":\"mining.set_extranonce\",\"params\":[\"ef\",4294967300]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && c.xnonce2_size == 6 && !strcmp(c.xnonce1, "abcd"),
            "set_extranonce with an n2size beyond int rejected");

    sv1_feed_line(&c, "{\"method\":\"mining.set_extranonce\",\"params\":[\"zz\",6]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.xnonce1, "abcd"),
            "set_extranonce with non-hex xnonce1 rejected");

    sv1_feed_line(&c, "{\"method\":\"mining.set_extranonce\",\"params\":[\"abc\",6]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.xnonce1, "abcd") && c.xnonce2_size == 6,
            "set_extranonce with an odd-length xnonce1 rejected");

    sv1_feed_line(&c,
            "{\"method\":\"mining.set_extranonce\",\"params\":"
            "[\"000102030405060708090a0b0c0d0e0f10\",6]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.xnonce1, "abcd"),
            "set_extranonce with an over-long xnonce1 rejected");

    sv1_feed_line(&c, "{\"method\":\"mining.set_extranonce\",\"params\":[\"ef\",17]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && c.xnonce2_size == 6, "set_extranonce with n2size>16 rejected");
}

static void test_subscribe_validated(void)
{
    sv1_client c;
    sv1_event  ev;
    char       line[160], why[128];

    const char *bad_n1[] = { "000", "0zz0", "", "000102030405060708090a0b0c0d0e0f10" };
    for (size_t i = 0; i < sizeof bad_n1 / sizeof bad_n1[0]; i++) {
        sv1_init(&c);
        c.state = SV1_CONFIGURED;
        snprintf(line, sizeof line,
                "{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"%s\",8],"
                "\"error\":null}",
                bad_n1[i]);
        sv1_feed_line(&c, line, &ev);
        snprintf(why, sizeof why, "subscribe nonce1 \"%s\" fails the subscribe", bad_n1[i]);
        CHECK(ev.type == SV1_EV_ERROR && c.state == SV1_CONFIGURED && c.xnonce1[0] == '\0' &&
                        c.xnonce2_size == 0,
                why);
    }

    const int bad_n2[] = { -1, 0, 1, 17, 64 };
    for (size_t i = 0; i < sizeof bad_n2 / sizeof bad_n2[0]; i++) {
        sv1_init(&c);
        c.state = SV1_CONFIGURED;
        snprintf(line, sizeof line,
                "{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"00\",%d],"
                "\"error\":null}",
                bad_n2[i]);
        sv1_feed_line(&c, line, &ev);
        snprintf(why, sizeof why, "subscribe n2size %d fails the subscribe", bad_n2[i]);
        CHECK(ev.type == SV1_EV_ERROR && c.state == SV1_CONFIGURED && c.xnonce1[0] == '\0' &&
                        c.xnonce2_size == 0,
                why);
    }

    const int ok_n2[] = { 2, 8, 16 };
    for (size_t i = 0; i < sizeof ok_n2 / sizeof ok_n2[0]; i++) {
        sv1_init(&c);
        c.state = SV1_CONFIGURED;
        snprintf(line, sizeof line,
                "{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"0a1b\",%d],"
                "\"error\":null}",
                ok_n2[i]);
        sv1_feed_line(&c, line, &ev);
        snprintf(why, sizeof why, "a valid subscribe at n2size %d still subscribes", ok_n2[i]);
        CHECK(ev.type == SV1_EV_SUBSCRIBED && c.state == SV1_SUBSCRIBED &&
                        !strcmp(c.xnonce1, "0a1b") && c.xnonce2_size == ok_n2[i],
                why);
    }
}

static void test_init_clears_the_session(void)
{
    sv1_client c;
    sv1_event  ev;

    sv1_init(&c);
    c.state = SV1_AUTHORIZED;
    sv1_feed_line(&c, "{\"method\":\"mining.set_difficulty\",\"params\":[4096]}", &ev);
    sv1_feed_line(&c, "{\"method\":\"mining.set_difficulty\",\"params\":[8192]}", &ev);
    CHECK(c.next_diff == 4096 && c.diff_after == 8192,
            "two difficulties staged on the live client");

    sv1_init(&c);
    CHECK(c.state == SV1_INIT && c.difficulty == 0 && c.next_diff == 0 && c.diff_after == 0,
            "sv1_init starts a session with no staged difficulty");
    CHECK(c.xnonce1[0] == '\0' && c.xnonce2_size == 0 && c.prev_hash[0] == '\0',
            "...and carries no extranonce or prevhash across");
}

static void test_version_mask_clamp(void)
{
    sv1_client c;
    sv1_event  ev;

    sv1_init(&c);
    sv1_feed_line(&c,
            "{\"id\":1,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"ffffffff\"},\"error\":null}",
            &ev);
    CHECK(ev.type == SV1_EV_CONFIGURED, "over-wide mask still configures");
    CHECK(c.version_rolling == 1, "over-wide mask keeps rolling on");
    CHECK(c.version_mask == 0x1fffe000u, "over-wide mask clamped to the window");

    sv1_init(&c);
    sv1_feed_line(&c,
            "{\"id\":1,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"00ffe000\"},\"error\":null}",
            &ev);
    CHECK(c.version_mask == 0x00ffe000u, "in-window mask passed through");

    sv1_init(&c);
    sv1_feed_line(&c,
            "{\"id\":1,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"000000ff\"},\"error\":null}",
            &ev);
    CHECK(c.version_mask == 0, "out-of-window-only mask clamps to 0");
    CHECK(c.version_rolling == 0, "no usable bits -> rolling off");
}

static int handshake(sv1_client *c)
{
    sv1_event ev;
    char      line[96];
    char     *l;
    sv1_init(c);
    c->extranonce_subscribe = 1;
    l                       = sv1_build_configure(c);
    free(l);
    sv1_feed_line(c,
            "{\"id\":1,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}",
            &ev);
    l = sv1_build_subscribe(c, "ua");
    free(l);
    sv1_feed_line(c,
            "{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"00\",8],"
            "\"error\":null}",
            &ev);
    l = sv1_build_authorize(c, "u", "p");
    free(l);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", c->id_authorize);
    sv1_feed_line(c, line, &ev);
    l = sv1_build_extranonce_subscribe(c);
    free(l);
    return c->next_id - 1;
}

static int submit(sv1_client *c)
{
    char *l = sv1_build_submit(c, "w", "J", "0000", "5f000000", "0f2ccd65", NULL);
    free(l);
    return c->next_id - 1;
}

static void test_authorize_matches_id(void)
{
    sv1_client c;
    sv1_event  ev;
    char       line[96];
    char      *l;

    sv1_init(&c);
    l = sv1_build_configure(&c);
    free(l);
    sv1_feed_line(&c,
            "{\"id\":1,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}",
            &ev);
    l = sv1_build_subscribe(&c, "ua");
    free(l);
    sv1_feed_line(&c,
            "{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"00\",8],"
            "\"error\":null}",
            &ev);
    l = sv1_build_authorize(&c, "u", "p");
    free(l);

    snprintf(line, sizeof line, "{\"id\":%d,\"result\":false,\"error\":null}", c.id_subscribe);
    sv1_feed_line(&c, line, &ev);
    CHECK(c.state == SV1_SUBSCRIBED && ev.type != SV1_EV_AUTHORIZED && ev.type != SV1_EV_ERROR,
            "a reply with another id in the authorize window is not the authorize result");
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}",
            c.id_authorize + 1000);
    sv1_feed_line(&c, line, &ev);
    CHECK(c.state == SV1_SUBSCRIBED && ev.type != SV1_EV_AUTHORIZED,
            "...nor is an unknown id that answers true");
    sv1_feed_line(&c, "{\"result\":true,\"error\":null}", &ev);
    CHECK(c.state == SV1_SUBSCRIBED && ev.type != SV1_EV_AUTHORIZED,
            "...nor a reply without an id");
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", c.id_authorize);
    sv1_feed_line(&c, line, &ev);
    CHECK(c.state == SV1_AUTHORIZED && ev.type == SV1_EV_AUTHORIZED,
            "the authorize reply arriving after them authorizes");
    sv1_free(&c);
}

static void test_submit_classification(void)
{
    sv1_client c;
    sv1_event  ev;
    char       line[160];
    int        xid, id;
    uint32_t   base;

    xid  = handshake(&c);
    base = sv1_inflight_count();
    CHECK(c.state == SV1_AUTHORIZED, "handshake reached AUTHORIZED");

    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", xid);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_NONE,
            "the extranonce.subscribe reply answers our own request, not a share");
    CHECK(!ev.submit_accepted && c.accepted == 0 && c.rejected == 0 && c.untracked == 0,
            "...so it moves Accepted, Rejected and untracked alike not at all");

    sv1_feed_line(&c, "{\"id\":987654,\"result\":true,\"error\":null}", &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_UNTRACKED && c.accepted == 0,
            "an id matching no in-flight submit is untracked");
    sv1_feed_line(&c, "{\"result\":true}", &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_UNTRACKED && c.accepted == 0,
            "a result with no id is untracked");
    sv1_feed_line(&c, "{\"id\":null,\"result\":true}", &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_UNTRACKED && c.accepted == 0,
            "a result with a null id is untracked");

    id = submit(&c);
    CHECK(sv1_inflight_count() == base + 1, "the submit is in flight");
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_RESULT && ev.submit_accepted && c.accepted == 1,
            "a result for an in-flight submit is an accepted share");
    CHECK(ev.submit_id == id, "...named by the id it answers");

    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_UNTRACKED && c.accepted == 1,
            "a second reply to the same id is untracked, not a second accept");

    id = submit(&c);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[21,\"Job not found\",null]}",
            id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_RESULT && !ev.submit_accepted && c.rejected == 1,
            "result:false for an in-flight submit is a rejected share");
    id = submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":null,\"error\":null}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(!ev.submit_accepted && c.rejected == 2, "result:null is a reject");
    id = submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"error\":null}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(!ev.submit_accepted && c.rejected == 3, "a missing result is a reject");
    id = submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":1}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(!ev.submit_accepted && c.rejected == 4, "a non-boolean result is a reject");

    id = submit(&c);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":true,"
            "\"error\":[21,\"ignored\",null]}",
            id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.submit_accepted && c.accepted == 2,
            "result:true with an error object is still an accept");

    CHECK(sv1_inflight_count() == base, "every submit made here has been answered");

    uint32_t racc = c.accepted, rrej = c.rejected, runt = c.untracked;
    uint32_t stale0  = c.stale;
    uint32_t cleared = sv1_clear_inflight(&c, NULL);
    CHECK(sv1_inflight_count() == 0 && c.stale == stale0 + cleared,
            "session teardown clears in-flight submits and counts them stale");

    int d1 = submit(&c);
    (void)submit(&c);
    int d2 = submit(&c);
    sv1_inflight_set_diff(d1, 8192.0);
    sv1_inflight_set_diff(d2, 1024.0);
    sv1_inflight_set_diff(d2 + 1000, 65536.0);
    double dstale = -1.0;
    cleared       = sv1_clear_inflight(&c, &dstale);
    CHECK(cleared == 3 && dstale == 9216.0,
            "teardown sums the difficulty of the submits it clears");
    stale0  = c.stale;
    dstale  = -1.0;
    cleared = sv1_clear_inflight(&c, &dstale);
    CHECK(cleared == 0 && dstale == 0.0 && c.stale == stale0,
            "...and a second teardown finds nothing");

    int lost_id = submit(&c);
    for (int i = 0; i < 64; i++)
        (void)submit(&c);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[23,\"Low difficulty share\",null]}",
            lost_id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_RESULT && !ev.submit_accepted && c.rejected == rrej + 1 &&
                    c.accepted == racc && c.untracked == runt,
            "a reject for an evicted in-flight record still counts REJECTED");
    CHECK(ev.submit_lost && ev.err_code == 23 && !strcmp(ev.err_msg, "Low difficulty share"),
            "...flagged as lost, with the pool's reason kept");
    rrej = c.rejected;
    runt = c.untracked;
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_UNTRACKED && c.rejected == rrej && c.untracked == runt + 1,
            "a repeat of that same reply is untracked, not a second reject");

    racc = c.accepted;
    sv1_clear_inflight(&c, NULL);
    lost_id = submit(&c);
    for (int i = 0; i < 64; i++)
        (void)submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", lost_id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_RESULT && ev.submit_accepted && ev.submit_lost &&
                    c.accepted == racc + 1,
            "an accept for an evicted record still counts ACCEPTED");

    runt = c.untracked;
    racc = c.accepted;
    rrej = c.rejected;
    sv1_clear_inflight(&c, NULL);
    lost_id = submit(&c);
    for (int i = 0; i < 64; i++)
        (void)submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"error\":null}", lost_id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_UNTRACKED && c.untracked == runt + 1 && c.accepted == racc &&
                    c.rejected == rrej,
            "a lost record with no result at all is untracked, not a verdict");
    sv1_free(&c);
}

static void test_submit_reject_reason(void)
{
    sv1_client c;
    sv1_event  ev;
    char       line[160];
    int        id;
    handshake(&c);
    CHECK(c.state == SV1_AUTHORIZED, "handshake reached AUTHORIZED");

    id = submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_RESULT && ev.submit_accepted, "accept parsed");
    CHECK(ev.submit_id == id, "accepted submit carries its id");
    CHECK(ev.err_code == 0 && ev.err_msg[0] == '\0', "an accept has no reason");

    id = submit(&c);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[21,\"Job not found\",null]}",
            id);
    sv1_feed_line(&c, line, &ev);
    CHECK(!ev.submit_accepted, "reject parsed");
    CHECK(ev.submit_id == id, "reject names the share by id");
    CHECK(ev.err_code == 21, "pool error code kept");
    CHECK(!strcmp(ev.err_msg, "Job not found"), "pool error text kept");

    id = submit(&c);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[23,\"Low difficulty share\",null]}",
            id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.err_code == 23 && !strcmp(ev.err_msg, "Low difficulty share"),
            "low-difficulty reason kept");
    id = submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":false,\"error\":\"stale\"}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.err_code == 0 && !strcmp(ev.err_msg, "stale"), "bare-string error kept");

    id = submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":false,\"error\":null}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(!ev.submit_accepted && ev.err_code == 0 && ev.err_msg[0] == '\0',
            "reject with no error object reports no reason");

    CHECK(c.accepted == 1 && c.rejected == 4, "tallies unchanged by the plumbing");
    sv1_free(&c);
}

static void test_protocol_rules(void)
{
    sv1_client c;
    sv1_event  ev;
    char       line[1024];

    const char *PH0 = "aa11223344556677889900112233445566778899001122334455667788990011";
    const char *PH1 = "bb11223344556677889900112233445566778899001122334455667788990011";

    sv1_init(&c);
    c.state = SV1_AUTHORIZED;
    sv1_feed_line(&c,
            "{\"id\":null,\"method\":\"mining.set_difficulty\","
            "\"params\":[1024]}",
            &ev);
    CHECK(ev.type == SV1_EV_SET_DIFFICULTY && c.difficulty == 0 && c.next_diff == 1024,
            "set_difficulty is staged, not applied to the live difficulty");
    snprintf(line, sizeof line, NOTIFY_FMT, PH0, 1, "false");
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_NOTIFY && c.difficulty == 1024 && c.next_diff == 0,
            "the next notify promotes the staged difficulty");

    sv1_feed_line(&c, "{\"method\":\"mining.set_difficulty\",\"params\":[2048]}", &ev);
    sv1_feed_line(&c, "{\"method\":\"mining.set_difficulty\",\"params\":[4096]}", &ev);
    CHECK(c.next_diff == 2048 && c.diff_after == 4096 && c.difficulty == 1024,
            "a second set_difficulty stacks behind the first");
    snprintf(line, sizeof line, NOTIFY_FMT, PH0, 1, "false");
    sv1_feed_line(&c, line, &ev);
    CHECK(c.difficulty == 2048 && c.next_diff == 4096 && c.diff_after == 0,
            "...and each notify promotes one of them, in order");
    snprintf(line, sizeof line, NOTIFY_FMT, PH0, 1, "false");
    sv1_feed_line(&c, line, &ev);
    CHECK(c.difficulty == 4096 && c.next_diff == 0,
            "...the stacked one lands on the following notify");

    sv1_feed_line(&c, "{\"method\":\"mining.set_difficulty\",\"params\":[0]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && c.difficulty == 4096 && c.next_diff == 0,
            "set_difficulty 0 is refused and stages nothing");
    sv1_feed_line(&c, "{\"method\":\"mining.set_difficulty\",\"params\":[-1]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && c.next_diff == 0, "a negative difficulty is refused too");

    snprintf(line, sizeof line, NOTIFY_FMT, PH0, 2, "false");
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_NOTIFY && c.job.clean_jobs == 0,
            "same prevhash + clean=false stays not-clean");
    snprintf(line, sizeof line, NOTIFY_FMT, PH1, 2, "false");
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_NOTIFY && c.job.clean_jobs == 1,
            "a new prevhash forces clean even with clean=false");

    char keep_job[64];
    snprintf(keep_job, sizeof keep_job, "%s", c.job.job_id);
    double keep_diff = c.difficulty;
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"BAD\",\"zz\",\"01\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",true]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) && c.difficulty == keep_diff,
            "a notify with a non-hex prevhash is dropped, previous job intact");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"BAD\",\"aa\",\"01\",\"02\",[\"zz\"],\"20000000\","
            "\"1d00ffff\",\"5f000000\",true]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job),
            "a notify with a non-hex merkle branch is dropped too");

    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"BAD\",\"aa\",\"012\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",true]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job),
            "a notify with an odd-length coinb1 is dropped");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"BAD\",\"aa\",\"01\",\"023\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",true]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job),
            "...and one with an odd-length coinb2 too");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"OK1\",\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_NOTIFY && !strcmp(c.job.job_id, "OK1"),
            "the same notify at an even length is still accepted");

    snprintf(keep_job, sizeof keep_job, "%s", c.job.job_id);
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[null,\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) && c.difficulty == keep_diff,
            "a notify with a null job_id is dropped, previous job intact");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[7,\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job),
            "a numeric job_id is dropped too, not read as an empty string");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[{},\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job),
            "...and an object job_id likewise");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"\",\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job),
            "an empty job_id is dropped: the pool rejects a share submitted with it");

    char over_id[SV1_ID_MAX + 8];
    memset(over_id, 'a', sizeof over_id - 1);
    over_id[sizeof over_id - 1] = '\0';
    snprintf(line, sizeof line,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"%s\",\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",false]}",
            over_id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) && c.difficulty == keep_diff,
            "an over-long job_id is refused, not silently truncated");

    char max_id[SV1_ID_MAX];
    memset(max_id, 'b', sizeof max_id - 1);
    max_id[sizeof max_id - 1] = '\0';
    snprintf(line, sizeof line,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"%s\",\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"5f000000\",false]}",
            max_id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_NOTIFY && !strcmp(c.job.job_id, max_id),
            "the longest job_id that fits whole is still accepted");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"J-1_x.9\",\"aa\",\"0123\",\"02\",[],\"20000000\","
            "\"1d00ffff\",\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_NOTIFY && !strcmp(c.job.job_id, "J-1_x.9"),
            "the accepted job_id character set is not narrowed");

    snprintf(keep_job, sizeof keep_job, "%s", c.job.job_id);
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"BAD\",\"aa\",\"0123\",\"02\",[],\"aaaaaaaaaaaaaaaa\","
            "\"1d00ffff\",\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) && c.difficulty == keep_diff,
            "an over-long version is refused, not silently truncated");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"BAD\",\"aa\",\"0123\",\"02\",[],\"20000000\","
            "\"aaaaaaaaaaaaaaaa\",\"5f000000\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) && c.difficulty == keep_diff,
            "an over-long nbits is refused: every share computed from it is wrong");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"BAD\",\"aa\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
            "\"aaaaaaaaaaaaaaaa\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) && c.difficulty == keep_diff,
            "an over-long ntime is refused too");
    sv1_feed_line(&c,
            "{\"method\":\"mining.notify\",\"params\":"
            "[\"OK2\",\"aa\",\"0123\",\"02\",[],\"aaaaaaaaaaaaaa\","
            "\"bbbbbbbbbbbbbb\",\"cccccccccccccc\",false]}",
            &ev);
    CHECK(ev.type == SV1_EV_NOTIFY && !strcmp(c.job.job_id, "OK2") &&
                    !strcmp(c.job.version, "aaaaaaaaaaaaaa") &&
                    !strcmp(c.job.nbits, "bbbbbbbbbbbbbb") &&
                    !strcmp(c.job.ntime, "cccccccccccccc"),
            "the longest version/nbits/ntime that fit whole are still accepted");

    {
        char hash72[73], hash70[71];
        memset(hash72, 'a', sizeof hash72 - 1);
        hash72[sizeof hash72 - 1] = '\0';
        memset(hash70, 'b', sizeof hash70 - 1);
        hash70[sizeof hash70 - 1] = '\0';
        snprintf(keep_job, sizeof keep_job, "%s", c.job.job_id);
        snprintf(line, sizeof line,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"BAD\",\"%s\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
                "\"5f000000\",false]}",
                hash72);
        sv1_feed_line(&c, line, &ev);
        CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) &&
                        c.difficulty == keep_diff,
                "an over-long prevhash is refused, not silently truncated");
        snprintf(line, sizeof line,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"OK3\",\"%s\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
                "\"5f000000\",false]}",
                hash70);
        sv1_feed_line(&c, line, &ev);
        CHECK(ev.type == SV1_EV_NOTIFY && !strcmp(c.job.job_id, "OK3") &&
                        !strcmp(c.job.prevhash, hash70) && !strcmp(c.prev_hash, hash70),
                "the longest prevhash that fits whole is still accepted");
    }

    {
        char   big[4096];
        size_t n = 0;
        n += (size_t)snprintf(big + n, sizeof big - n,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"BAD\",\"aa\",\"0123\",\"02\",[");
        for (size_t i = 0; i <= SV1_MAX_MERKLE; i++)
            n += (size_t)snprintf(big + n, sizeof big - n, "%s\"ab\"", i ? "," : "");
        n += (size_t)snprintf(big + n, sizeof big - n,
                "],\"20000000\",\"1d00ffff\",\"5f000000\",false]}");
        snprintf(keep_job, sizeof keep_job, "%s", c.job.job_id);
        sv1_feed_line(&c, big, &ev);
        CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) &&
                        c.difficulty == keep_diff,
                "a notify with more merkle branches than fit is refused, not truncated");

        n = 0;
        n += (size_t)snprintf(big + n, sizeof big - n,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"OK4\",\"aa\",\"0123\",\"02\",[");
        for (size_t i = 0; i < SV1_MAX_MERKLE; i++)
            n += (size_t)snprintf(big + n, sizeof big - n, "%s\"ab\"", i ? "," : "");
        n += (size_t)snprintf(big + n, sizeof big - n,
                "],\"20000000\",\"1d00ffff\",\"5f000000\",false]}");
        sv1_feed_line(&c, big, &ev);
        CHECK(ev.type == SV1_EV_NOTIFY && !strcmp(c.job.job_id, "OK4") &&
                        c.job.n_merkle == SV1_MAX_MERKLE,
                "exactly SV1_MAX_MERKLE branches are still accepted, all of them kept");
    }

    snprintf(keep_job, sizeof keep_job, "%s", c.job.job_id);
    {
        size_t keep_merkle = c.job.n_merkle;
        sv1_feed_line(&c,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"BAD\",\"aa\",\"0123\",\"02\",\"ab\",\"20000000\","
                "\"1d00ffff\",\"5f000000\",false]}",
                &ev);
        CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) &&
                        c.job.n_merkle == keep_merkle && c.difficulty == keep_diff,
                "a string merkle field is refused, not read as zero branches");
        sv1_feed_line(&c,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"BAD\",\"aa\",\"0123\",\"02\",{},\"20000000\","
                "\"1d00ffff\",\"5f000000\",false]}",
                &ev);
        CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) &&
                        c.job.n_merkle == keep_merkle && c.difficulty == keep_diff,
                "an object merkle field is refused too");
        sv1_feed_line(&c,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"BAD\",\"aa\",\"0123\",\"02\",null,\"20000000\","
                "\"1d00ffff\",\"5f000000\",false]}",
                &ev);
        CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) &&
                        c.job.n_merkle == keep_merkle && c.difficulty == keep_diff,
                "a null merkle field is refused, previous job left mineable");
        sv1_feed_line(&c,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"BAD\",\"aa\",\"0123\",\"02\",7,\"20000000\","
                "\"1d00ffff\",\"5f000000\",false]}",
                &ev);
        CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) &&
                        c.job.n_merkle == keep_merkle && c.difficulty == keep_diff,
                "...and a numeric one likewise");

        sv1_feed_line(&c,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"OK6\",\"aa\",\"0123\",\"02\",[],\"20000000\","
                "\"1d00ffff\",\"5f000000\",false]}",
                &ev);
        CHECK(ev.type == SV1_EV_NOTIFY && !strcmp(c.job.job_id, "OK6") && c.job.n_merkle == 0,
                "a legitimate zero-branch notify (empty array) is still accepted");
    }

    {
        char hash62[63], mk100[101];
        void *volatile probe;
        int hook_live;

        memset(hash62, 'a', sizeof hash62 - 1);
        hash62[sizeof hash62 - 1] = '\0';
        memset(mk100, 'c', sizeof mk100 - 1);
        mk100[sizeof mk100 - 1] = '\0';

        g_fail_alloc_size = 4093;
        probe             = malloc(4093);
        hook_live         = (probe == NULL);
        g_fail_alloc_size = 0;
        free((void *)probe);
        CHECK(hook_live, "the test's malloc hook is in force");

        snprintf(line, sizeof line,
                "{\"method\":\"mining.notify\",\"params\":"
                "[\"OK5\",\"aa\",\"0123\",\"02\",[\"ab\",\"%s\"],\"20000000\","
                "\"1d00ffff\",\"5f000000\",false]}",
                mk100);
        sv1_feed_line(&c, line, &ev);
        CHECK(ev.type == SV1_EV_NOTIFY && c.job.n_merkle == 2 && c.job.merkle[0] &&
                        c.job.merkle[1] && !strcmp(c.job.merkle[0], "ab") &&
                        !strcmp(c.job.merkle[1], mk100),
                "every merkle branch of an accepted notify is a live string");

        snprintf(keep_job, sizeof keep_job, "%s", c.job.job_id);
        if (hook_live) {
            g_fail_alloc_size = sizeof hash62;
            snprintf(line, sizeof line,
                    "{\"method\":\"mining.notify\",\"params\":"
                    "[\"BAD\",\"%s\",\"0123\",\"02\",[],\"20000000\",\"1d00ffff\","
                    "\"5f000000\",false]}",
                    hash62);
            sv1_feed_line(&c, line, &ev);
            g_fail_alloc_size = 0;
            CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) && c.job.prevhash &&
                            !strcmp(c.job.prevhash, "aa") && !strcmp(c.prev_hash, "aa") &&
                            c.has_job == 1 && c.job.n_merkle == 2 && c.difficulty == keep_diff,
                    "a notify whose prevhash cannot be allocated leaves the previous job mineable");

            g_fail_alloc_size = sizeof mk100;
            snprintf(line, sizeof line,
                    "{\"method\":\"mining.notify\",\"params\":"
                    "[\"BAD\",\"aa\",\"0123\",\"02\",[\"ab\",\"%s\"],\"20000000\","
                    "\"1d00ffff\",\"5f000000\",false]}",
                    mk100);
            sv1_feed_line(&c, line, &ev);
            g_fail_alloc_size = 0;
            CHECK(ev.type == SV1_EV_ERROR && !strcmp(c.job.job_id, keep_job) &&
                            c.job.n_merkle == 2 && c.job.merkle[0] && c.job.merkle[1] &&
                            c.has_job == 1 && c.difficulty == keep_diff,
                    "a merkle branch that cannot be allocated is refused, never left NULL "
                    "inside a job reported as valid");
        }
    }

    sv1_feed_line(&c, "{\"id\":77,\"method\":\"mining.ping\",\"params\":[]}", &ev);
    CHECK(ev.type == SV1_EV_PING && ev.req_id == 77,
            "mining.ping -> PING event carrying the id to answer");
    {
        char       *l = sv1_build_pong(77);
        json_value *v = json_parse(l);
        CHECK(v && json_int(json_object_get(v, "id")) == 77 &&
                        !strcmp(json_string(json_object_get(v, "result")), "pong"),
                "sv1_build_pong answers that id with pong");
        json_free(v);
        free(l);
    }

    sv1_feed_line(&c, "{\"id\":78,\"method\":\"client.get_version\",\"params\":[]}", &ev);
    CHECK(ev.type == SV1_EV_GET_VERSION && ev.req_id == 78,
            "client.get_version -> GET_VERSION event with the id");
    {
        char       *l = sv1_build_version(78, "canaan-miner/1.0");
        json_value *v = json_parse(l);
        CHECK(v && json_int(json_object_get(v, "id")) == 78 &&
                        !strcmp(json_string(json_object_get(v, "result")), "canaan-miner/1.0"),
                "sv1_build_version answers with the user agent");
        json_free(v);
        free(l);
    }

    sv1_feed_line(&c,
            "{\"method\":\"mining.set_version_mask\","
            "\"params\":[\"00ffe000\"]}",
            &ev);
    CHECK(ev.type == SV1_EV_SET_VERSION_MASK && c.version_mask == 0x00ffe000u &&
                    c.version_rolling == 1,
            "set_version_mask applies the new mask");
    sv1_feed_line(&c,
            "{\"method\":\"mining.set_version_mask\","
            "\"params\":\"ffffffff\"}",
            &ev);
    CHECK(c.version_mask == 0x1fffe000u, "...as a bare string too, clamped to the BIP310 window");
    sv1_feed_line(&c, "{\"method\":\"mining.set_version_mask\",\"params\":[\"zz\"]}", &ev);
    CHECK(ev.type == SV1_EV_ERROR && c.version_mask == 0x1fffe000u,
            "a non-hex mask is refused, the live mask kept");

    sv1_feed_line(&c,
            "{\"method\":\"client.show_message\","
            "\"params\":[\"hello miner\"]}",
            &ev);
    CHECK(ev.type == SV1_EV_SHOW_MESSAGE && !strcmp(ev.err_msg, "hello miner"),
            "client.show_message carries the pool text");
    sv1_feed_line(&c, "{\"method\":\"client.reconnect\",\"params\":[\"h\",1,2]}", &ev);
    CHECK(ev.type == SV1_EV_RECONNECT, "client.reconnect is reported");

    sv1_free(&c);
}

static void test_inflight_clock(void)
{
    sv1_client c;
    sv1_event  ev;
    char       line[96];
    int        id;

    handshake(&c);
    sv1_clear_inflight(&c, NULL);

    id          = submit(&c);
    g_wall_jump = 3600;
    CHECK(sv1_inflight_count() == 1, "a wall clock jump forward keeps the submit in flight");
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_RESULT && ev.submit_accepted && !ev.submit_lost,
            "...and its reply is matched, not lost");

    sv1_clear_inflight(&c, NULL);

    g_wall_jump = 3600;
    int first   = submit(&c);
    g_wall_jump = 0;
    for (int i = 0; i < 64; i++)
        (void)submit(&c);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", first);
    sv1_feed_line(&c, line, &ev);
    CHECK(ev.type == SV1_EV_SUBMIT_RESULT && ev.submit_lost,
            "a full table evicts the submit sent first, whatever the wall clock said");
    sv1_clear_inflight(&c, NULL);

    uint32_t stale0 = sv1_stale_shares();
    (void)submit(&c);
    g_wall_jump = -3600;
    g_mono_step = 121;
    CHECK(sv1_inflight_count() == 0 && sv1_stale_shares() == stale0 + 1,
            "a submit unanswered for 121 s of uptime expires stale, the wall clock set back");
    sv1_free(&c);
}

int main(void)
{
    test_builders();
    test_lifecycle();
    test_xnsub();
    test_subscribe_validated();
    test_init_clears_the_session();
    test_version_mask_clamp();
    test_authorize_matches_id();
    test_submit_classification();
    test_submit_reject_reason();
    test_protocol_rules();
    test_inflight_clock();
    if (fails) {
        fprintf(stderr, "test_sv1: %d FAIL\n", fails);
        return 1;
    }
    printf("test_sv1: all pass\n");
    return 0;
}
