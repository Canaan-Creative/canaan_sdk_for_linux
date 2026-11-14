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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osif.h"
#include "miner_ipc.h"
#include "stratum_net.h"

#include "../../tests/include/tst_check.h"

#ifndef STRATUM_FIXTURES_DIR
/* Directory holding the stratum/ fixtures when the build does not set it */
#define STRATUM_FIXTURES_DIR "."
#endif

static int mock_send(void *io, const char *line);

static char g_log[64][512]; /* Captured osif_log() lines */
static int  g_log_n;        /* osif_log() lines captured */

static int (*g_log_lockprobe)(void); /* Probe run at each captured log line when set */
static int g_log_free[64];           /* Probe result per captured line; -1 when no probe was set */

/* Results counted; those delivered with g_stratum.lock free */
static int g_result_n, g_result_unlocked;

/* Job id, version, prevhash and coinbase 1 of the first accepted share in happy.shares.tsv */
static char f_job_id[64], f_version[16], f_prevhash[80], f_coinb1[1024];
/* Coinbase 2, merkle branches, nbits and job ntime of that share */
static char f_coinb2[2048], f_merkle[8192], f_nbits[16], f_ntime_job[16];
/* Extranonce1 followed by extranonce2 less its last four bytes; share difficulty */
static char    f_xnonce1[68], f_diff[32];
static uint8_t f_ntime_off; /* Share ntime minus job ntime, s */
/* Nonce, version and version mask of that share */
static uint32_t f_true_nonce, f_true_version, f_mask;

static char g_sent[4][512]; /* First four lines mock_send() sent */
static int  g_sent_n;       /* mock_send() calls */
/* Stratum transport whose send is mock_send() */
static stratum_transport g_mock = { mock_send, NULL, NULL };

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    return OSIF_OK;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)type;
    (void)data;
    (void)len;
    return OSIF_OK;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    if (g_log_n >= (int)(sizeof g_log / sizeof g_log[0]))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log[g_log_n], sizeof g_log[0], fmt, ap);
    va_end(ap);
    g_log_free[g_log_n] = g_log_lockprobe ? g_log_lockprobe() : -1;
    g_log_n++;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

static int log_find_idx(const char *needle)
{
    for (int i = 0; i < g_log_n; i++)
        if (strstr(g_log[i], needle))
            return i;
    return -1;
}
static int log_head_idx(const char *head)
{
    for (int i = 0; i < g_log_n; i++)
        if (strncmp(g_log[i], head, strlen(head)) == 0)
            return i;
    return -1;
}
static const char *log_head(const char *head)
{
    int i = log_head_idx(head);
    return i >= 0 ? g_log[i] : NULL;
}

/* Routes the product's mc_pipeline_on_result() through the lock-checking wrapper */
#define mc_pipeline_on_result test_pipeline_on_result
#define main                  core_miner_main_unused
#include "core_miner_tu.h"
#undef main
#undef mc_pipeline_on_result

void mc_pipeline_on_result(mc_pipeline *p, int accepted);

void test_pipeline_on_result(mc_pipeline *p, int accepted)
{
    g_result_n++;
    if (pthread_mutex_trylock(&g_stratum.lock) == 0) {
        g_result_unlocked++;
        pthread_mutex_unlock(&g_stratum.lock);
    }
    mc_pipeline_on_result(p, accepted);
}

static int load_share(void)
{
    FILE *f = fopen(STRATUM_FIXTURES_DIR "/stratum/happy.shares.tsv", "rb");
    if (!f)
        return -1;
    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     got = -1;
    while ((n = getline(&buf, &cap, f)) > 0) {
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        char *c[16] = { 0 }, *sp, *t = strtok_r(buf, "\t", &sp);
        int   i = 0;
        while (t && i < 16) {
            c[i++] = t;
            t      = strtok_r(NULL, "\t", &sp);
        }
        if (i < 16 || strcmp(c[15], "1") != 0)
            continue;

        size_t x2len = strlen(c[9]);
        if (x2len < 8 || strcmp(c[9] + x2len - 8, "00000000") != 0)
            continue;
        snprintf(f_xnonce1, sizeof f_xnonce1, "%s%.*s", c[8], (int)(x2len - 8), c[9]);

        snprintf(f_job_id, sizeof f_job_id, "%s", c[0]);
        snprintf(f_version, sizeof f_version, "%s", c[1]);
        snprintf(f_prevhash, sizeof f_prevhash, "%s", c[2]);
        snprintf(f_coinb1, sizeof f_coinb1, "%s", c[3]);
        snprintf(f_coinb2, sizeof f_coinb2, "%s", c[4]);
        snprintf(f_merkle, sizeof f_merkle, "%s", c[5]);
        snprintf(f_nbits, sizeof f_nbits, "%s", c[6]);
        snprintf(f_ntime_job, sizeof f_ntime_job, "%s", c[7]);
        snprintf(f_diff, sizeof f_diff, "%s", c[14]);
        f_ntime_off    = (uint8_t)(strtoul(c[10], NULL, 16) - strtoul(c[7], NULL, 16));
        f_true_nonce   = (uint32_t)strtoul(c[11], NULL, 16);
        f_true_version = (uint32_t)strtoul(c[12], NULL, 16);
        f_mask         = (uint32_t)strtoul(c[13], NULL, 16);
        got            = 0;
        break;
    }
    free(buf);
    fclose(f);
    return got;
}
static int mock_send(void *io, const char *line)
{
    (void)io;
    if (g_sent_n < 4)
        snprintf(g_sent[g_sent_n], sizeof g_sent[0], "%s", line);
    g_sent_n++;
    return 0;
}

static int stats_lock_free(void)
{
    if (pthread_mutex_trylock(&g_stats_lock) != 0)
        return 0;
    pthread_mutex_unlock(&g_stats_lock);
    return 1;
}

static void fill_client(sv1_client *c, const char *job_id, double difficulty)
{
    memset(c, 0, sizeof *c);
    c->xnonce2_size    = 4;
    c->version_rolling = 1;
    c->version_mask    = f_mask;
    c->difficulty      = difficulty;
    snprintf(c->xnonce1, sizeof c->xnonce1, "%.*s", (int)sizeof c->xnonce1 - 1, f_xnonce1);
    snprintf(c->job.job_id, sizeof c->job.job_id, "%.*s", (int)sizeof c->job.job_id - 1, job_id);
    snprintf(c->job.version, sizeof c->job.version, "%s", f_version);
    snprintf(c->job.nbits, sizeof c->job.nbits, "%s", f_nbits);
    snprintf(c->job.ntime, sizeof c->job.ntime, "%s", f_ntime_job);
    c->job.prevhash  = f_prevhash;
    c->job.coinb1    = f_coinb1;
    c->job.coinb2    = f_coinb2;
    c->job.merkle[0] = f_merkle;
    c->job.n_merkle  = 1;
}

static uint32_t deliver_job(core_session_ctx *ctx, const char *job_id, double difficulty)
{
    sv1_client c;
    fill_client(&c, job_id, difficulty);
    sv1_event ev = { .type = SV1_EV_SET_DIFFICULTY, .submit_accepted = 0 };
    on_sv1_event(ctx, &c, &ev);
    ev.type = SV1_EV_NOTIFY;
    on_sv1_event(ctx, &c, &ev);
    return g_job_seq;
}

static void deliver_set_diff(core_session_ctx *ctx, double difficulty)
{
    sv1_client c;
    fill_client(&c, f_job_id, difficulty);
    sv1_event ev = { .type = SV1_EV_SET_DIFFICULTY, .submit_accepted = 0 };
    on_sv1_event(ctx, &c, &ev);
}

static void deliver_share(uint32_t seq)
{
    avalon_nonce_out no;
    memset(&no, 0, sizeof no);
    no.valid   = 1;
    no.job_id  = seq;
    no.nonce2  = 0;
    no.nonce   = __builtin_bswap32(f_true_nonce);
    no.version = __builtin_bswap32(f_true_version);
    no.ntime   = f_ntime_off;
    nonce_cb(MSG_ASIC_NONCE, &no, (int)sizeof no);
}

static void session_client(sv1_client *rc)
{
    sv1_event ev;
    sv1_init(rc);
    sv1_feed_line(rc,
            "{\"id\":1,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"1fffe000\"},\"error\":null}",
            &ev);
    sv1_feed_line(rc,
            "{\"id\":2,\"result\":[[[\"mining.notify\",\"a\"]],\"00\",8],"
            "\"error\":null}",
            &ev);
    sv1_feed_line(rc, "{\"id\":3,\"result\":true,\"error\":null}", &ev);
}

static void deliver_result(core_session_ctx *ctx, sv1_client *rc, const char *line)
{
    sv1_event ev;
    sv1_feed_line(rc, line, &ev);
    on_sv1_event(ctx, rc, &ev);
}

static void reset_state(mc_pipeline *p)
{
    mc_pipeline_init(p);
    memset(g_ring, 0, sizeof g_ring);
    memset(g_sub, 0, sizeof g_sub);
    g_sub_w         = 0;
    g_job_seq       = 0;
    g_dead_seq      = 0;
    g_nst.hw_errors = 0;
    g_sent_n        = 0;
    g_log_n         = 0;
    for (int i = 0; i < POOL_VALID_NUM; i++) {
        g_pool_st[i].rej    = 0;
        g_pool_st[i].drej   = 0;
        g_pool_st[i].acc    = 0;
        g_pool_st[i].dacc   = 0;
        g_pool_st[i].lsdiff = 0;
    }
    memset(g_mw, 0, sizeof g_mw);
    g_diff_acc = g_diff_rej = 0;
    g_stratum.pipe          = p;
    g_stratum.t             = &g_mock;
    g_stratum.have_job      = 0;
    publish_active_pool(0);
}

static int submit_one(core_session_ctx *ctx, mc_pipeline *p, double difficulty)
{
    uint32_t seq = deliver_job(ctx, f_job_id, difficulty);
    deliver_share(seq);
    if (g_sent_n != 1)
        return -1;
    g_log_n = 0;
    return p->sv1.next_id - 1;
}

static int submit_next(core_session_ctx *ctx, mc_pipeline *p, const char *job_id, double difficulty)
{
    deliver_share(deliver_job(ctx, job_id, difficulty));
    return p->sv1.next_id - 1;
}

int main(void)
{
    if (load_share() != 0) {
        fprintf(stderr, "  FAIL: no usable accepted share in happy.shares.tsv\n");
        return 2;
    }

    g_log_lockprobe = stats_lock_free;

    mc_pipeline      pipe;
    core_session_ctx ctx = { .pipe = &pipe, .user = "poolworker.001" };
    double           d   = strtod(f_diff, NULL);
    char             needle[64];
    char             line[256];
    sv1_client       rc;

    reset_state(&pipe);
    session_client(&rc);
    uint64_t t0 = now_ms();
    int      id = submit_one(&ctx, &pipe, d);
    CHECK(id > 0, "the fixture share was really submitted and got an id (%d)", id);

    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[21,\"Job not found\",null]}",
            id);
    nanosleep(&(struct timespec){ 0, 5 * 1000 * 1000 }, NULL);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Rejected ");
        CHECK(l != NULL, "a rejected share is logged at all%s", l ? "" : " (NO LINE)");
        CHECK(l && strstr(l, "Job not found"), "the line carries the pool's reason (%s)",
                l ? l : "-");
        CHECK(l && strstr(l, "code=21"), "...and the pool's error code");
        snprintf(needle, sizeof needle, "id=%d", id);
        CHECK(l && strstr(l, needle), "...and the id it answers (%s)", needle);
        snprintf(needle, sizeof needle, "jobdiff=%s", f_diff);
        CHECK(l && strstr(l, needle),
                "...and the diff the share was VERIFIED against "
                "(%s) — i.e. the record was found by id",
                needle);
        CHECK(l && strstr(l, "job=1 age=0"), "...and the share's own job seq + age");
        snprintf(needle, sizeof needle, "nonce=%08x", f_true_nonce);
        CHECK(l && strstr(l, needle), "...and the nonce (%s)", needle);
        {
            char sfx[16];
            diff_suffix(g_sub[0].sdiff, sfx, sizeof sfx);
            snprintf(needle, sizeof needle, "Rejected %08x Diff %s/", g_sub[0].h32, sfx);
            CHECK(l && strstr(l, needle), "...and the hash word and difficulty it verified at (%s)",
                    needle);
        }
        CHECK(l && !strstr(l, "jobdiff=?"), "the record is NOT reported as missing (%s)",
                l ? l : "-");
        uint64_t t1 = now_ms();
        CHECK(g_sub[0].at_ms >= t0 && g_sub[0].at_ms <= t1,
                "the ring records the submit's monotonic ms (%llu in [%llu..%llu])",
                (unsigned long long)g_sub[0].at_ms, (unsigned long long)t0, (unsigned long long)t1);
        {
            unsigned long long ss = 0, sms = 0, dt = 0;
            const char        *p = l ? strstr(l, "sub=") : NULL;
            const char        *q = l ? strstr(l, "dt=") : NULL;
            CHECK(p && sscanf(p, "sub=%llu.%3llu", &ss, &sms) == 2 &&
                            ss * 1000u + sms == g_sub[0].at_ms,
                    "...and the line carries it as sec.ms (%s)", p ? p : "no sub= field");
            CHECK(q && sscanf(q, "dt=%llu", &dt) == 1 && dt >= 4 && dt <= t1 - t0,
                    "...and the ms from submit to this reject, really measured "
                    "(dt=%llu, slept 5, bound %llu)",
                    dt, (unsigned long long)(t1 - t0));
        }
        CHECK(g_pool_st[0].rej == 1 && g_pool_st[0].drej == d,
                "the reject is also accounted per pool (n=%u diff=%.0f)", g_pool_st[0].rej,
                g_pool_st[0].drej);
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    (void)deliver_job(&ctx, "NEXTJOB", d * 2.0);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[23,\"Low difficulty share\",null]}",
            id);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Rejected ");
        char        want[64];
        snprintf(want, sizeof want, "/%.8g ASC", d * 2.0);
        CHECK(l && strstr(l, want), "a vardiff step shows as %s (%s)", want, l ? l : "-");
        CHECK(g_pool_st[0].drej == d && g_diff_rej == d,
                "...and the ledger is charged the share's own diff, not the live "
                "one (pool=%.0f device=%.0f, want %.0f, live %.0f)",
                g_pool_st[0].drej, g_diff_rej, d, d * 2.0);
        snprintf(want, sizeof want, "jobdiff=%.8g", d);
        CHECK(l && strstr(l, want),
                "...with the line still reporting both, so the two can be compared "
                "(%s)",
                want);
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    (void)deliver_job(&ctx, "NEXTJOB", d * 2.0);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    deliver_result(&ctx, &rc, line);
    CHECK(g_pool_st[0].dacc == d && g_diff_acc == d && g_pool_st[0].lsdiff == d,
            "an accepted share is credited at the diff it was verified against "
            "(pool=%.0f device=%.0f last=%.0f, want %.0f, live %.0f)",
            g_pool_st[0].dacc, g_diff_acc, g_pool_st[0].lsdiff, d, d * 2.0);

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    deliver_set_diff(&ctx, d * 2.0);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[23,\"Low difficulty share\",null]}",
            id);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Rejected ");
        char        want[64];
        snprintf(want, sizeof want, "/%.8g ASC", d);
        CHECK(l && strstr(l, want),
                "an announced-but-not-promoted diff is not reported as a step (%s)", l ? l : "-");
        CHECK(l && strstr(l, "Low difficulty share"), "...with the pool's own wording");
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":false,\"error\":\"stale\"}", id);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Rejected ");
        CHECK(l && strstr(l, "stale"), "a bare-string error reaches the line (%s)", l ? l : "-");
        CHECK(l && strstr(l, "code=0"), "...with no code, since the shape carries none");
        snprintf(needle, sizeof needle, "jobdiff=%s", f_diff);
        CHECK(l && strstr(l, needle), "...and still joins to its submit record");
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,\"error\":[23,"
            "\"Low difficulty share\",null]}",
            id);
    deliver_result(&ctx, &rc, line);
    id      = submit_next(&ctx, &pipe, "JOB2", d);
    g_log_n = 0;
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":false,\"error\":null}", id);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Rejected ");
        CHECK(l != NULL, "a reason-less reject is still logged%s", l ? "" : " (NO LINE)");
        CHECK(l && strstr(l, "(no reason given)"), "...as having no reason (%s)", l ? l : "-");
        CHECK(l && !strstr(l, "Low difficulty share"),
                "...and does NOT reuse the previous share's reason");
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    for (int i = 0; i < SUBMIT_LOG; i++) {
        char jid[16];
        snprintf(jid, sizeof jid, "ROLL%d", i);
        (void)submit_next(&ctx, &pipe, jid, d);
    }
    (void)deliver_job(&ctx, "LIVEJOB", d * 4.0);
    g_log_n = 0;
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[21,\"Job not found\",null]}",
            id);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Rejected ");
        CHECK(l && strstr(l, "Job not found"), "an unmatched reject still reports the reason (%s)",
                l ? l : "-");
        CHECK(l && strstr(l, "jobdiff=?"),
                "...and marks the lost share diff as unknown (jobdiff=?)");
        CHECK(l && strstr(l, "sub=? dt=?"),
                "...and the submit time reads as unknown, not as zero (%s)", l ? l : "-");
        CHECK(l && strstr(l, "job=? age=?") && strstr(l, "n2=? nonce=? roll=?"),
                "...as do the job, age, extranonce2, nonce and ntime roll (%s)", l ? l : "-");
        CHECK(l && strstr(l, "Rejected ? Diff ?/"),
                "...and so do the hash word and the share difficulty the line "
                "leads with (%s)",
                l ? l : "-");
        CHECK(l && !strstr(l, "job=0 ") && !strstr(l, "nonce=00000000") &&
                        !strstr(l, "Rejected 00000000") && !strstr(l, "Diff 0/"),
                "...and NOT as the zeros that read like data");
        CHECK(g_pool_st[0].drej == d * 4.0 && g_diff_rej == d * 4.0,
                "...and the ledger falls back to the live diff (pool=%.0f "
                "device=%.0f, want %.0f)",
                g_pool_st[0].drej, g_diff_rej, d * 4.0);
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    deliver_result(&ctx, &rc, line);
    CHECK(log_head("Rejected ") == NULL, "an accepted share logs no reject%s",
            log_head("Rejected ") ? " — but it did" : "");
    CHECK(g_pool_st[0].rej == 0, "...and counts no reject (%u)", g_pool_st[0].rej);

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    deliver_result(&ctx, &rc, line);
    {
        char *l = sv1_build_extranonce_subscribe(&rc);
        free(l);
    }
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", rc.next_id - 1);
    deliver_result(&ctx, &rc, line);
    {
        unsigned long long mw = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            for (int a = 0; a < MINER_MAX_CHIPS; a++)
                mw += g_mw[b][a];
        CHECK(pipe.tally.accepted == 1 && g_pool_st[0].acc == 1,
                "the extranonce.subscribe reply is not an accepted share "
                "(tally=%u pool=%u)",
                pipe.tally.accepted, g_pool_st[0].acc);
        CHECK(mw == pipe.tally.accepted,
                "the one submitted share is the one accepted share: MW[] sums to "
                "Accepted here because nothing was rejected (%llu vs %u)",
                mw, pipe.tally.accepted);
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    pthread_mutex_lock(&g_stats_lock);
    g_ping.sent_ms -= 1500;
    pthread_mutex_unlock(&g_stats_lock);
    g_log_n = 0;
    snprintf(line, sizeof line,
            "{\"id\":%d,\"result\":false,"
            "\"error\":[23,\"Low difficulty share\",null]}",
            id);
    deliver_result(&ctx, &rc, line);
    {
        CHECK(stats_lock_free(), "the probe reads a free lock as free (control)");
        int li = log_find_idx("s to answer a share");
        CHECK(li >= 0, "a slow share result is reported (%s)", li >= 0 ? g_log[li] : "NO LINE");
        CHECK(li >= 0 && g_log_free[li] == 1, "...with g_stats_lock already released (%d)",
                li >= 0 ? g_log_free[li] : -2);
        int rj = log_head_idx("Rejected ");
        CHECK(rj >= 0 && g_log_free[rj] == 1, "...and so is the reject line beside it (%d)",
                rj >= 0 ? g_log_free[rj] : -2);
    }

    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    for (int i = 0; i < SUBMIT_LOG; i++) {
        char jid[16];
        snprintf(jid, sizeof jid, "AGE%d", i);
        (void)submit_next(&ctx, &pipe, jid, d);
    }
    g_log_n = 0;
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Accepted ");
        CHECK(l != NULL, "an unmatched accept is still logged%s", l ? "" : " (NO LINE)");
        CHECK(l && strstr(l, "Accepted ? Diff ?/"), "...with its unknown fields as \"?\" (%s)",
                l ? l : "-");
        CHECK(l && !strstr(l, "Accepted 00000000") && !strstr(l, "Diff 0/"),
                "...not as 00000000 and 0");
    }
    reset_state(&pipe);
    session_client(&rc);
    id = submit_one(&ctx, &pipe, d);
    snprintf(line, sizeof line, "{\"id\":%d,\"result\":true,\"error\":null}", id);
    deliver_result(&ctx, &rc, line);
    {
        const char *l = log_head("Accepted ");
        char        sfx[16];
        diff_suffix(g_sub[0].sdiff, sfx, sizeof sfx);
        snprintf(needle, sizeof needle, "Accepted %08x Diff %s/", g_sub[0].h32, sfx);
        CHECK(l && strstr(l, needle), "a joined accept names the share it answers (%s)", needle);
    }

    CHECK(g_result_n > 0 && g_result_unlocked == 0,
            "every pool result is tallied with the session lock held "
            "(%d of %d unlocked)",
            g_result_unlocked, g_result_n);

    if (fails) {
        fprintf(stderr, "test_core_reject: %d FAIL\n", fails);
        return 1;
    }
    printf("test_core_reject: all pass\n");
    return 0;
}
