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
#include <pthread.h>

#include "osif.h"
#include "miner_ipc.h"
#include "jread.h"
#include "sha256.h"
#include "work.h"
#include "pool.h"
#include "stratum_net.h"

#include "../../tests/include/tst_check.h"
#include "mc_api_lcd.h"

#ifndef STRATUM_FIXTURES_DIR
/* Directory holding the stratum/ fixtures when the build does not set it */
#define STRATUM_FIXTURES_DIR "."
#endif
#define VARDIFF_UP 4294967296.0

static int mock_send(void *io, const char *line);
static int mock_send_fail(void *io, const char *line);

static mc_asic_job   g_tmpl;          /* Last MSG_ASIC_JOB template sent */
static int           g_tmpl_n;        /* MSG_ASIC_JOB templates sent */
static int           g_tmpl_unlocked; /* Templates sent with g_stratum.lock free */
static mc_core_stats g_cs;            /* Last MSG_CORE_STATS payload sent */
static int           g_cs_n;          /* MSG_CORE_STATS messages sent */

static char g_log[128][256]; /* Captured osif_log() lines */
static int  g_log_n;         /* osif_log() lines captured */

static int64_t g_mono_off_ms; /* Offset added to CLOCK_MONOTONIC, ms */
static int64_t g_wall_off_s;  /* Offset added to time(), s */

static pool_set    g_conf;       /* Pool config that test_cfgwr_update_pools() updates */
static int         g_hdr_hashes; /* sha256d() calls on an 80-byte block header */
static pool_set    g_pub_set[2]; /* Pool lists pub_thread() publishes in turn */
static _Atomic int g_pub_stop;   /* Nonzero stops pub_thread() */
static _Atomic int g_pub_n;      /* publish_pools() calls pub_thread() made */

/* Job id, version, prevhash and coinbase 1 of the first accepted share in happy.shares.tsv */
static char f_job_id[64], f_version[16], f_prevhash[80], f_coinb1[1024];
/* Coinbase 2, merkle branches, nbits and job ntime of that share */
static char f_coinb2[2048], f_merkle[8192], f_nbits[16], f_ntime_job[16];
/* Extranonce1 followed by extranonce2 less its last four bytes; share difficulty */
static char    f_xnonce1[68], f_diff[32];
static uint8_t f_ntime_off; /* Share ntime minus job ntime, s */
/* Nonce, version and version mask of that share */
static uint32_t f_true_nonce, f_true_version, f_mask;
static int      f_no_rolling; /* Non-zero makes fill_client() disable version rolling */

static char g_sent[4][512];   /* First four lines mock_send() sent */
static int  g_sent_n;         /* mock_send() calls */
static int  g_send_lock_free; /* mock_send() calls made with g_stratum.lock free */
static int  g_send_guarded;   /* mock_send() calls made with g_stratum.send held */

static int g_drop_probe; /* Non-zero makes mock_send() run session_drop() on another thread */
/* Drop thread started; still blocked 50 ms after starting; finished */
static volatile int g_drop_started, g_drop_pending_seen, g_drop_done;
static pthread_t    g_drop_th; /* Thread running session_drop() */
/* Stratum transport whose send is mock_send() */
static stratum_transport g_mock = { mock_send, NULL, NULL };
/* Stratum transport whose send always fails */
static stratum_transport g_mock_fail = { mock_send_fail, NULL, NULL };

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    return OSIF_OK;
}
static int    template_lock_free(void);
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (type == MSG_ASIC_JOB && data && len == (int)sizeof(mc_asic_job)) {
        memcpy(&g_tmpl, data, sizeof g_tmpl);
        g_tmpl_n++;
        g_tmpl_unlocked += template_lock_free();
    }
    if (type == MSG_CORE_STATS && data && len == (int)sizeof(mc_core_stats)) {
        memcpy(&g_cs, data, sizeof g_cs);
        g_cs_n++;
    }
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
    g_log_n++;
}
static const char *log_find(const char *needle)
{
    for (int i = 0; i < g_log_n; i++)
        if (strstr(g_log[i], needle))
            return g_log[i];
    return NULL;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

static int test_clock_gettime(clockid_t id, struct timespec *ts)
{
    int rc = clock_gettime(id, ts);
    if (rc == 0 && id == CLOCK_MONOTONIC) {
        int64_t ns = ts->tv_nsec + (g_mono_off_ms % 1000) * 1000000;
        ts->tv_sec += g_mono_off_ms / 1000 + ns / 1000000000;
        ts->tv_nsec = ns % 1000000000;
    }
    return rc;
}

static time_t test_time(time_t *t)
{
    time_t v = time(NULL) + g_wall_off_s;
    if (t)
        *t = v;
    return v;
}

#define clock_gettime test_clock_gettime
#define time(t)       test_time(t)
/* Routes the product's cfgwr_update_pools() to the in-memory g_conf */
#define cfgwr_update_pools test_cfgwr_update_pools
#define main               core_miner_main_unused
#include "core_miner_tu.h"
#undef main
#undef cfgwr_update_pools
#undef time
#undef clock_gettime

void __real_sha256d(const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
void __wrap_sha256d(const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
void __wrap_sha256d(const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    if (len == 80)
        g_hdr_hashes++;
    __real_sha256d(msg, len, out);
}

static int template_lock_free(void)
{
    if (pthread_mutex_trylock(&g_stratum.lock) != 0)
        return 0;
    pthread_mutex_unlock(&g_stratum.lock);
    return 1;
}

static void *pub_thread(void *a)
{
    (void)a;
    for (int i = 0; !atomic_load(&g_pub_stop); i++) {
        publish_pools(&g_pub_set[i & 1]);
        atomic_fetch_add(&g_pub_n, 1);
    }
    return NULL;
}

int test_cfgwr_update_pools(const char *path, pool_config_update_fn update, void *ctx)
{
    (void)path;
    pool_set ps = g_conf;
    if (update(&ps, ctx) != 0)
        return -1;
    g_conf = ps;
    return 0;
}

static int load_share_vbits(const char *vbits)
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
        if (vbits && strcmp(c[12], vbits) != 0)
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

static int load_share(void)
{
    return load_share_vbits(NULL);
}
static void nap_ms(long ms)
{
    nanosleep(&(struct timespec){ ms / 1000, (ms % 1000) * 1000000L }, NULL);
}
static void *drop_thread(void *a)
{
    (void)a;
    g_drop_started = 1;
    session_drop();
    g_drop_done = 1;
    return NULL;
}

static int mock_send(void *io, const char *line)
{
    (void)io;
    if (pthread_mutex_trylock(&g_stratum.lock) == 0) {
        g_send_lock_free++;
        pthread_mutex_unlock(&g_stratum.lock);
    }
    if (pthread_mutex_trylock(&g_stratum.send) == 0)
        pthread_mutex_unlock(&g_stratum.send);
    else
        g_send_guarded++;
    if (g_drop_probe) {
        g_drop_started = g_drop_pending_seen = g_drop_done = 0;
        pthread_create(&g_drop_th, NULL, drop_thread, NULL);
        for (int i = 0; i < 200 && !g_drop_started; i++)
            nap_ms(1);
        nap_ms(50);
        g_drop_pending_seen = !g_drop_done;
    }
    if (g_sent_n < 4)
        snprintf(g_sent[g_sent_n], sizeof g_sent[0], "%s", line);
    g_sent_n++;
    return 0;
}
static int mock_send_fail(void *io, const char *line)
{
    (void)io;
    (void)line;
    return -1;
}

static void fill_client(sv1_client *c, const char *job_id, const char *xnonce1)
{
    memset(c, 0, sizeof *c);
    c->xnonce2_size    = 4;
    c->version_rolling = !f_no_rolling;
    c->version_mask    = f_no_rolling ? 0 : f_mask;
    c->difficulty      = strtod(f_diff, NULL);
    snprintf(c->xnonce1, sizeof c->xnonce1, "%s", xnonce1);
    snprintf(c->job.job_id, sizeof c->job.job_id, "%s", job_id);
    snprintf(c->job.version, sizeof c->job.version, "%s", f_version);
    snprintf(c->job.nbits, sizeof c->job.nbits, "%s", f_nbits);
    snprintf(c->job.ntime, sizeof c->job.ntime, "%s", f_ntime_job);
    c->job.prevhash  = f_prevhash;
    c->job.coinb1    = f_coinb1;
    c->job.coinb2    = f_coinb2;
    c->job.merkle[0] = f_merkle;
    c->job.n_merkle  = 1;
}

static uint32_t deliver_job_at_diff(core_session_ctx *ctx, const char *job_id, const char *xnonce1,
        double diff)
{
    sv1_client c;
    fill_client(&c, job_id, xnonce1);
    c.difficulty = diff;
    sv1_event ev = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
    on_sv1_event(ctx, &c, &ev);
    return g_job_seq;
}

static uint32_t deliver_job(core_session_ctx *ctx, const char *job_id, const char *xnonce1)
{
    sv1_client c;
    fill_client(&c, job_id, xnonce1);
    sv1_event ev = { .type = SV1_EV_SET_DIFFICULTY, .submit_accepted = 0 };
    on_sv1_event(ctx, &c, &ev);
    ev.type = SV1_EV_NOTIFY;
    on_sv1_event(ctx, &c, &ev);
    return g_job_seq;
}

static void deliver_set_difficulty(core_session_ctx *ctx, double diff)
{
    sv1_client c;
    fill_client(&c, f_job_id, f_xnonce1);
    c.difficulty = diff;
    sv1_event ev = { .type = SV1_EV_SET_DIFFICULTY, .submit_accepted = 0 };
    on_sv1_event(ctx, &c, &ev);
}

static uint32_t deliver_clean_job(core_session_ctx *ctx, const char *job_id, const char *xnonce1)
{
    sv1_client c;
    fill_client(&c, job_id, xnonce1);
    c.job.clean_jobs = 1;
    sv1_event ev     = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
    on_sv1_event(ctx, &c, &ev);
    return g_job_seq;
}

static void deliver_nonce_from(uint32_t seq, uint16_t mid, uint16_t aid)
{
    avalon_nonce_out no;
    memset(&no, 0, sizeof no);
    no.valid    = 1;
    no.job_id   = seq;
    no.nonce2   = 0;
    no.nonce    = __builtin_bswap32(f_true_nonce);
    no.version  = __builtin_bswap32(f_true_version);
    no.ntime    = f_ntime_off;
    no.miner_id = mid;
    no.asic_id  = aid;
    nonce_cb(MSG_ASIC_NONCE, &no, (int)sizeof no);
}

static void deliver_nonce(uint32_t seq)
{
    deliver_nonce_from(seq, 0, 0);
}

static void win_age_one_second(void)
{
    pthread_mutex_lock(&g_stats_lock);
    g_hashwin.slot[(g_hashwin.at - 1) % MHS_WIN_SLOTS] +=
            g_hashwin.slot[g_hashwin.at % MHS_WIN_SLOTS];
    g_hashwin.slot[g_hashwin.at % MHS_WIN_SLOTS] = 0;
    pthread_mutex_unlock(&g_stats_lock);
}

static void reset_state(mc_pipeline *p)
{
    mc_pipeline_init(p);
    memset(g_ring, 0, sizeof g_ring);
    g_job_seq  = 0;
    g_dead_seq = 0;
    zero_all_stats();
    g_sent_n         = 0;
    g_send_lock_free = g_send_guarded = 0;
    g_log_n                           = 0;
    g_stratum.pipe                    = p;
    g_stratum.t                       = &g_mock;
    g_stratum.have_job                = 0;
    publish_active_pool(0);
}

int main(void)
{
    if (load_share() != 0) {
        fprintf(stderr, "  FAIL: no usable accepted share in happy.shares.tsv\n");
        return 2;
    }

    mc_pipeline      pipe;
    core_session_ctx ctx = { .pipe = &pipe, .user = "poolworker.001" };

    reset_state(&pipe);
    g_cs_n = 0;
    core_stats_push();
    CHECK(g_cs_n == 1 && g_cs.job_gap_s == 0,
            "MSG_CORE_STATS reports no job gap before the first pool job (%u)", g_cs.job_gap_s);
    uint32_t seq = deliver_job(&ctx, f_job_id, f_xnonce1);
    deliver_nonce(seq);
    CHECK(g_sent_n == 1, "live-job nonce -> one mining.submit (%d)", g_sent_n);
    CHECK(g_sent_n == 1 && strstr(g_sent[0], f_job_id) != NULL,
            "the submit carries the live job id (%s)", f_job_id);
    CHECK(g_pool_st[0].diff1 > 0, "the live share counts into Diff1 Shares (%.0f)",
            g_pool_st[0].diff1);
    CHECK(g_nst.hw_errors == 0, "no HW error for the live share (%u)", g_nst.hw_errors);
    {
        char want[9];
        snprintf(want, sizeof want, "%08x", f_true_version & f_mask);
        json_value       *v  = g_sent_n == 1 ? json_parse(g_sent[0]) : NULL;
        const json_value *pa = v ? json_object_get(v, "params") : NULL;
        CHECK(json_array_size(pa) == 6, "a negotiated version-rolling submit carries 6 params (%s)",
                g_sent_n ? g_sent[0] : "-");
        const char *vb = json_string(json_array_get(pa, 5));
        CHECK(!strcmp(vb, want),
                "...and params[5] is the rolled version masked by the grant "
                "(%s, want %s)",
                vb, want);
        json_free(v);
    }
    {
        CHECK(load_share_vbits("00000000") == 0,
                "the fixture holds an accepted share with no rolled bits");
        f_no_rolling = 1;
        reset_state(&pipe);
        uint32_t nseq = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(nseq);
        json_value *v = g_sent_n == 1 ? json_parse(g_sent[0]) : NULL;
        CHECK(v && json_array_size(json_object_get(v, "params")) == 5,
                "without a version-rolling grant the submit carries 5 params (%s)",
                g_sent_n ? g_sent[0] : "-");
        json_free(v);
        f_no_rolling = 0;
        load_share();
    }

    {
        char worker[129];
        memset(worker, 'w', 128);
        worker[128] = 0;
        ctx.user    = worker;
        reset_state(&pipe);
        uint32_t longseq = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(longseq);
        CHECK(g_sent_n == 1 && strstr(g_sent[0], worker),
                "128-byte worker survives notify through mining.submit");
        ctx.user = "poolworker.001";
    }

    double share_d = g_pool_st[0].best;
    CHECK(share_d > 0 && share_d < VARDIFF_UP,
            "the fixture share's difficulty %.0f is below the raised pool diff %.0f", share_d,
            VARDIFF_UP);

    reset_state(&pipe);
    uint32_t oldseq = deliver_job(&ctx, f_job_id, f_xnonce1);
    deliver_set_difficulty(&ctx, VARDIFF_UP);
    deliver_nonce(oldseq);
    CHECK(g_sent_n == 1 && g_nst.low_diff == 0,
            "a staged step does not touch the live job's gate (sent=%d lowdiff=%u)", g_sent_n,
            g_nst.low_diff);

    reset_state(&pipe);
    oldseq = deliver_job(&ctx, f_job_id, f_xnonce1);
    (void)deliver_job_at_diff(&ctx, "NEXTJOB", f_xnonce1, VARDIFF_UP);
    deliver_nonce(oldseq);
    CHECK(g_sent_n == 1, "a promoted step does not refuse the older job's share (sent=%d)",
            g_sent_n);
    CHECK(g_nst.low_diff == 0, "...and it is not counted below-target (%u)", g_nst.low_diff);
    CHECK(g_nst.hw_errors == 0, "...and never a HW error (%u)", g_nst.hw_errors);

    reset_state(&pipe);
    (void)deliver_job(&ctx, f_job_id, f_xnonce1);
    uint32_t hardseq = deliver_job_at_diff(&ctx, "HARDJOB", f_xnonce1, VARDIFF_UP);
    deliver_nonce(hardseq);
    CHECK(g_sent_n == 0, "a share under ITS OWN job's difficulty is not submitted%s%s",
            g_sent_n ? " — sent: " : "", g_sent_n ? g_sent[0] : "");
    CHECK(g_nst.low_diff == 1,
            "...it is counted below-target, the API's \"Share above target\" "
            "(%u)",
            g_nst.low_diff);
    CHECK(g_nst.hw_errors == 0 && g_mw[0][0] == 1,
            "...and is the chip's work, not a HW error (hw=%u mw=%llu)", g_nst.hw_errors,
            (unsigned long long)g_mw[0][0]);

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_xnonce1);
    {
        job_tmpl snap = g_ring[seq % JOB_RING];
        uint32_t th_job, tl_job, th_live, tl_live;
        mc_asic_target(f_diff, &th_job, &tl_job);
        mc_asic_target("512", &th_live, &tl_live);
        CHECK(th_job != th_live || tl_job != tl_live,
                "the job's diff %s and a later %s program different chip targets", f_diff, "512");
        deliver_set_difficulty(&ctx, 512.0);
        g_tmpl_n = 0;
        send_asic_template(&snap);
        CHECK(g_tmpl_n == 1 && g_tmpl.target_h == th_job && g_tmpl.target_l == tl_job,
                "the chip target is the job's own (%08x:%08x, want %08x:%08x)", g_tmpl.target_h,
                g_tmpl.target_l, th_job, tl_job);
    }

    {
        static char big[2 * MC_COINBASE_MAX + 2];
        memset(big, 'a', sizeof big - 1);
        sv1_client c;
        fill_client(&c, "BIGCOINB", f_xnonce1);
        c.job.coinb2 = big;
        sv1_event ev = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
        reset_state(&pipe);
        on_sv1_event(&ctx, &c, &ev);
        uint32_t bseq = g_job_seq;
        CHECK(bseq == 0 && !g_stratum.have_job,
                "a coinbase longer than the template is not taken as a job "
                "(seq %u, have_job %d)",
                bseq, g_stratum.have_job);
        deliver_nonce(bseq);
        CHECK(g_sent_n == 0, "...and its nonces submit nothing%s%s", g_sent_n ? " — sent: " : "",
                g_sent_n ? g_sent[0] : "");
    }

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_xnonce1);
    {
        const job_tmpl      *jt         = &g_ring[seq % JOB_RING];
        static const uint8_t zero32[32] = { 0 };
        CHECK(jt->hdr_ok, "the notify stored a block header for the job (%d)", jt->hdr_ok);
        CHECK(memcmp(jt->hdr + 36, zero32, 32) == 0 && jt->hdr[76] == 0 && jt->hdr[77] == 0 &&
                        jt->hdr[78] == 0 && jt->hdr[79] == 0,
                "...with a blank merkle root and a zero nonce (pool->header_bin)");
        uint32_t h_ntime = jt->hdr[68] | (jt->hdr[69] << 8) | (jt->hdr[70] << 16) |
                ((uint32_t)jt->hdr[71] << 24);
        uint32_t h_nbits = jt->hdr[72] | (jt->hdr[73] << 8) | (jt->hdr[74] << 16) |
                ((uint32_t)jt->hdr[75] << 24);
        CHECK(h_ntime == (uint32_t)strtoul(f_ntime_job, NULL, 16) &&
                        h_nbits == (uint32_t)strtoul(f_nbits, NULL, 16),
                "...carrying the notify's base ntime and nbits (%08x/%08x)", h_ntime, h_nbits);
    }
    g_ring[seq % JOB_RING].hdr[4] ^= 0xff;
    deliver_nonce(seq);
    CHECK(g_sent_n == 0 && g_nst.hw_verify == 1,
            "a corrupted stored header fails the verify, so the check reads it "
            "(sent=%d verify=%u)",
            g_sent_n, g_nst.hw_verify);

    reset_state(&pipe);
    seq          = deliver_job(&ctx, f_job_id, f_xnonce1);
    g_hdr_hashes = 0;
    deliver_nonce(seq);
    CHECK(g_sent_n == 1 && g_hdr_hashes == 1,
            "a submitted share hashes its header once (sent=%d hashes=%d)", g_sent_n, g_hdr_hashes);
    {
        uint32_t true_nonce = f_true_nonce;
        f_true_nonce ^= 0x80000000u;
        g_hdr_hashes = 0;
        deliver_nonce(seq);
        f_true_nonce = true_nonce;
    }
    CHECK(g_sent_n == 1 && g_nst.hw_verify == 1 && g_nst.hw_errors == 1,
            "a tampered nonce fails the verify: one HW error, nothing submitted "
            "(sent=%d verify=%u hw=%u)",
            g_sent_n, g_nst.hw_verify, g_nst.hw_errors);
    CHECK(g_hdr_hashes == 1, "...after hashing its header once (%d)", g_hdr_hashes);

    reset_state(&pipe);
    (void)deliver_job(&ctx, f_job_id, f_xnonce1);
    seq          = deliver_job_at_diff(&ctx, "HARDJOB", f_xnonce1, 4294967296.0);
    g_hdr_hashes = 0;
    deliver_nonce(seq);
    CHECK(g_sent_n == 0 && g_nst.low_diff == 1 && g_hdr_hashes == 1,
            "a share under the job's difficulty hashes its header once "
            "(sent=%d lowdiff=%u hashes=%d)",
            g_sent_n, g_nst.low_diff, g_hdr_hashes);

    reset_state(&pipe);
    seq                           = deliver_job(&ctx, f_job_id, f_xnonce1);
    g_ring[seq % JOB_RING].hdr_ok = 0;
    deliver_nonce(seq);
    CHECK(g_sent_n == 0, "a job with no usable stored header submits nothing%s%s",
            g_sent_n ? " — sent: " : "", g_sent_n ? g_sent[0] : "");

    reset_state(&pipe);
    uint32_t stale_seq = deliver_job(&ctx, f_job_id, f_xnonce1);

    session_drop();

    g_stratum.t       = &g_mock;
    uint32_t live_seq = deliver_job(&ctx, "NEWJOBID", "deadbeef00");
    CHECK(live_seq != stale_seq, "the reconnect issued a new job seq (%u -> %u)", stale_seq,
            live_seq);

    deliver_nonce(stale_seq);

    CHECK(g_sent_n == 0, "stale-job nonce after reconnect is dropped, not submitted%s%s",
            g_sent_n ? " — sent: " : "", g_sent_n ? g_sent[0] : "");
    CHECK(g_pool_st[0].diff1 == 0, "the dropped nonce does not inflate Diff1 Shares (%.0f)",
            g_pool_st[0].diff1);
    CHECK(g_pool_st[0].best == 0, "the dropped nonce does not inflate Best Share (%.0f)",
            g_pool_st[0].best);
    CHECK(g_mw[0][0] == 0, "the dropped nonce does not inflate MW (%llu)",
            (unsigned long long)g_mw[0][0]);
    CHECK(g_nst.hw_errors == 0, "a retired-job nonce is not counted as a HW error (hw=%u)",
            g_nst.hw_errors);

    deliver_nonce(9999);
    CHECK(g_sent_n == 0 && g_nst.hw_errors == 1,
            "a nonce for an unknown job seq still counts one HW error (hw=%u)", g_nst.hw_errors);

    reset_state(&pipe);
    uint32_t old_seq   = deliver_job(&ctx, f_job_id, f_xnonce1);
    uint32_t clean_seq = deliver_clean_job(&ctx, "CLEANJOB", f_xnonce1);
    CHECK(clean_seq == old_seq + 1, "the clean notify took the next ring slot (%u -> %u)", old_seq,
            clean_seq);
    CHECK(g_dead_seq == old_seq,
            "clean_jobs advances the dead watermark to the abandoned generation "
            "(dead=%u, want %u)",
            g_dead_seq, old_seq);

    deliver_nonce(old_seq);
    CHECK(g_sent_n == 0, "a nonce for the abandoned job is dropped, not submitted%s%s",
            g_sent_n ? " — sent: " : "", g_sent_n ? g_sent[0] : "");
    CHECK(g_nst.retired == 1, "...and it is counted as retired, not lost (%u)", g_nst.retired);
    CHECK(g_nst.hw_errors == 0, "...and never as a HW error (hw=%u)", g_nst.hw_errors);
    CHECK(g_pool_st[0].diff1 == 0 && g_mw[0][0] == 0,
            "...and inflates neither Diff1 Shares nor MW (%.0f / %llu)", g_pool_st[0].diff1,
            (unsigned long long)g_mw[0][0]);

    deliver_nonce(clean_seq);
    CHECK(g_sent_n == 1, "the clean notify's own job still submits (%d)", g_sent_n);
    CHECK(g_sent_n == 1 && strstr(g_sent[0], "CLEANJOB") != NULL, "...carrying the new job id (%s)",
            g_sent_n ? g_sent[0] : "-");

    reset_state(&pipe);
    old_seq           = deliver_job(&ctx, f_job_id, f_xnonce1);
    uint32_t next_seq = deliver_job(&ctx, "NEXTJOB", f_xnonce1);
    CHECK(g_dead_seq == 0, "clean=0 retires nothing (dead=%u)", g_dead_seq);
    deliver_nonce(old_seq);
    CHECK(g_sent_n == 1 && g_nst.retired == 0,
            "a nonce for the previous non-clean job still submits (sent=%d retired=%u)", g_sent_n,
            g_nst.retired);
    CHECK(next_seq == old_seq + 1, "the non-clean notify still advanced the seq (%u)", next_seq);

    reset_state(&pipe);
    old_seq = deliver_job(&ctx, f_job_id, f_xnonce1);
    {
        static char other_prevhash[] =
                "00000000000000000001aaaabbbbccccddddeeeeffff00001111222233334444";
        uint32_t   lw = g_local_work;
        sv1_client c;
        fill_client(&c, "NEWPREV", f_xnonce1);
        c.job.prevhash = other_prevhash;
        sv1_event ev   = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
        on_sv1_event(&ctx, &c, &ev);
        CHECK(g_dead_seq == old_seq && g_local_work == lw + 1,
                "a non-clean notify on a new prevhash is clean: it retires older "
                "jobs and counts Local Work (dead=%u, local work +%u)",
                g_dead_seq, g_local_work - lw);
        uint32_t dead = g_dead_seq;
        lw            = g_local_work;
        fill_client(&c, "SAMEPREV", f_xnonce1);
        c.job.prevhash = other_prevhash;
        on_sv1_event(&ctx, &c, &ev);
        CHECK(g_dead_seq == dead && g_local_work == lw,
                "...and one on the same prevhash without clean_jobs is not "
                "(dead=%u, local work +%u)",
                g_dead_seq, g_local_work - lw);
        fill_client(&c, f_job_id, f_xnonce1);
        on_sv1_event(&ctx, &c, &ev);
    }

    reset_state(&pipe);
    old_seq = deliver_job(&ctx, f_job_id, f_xnonce1);
    {
        sv1_client c;
        fill_client(&c, "BADCLEAN", f_xnonce1);
        c.xnonce2_size   = 2;
        c.job.clean_jobs = 1;
        sv1_event ev     = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
        on_sv1_event(&ctx, &c, &ev);
    }
    CHECK(g_job_seq == old_seq && g_dead_seq == 0,
            "a clean notify the ASICs cannot take retires nothing yet "
            "(seq=%u dead=%u)",
            g_job_seq, g_dead_seq);
    next_seq = deliver_job(&ctx, "AFTERBAD", f_xnonce1);
    CHECK(next_seq == old_seq + 1 && g_dead_seq == old_seq,
            "...the next notify sent carries its clean (seq=%u dead=%u)", next_seq, g_dead_seq);
    deliver_nonce(old_seq);
    CHECK(g_sent_n == 0 && g_nst.retired == 1,
            "...so a nonce for the job before it is retired, not submitted "
            "(sent=%d retired=%u)",
            g_sent_n, g_nst.retired);
    uint32_t third_seq = deliver_job(&ctx, "THIRDJOB", f_xnonce1);
    CHECK(third_seq == next_seq + 1 && g_dead_seq == old_seq,
            "...and the carried clean is applied once (dead=%u)", g_dead_seq);

    reset_state(&pipe);
    (void)deliver_job(&ctx, f_job_id, f_xnonce1);
    {
        sv1_client c;
        fill_client(&c, "BADCLEAN", f_xnonce1);
        c.xnonce2_size   = 2;
        c.job.clean_jobs = 1;
        sv1_event ev     = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
        on_sv1_event(&ctx, &c, &ev);
    }
    session_drop();
    g_stratum.t = &g_mock;
    g_log_n     = 0;
    (void)deliver_job(&ctx, "NEWSESSION", f_xnonce1);
    CHECK(log_find("new block") == NULL,
            "a refused clean notify does not carry into the next session (%s)",
            log_find("new block") ? log_find("new block") : "-");

    g_block.hash[0] = '\0';
    g_log_n         = 0;
    (void)deliver_clean_job(&ctx, "BLOCKA", f_xnonce1);
    CHECK(log_find("new block") != NULL, "a clean job on a new prevhash logs a new block");
    g_log_n = 0;
    (void)deliver_clean_job(&ctx, "BLOCKA2", f_xnonce1);
    CHECK(log_find("new block") == NULL,
            "a second clean job on the same prevhash logs no new block (%s)",
            log_find("new block") ? log_find("new block") : "-");

    reset_state(&pipe);
    uint32_t inflight = deliver_job(&ctx, f_job_id, f_xnonce1);
    session_drop();
    g_stratum.t = &g_mock;
    for (int i = 0; i < JOB_RING; i++)
        deliver_job(&ctx, "NEWJOBID", "deadbeef00");
    CHECK(g_ring[inflight % JOB_RING].seq != inflight,
            "the new session recycled the in-flight job's slot (slot=%u holds %u)",
            (unsigned)(inflight % JOB_RING), g_ring[inflight % JOB_RING].seq);
    CHECK(inflight <= g_dead_seq, "the watermark still knows that job was retired (seq=%u dead=%u)",
            inflight, g_dead_seq);

    deliver_nonce(inflight);
    CHECK(g_sent_n == 0, "a recycled-slot in-flight nonce is still not submitted%s%s",
            g_sent_n ? " — sent: " : "", g_sent_n ? g_sent[0] : "");
    CHECK(g_nst.retired == 0 && g_nst.hw_nomatch == 1 && g_nst.hw_errors == 1,
            "...but it is charged nomatch + HW, not retired "
            "(retired=%u nomatch=%u hw=%u)",
            g_nst.retired, g_nst.hw_nomatch, g_nst.hw_errors);
    CHECK(g_nst.hw_nosession == 0,
            "...and never as nosession, even though the session did die (%u)", g_nst.hw_nosession);
    CHECK(g_pool_st[0].diff1 == 0 && g_mw[0][0] == 0,
            "...and it inflates neither Diff1 Shares nor MW (%.0f / %llu)", g_pool_st[0].diff1,
            (unsigned long long)g_mw[0][0]);

    reset_state(&pipe);
    uint32_t lagged = deliver_job(&ctx, f_job_id, f_xnonce1);
    for (int i = 0; i < JOB_RING; i++)
        deliver_job(&ctx, "LAGJOB", f_xnonce1);
    CHECK(g_dead_seq == 0 && lagged > g_dead_seq,
            "no session died and no clean notify arrived, so the lagged job was "
            "never retired (seq=%u dead=%u)",
            lagged, g_dead_seq);
    CHECK(g_ring[lagged % JOB_RING].seq != lagged,
            "...but its ring slot has been recycled (slot=%u holds %u)",
            (unsigned)(lagged % JOB_RING), g_ring[lagged % JOB_RING].seq);
    deliver_nonce(lagged);
    CHECK(g_sent_n == 0, "a LIVE-session nonce %d notifies late is dropped%s%s", JOB_RING,
            g_sent_n ? " — sent: " : "", g_sent_n ? g_sent[0] : "");
    CHECK(g_nst.hw_nomatch == 1 && g_nst.hw_errors == 1,
            "...and charged nomatch + HW, though nothing retired it "
            "(nomatch=%u hw=%u)",
            g_nst.hw_nomatch, g_nst.hw_errors);
    CHECK(g_nst.retired == 0 && g_nst.hw_nosession == 0,
            "...and neither retired nor nosession can account for it (%u/%u)", g_nst.retired,
            g_nst.hw_nosession);

    reset_state(&pipe);
    uint32_t near = deliver_job(&ctx, f_job_id, f_xnonce1);
    for (int i = 0; i < JOB_RING - 1; i++)
        deliver_job(&ctx, "LAGJOB", f_xnonce1);
    CHECK(g_ring[near % JOB_RING].seq == near,
            "one notify sooner, the slot still holds the job (%u)", g_ring[near % JOB_RING].seq);
    deliver_nonce(near);
    CHECK(g_sent_n == 1 && g_nst.hw_errors == 0,
            "...and the same nonce submits as a share (sent=%d hw=%u) — the only "
            "difference is JOB_RING",
            g_sent_n, g_nst.hw_errors);

    reset_state(&pipe);
    (void)deliver_job(&ctx, f_job_id, f_xnonce1);
    session_drop();
    deliver_nonce(9999);
    CHECK(g_nst.hw_nosession == 1 && g_nst.hw_nomatch == 0,
            "unknown seq with no live session -> nosession, not nomatch (%u/%u)",
            g_nst.hw_nosession, g_nst.hw_nomatch);

    reset_state(&pipe);
    uint32_t rseq = deliver_job(&ctx, f_job_id, f_xnonce1);
    (void)deliver_clean_job(&ctx, "CLEANJOB", f_xnonce1);
    g_log_n = 0;
    deliver_nonce_from(rseq, 0, 26);
    CHECK(g_nst.retired == 1, "the retired path is the one under test (%u)", g_nst.retired);
    {
        const char *l = log_find("retired job");
        CHECK(l && strstr(l, "asic=0/26"), "the retired line names the reporting chip (%s)",
                l ? l : "-");
    }

    reset_state(&pipe);
    uint32_t lagd = deliver_job(&ctx, f_job_id, f_xnonce1);
    for (int i = 0; i < JOB_RING; i++)
        deliver_job(&ctx, "LAGJOB", f_xnonce1);
    g_log_n = 0;
    deliver_nonce_from(lagd, 0, 171);
    CHECK(g_nst.hw_nomatch == 1, "the no-match path is the one under test (%u)", g_nst.hw_nomatch);
    {
        const char *l = log_find("no-match/stale");
        CHECK(l && strstr(l, "asic=0/171"), "the no-match/stale line names the reporting chip (%s)",
                l ? l : "-");
    }

    reset_state(&pipe);
    uint32_t curseq     = deliver_job(&ctx, f_job_id, f_xnonce1);
    g_log_n             = 0;
    uint32_t bad_before = g_pool_st[0].bad;
    deliver_nonce_from(curseq + 3u * 198u, 0, 26);
    CHECK(g_nst.hw_impossible == 1 && g_nst.hw_nomatch == 0,
            "a seq above every seq ever stamped is its own bucket, not a nomatch "
            "(impossible=%u nomatch=%u)",
            g_nst.hw_impossible, g_nst.hw_nomatch);
    CHECK(g_nst.hw_errors == 1 && g_nst.hw_board[0] == 1,
            "...charged to HW[] and its board's MH[] exactly as before "
            "(hw=%u mh=%u)",
            g_nst.hw_errors, g_nst.hw_board[0]);
    CHECK(bad_before == 1 && g_pool_st[0].bad == bad_before,
            "...and moves no pool Bad Work: that is the notify's counter "
            "(bad=%u->%u)",
            bad_before, g_pool_st[0].bad);
    CHECK(g_sent_n == 0 && g_pool_st[0].diff1 == 0 && g_mw[0][0] == 0,
            "...and submitted nowhere (sent=%d diff1=%.0f mw=%llu)", g_sent_n, g_pool_st[0].diff1,
            (unsigned long long)g_mw[0][0]);
    {
        const char *l = log_find("no-match/impossible");
        CHECK(l && strstr(l, "asic=0/26"), "...under its own label, naming the chip (%s)",
                l ? l : "-");
    }
    reset_state(&pipe);
    curseq = deliver_job(&ctx, f_job_id, f_xnonce1);
    for (int i = 0; i < JOB_RING; i++)
        deliver_job(&ctx, "LAGJOB", f_xnonce1);
    deliver_nonce_from(curseq, 0, 26);
    CHECK(g_nst.hw_nomatch == 1 && g_nst.hw_impossible == 0,
            "a seq the pool DID issue stays a nomatch (nomatch=%u impossible=%u)", g_nst.hw_nomatch,
            g_nst.hw_impossible);
    deliver_nonce_from(g_job_seq + 1u, 0, 26);
    CHECK(g_nst.hw_impossible == 1, "...and one seq past the latest is already impossible (%u)",
            g_nst.hw_impossible);

    g_mono_off_ms = -(int64_t)(NONCE_STATS_PERIOD_S + 1) * 1000;
    nonce_ledger_tick();
    CHECK(log_find("nonce stats") == NULL,
            "the first stats tick only arms the period, it prints nothing");
    g_mono_off_ms = 0;
    g_log_n       = 0;
    nonce_ledger_tick();
    {
        char want[128];
        snprintf(want, sizeof want, "%u hw, ",
                g_nst.hw_verify + g_nst.hw_nosession + g_nst.hw_nomatch + g_nst.hw_impossible);
        const char *l = log_find("nonce stats");
        CHECK(l && strstr(l, want), "the stats line carries \"%s\" (%s)", want, l ? l : "-");
        CHECK(g_nst.hw_verify + g_nst.hw_nosession + g_nst.hw_nomatch + g_nst.hw_impossible ==
                        g_nst.hw_errors,
                "the four causes still sum to HW[] (%u+%u+%u+%u vs %u)", g_nst.hw_verify,
                g_nst.hw_nosession, g_nst.hw_nomatch, g_nst.hw_impossible, g_nst.hw_errors);
    }

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_xnonce1);
    CHECK(g_ring[seq % JOB_RING].pool == 0,
            "the notify stamps the issuing pool onto the job template (%d)",
            g_ring[seq % JOB_RING].pool);
    publish_active_pool(1);
    deliver_nonce(seq);
    CHECK(g_sent_n == 1, "the in-flight nonce is still submitted (%d)", g_sent_n);
    CHECK(g_pool_st[0].diff1 > 0 && g_pool_st[1].diff1 == 0,
            "Diff1 Shares credits the job's pool, not the newly active one "
            "(p0=%.0f p1=%.0f)",
            g_pool_st[0].diff1, g_pool_st[1].diff1);
    CHECK(g_pool_st[0].best > 0 && g_pool_st[1].best == 0,
            "...and so does Best Share (p0=%.0f p1=%.0f)", g_pool_st[0].best, g_pool_st[1].best);

    uint32_t p1seq = deliver_job(&ctx, "POOL1JOB", f_xnonce1);
    CHECK(g_ring[p1seq % JOB_RING].pool == 1, "the job issued under pool 1 is stamped pool 1 (%d)",
            g_ring[p1seq % JOB_RING].pool);
    publish_active_pool(0);
    deliver_nonce(p1seq);
    CHECK(g_pool_st[1].diff1 > 0, "...and its share credits pool 1 (%.0f)", g_pool_st[1].diff1);

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_xnonce1);
    publish_active_pool(1);
    g_stratum.t = &g_mock_fail;
    deliver_nonce(seq);
    CHECK(g_pool_st[0].rfail == 1 && g_pool_st[1].rfail == 0,
            "a failed submit send is a Remote Failure for the job's pool "
            "(p0=%u p1=%u)",
            g_pool_st[0].rfail, g_pool_st[1].rfail);
    publish_active_pool(0);
    {
        reset_state(&pipe);
        CHECK(g_dev_start == 0 && g_last_valid_work == 0,
                "before any nonce there is no device clock and no valid-work stamp");
        uint32_t s2 = deliver_job(&ctx, f_job_id, f_xnonce1);
        CHECK(g_dev_start == 0,
                "a notify alone does not start it — the device has produced nothing");
        deliver_nonce_from(s2, 0, 0);
        CHECK(g_dev_start != 0, "the first diff-1 nonce starts the device clock");
        CHECK(g_last_valid_work != 0, "...and stamps Last Valid Work");
        g_dev_start -= 300;
        time_t   t0   = g_dev_start;
        uint32_t seq2 = deliver_job(&ctx, "job-two", f_xnonce1);
        deliver_nonce_from(seq2, 0, 0);
        CHECK(g_dev_start == t0, "a later nonce does not restart the device clock");
        CHECK(g_board_diff1[0] > 0 && g_board_diff1[0] == g_pool_st[0].diff1,
                "the per-board credit sums to the pool's (%.0f vs %.0f)", g_board_diff1[0],
                g_pool_st[0].diff1);
        if (HBOARD_COUNT > 1) {
            double   b0   = g_board_diff1[0];
            uint32_t seq3 = deliver_job(&ctx, "job-three", f_xnonce1);
            deliver_nonce_from(seq3, HBOARD_COUNT - 1, 0);
            CHECK(g_board_diff1[0] == b0 && g_board_diff1[HBOARD_COUNT - 1] > 0,
                    "a nonce from another board credits that board, not board 0");
            double tot = 0;
            for (int b = 0; b < HBOARD_COUNT; b++)
                tot += g_board_diff1[b];
            CHECK(tot == g_pool_st[0].diff1,
                    "and the per-board total still equals the pool's (%.0f vs %.0f)", tot,
                    g_pool_st[0].diff1);
        }
    }

    {
        reset_state(&pipe);
        uint32_t s = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce_from(s, HBOARD_COUNT, 0);
        double bsum = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            bsum += g_board_diff1[b];
        CHECK(g_sent_n == 1 && g_diff1 > 0 && g_pool_st[0].diff1 == g_diff1,
                "a valid nonce from an out-of-range board is still submitted and "
                "credited to the device and pool (sent=%d diff1=%.0f)",
                g_sent_n, g_diff1);
        CHECK(bsum == 0, "...but to no board's diff1, board 0 included (%.0f)", bsum);
        CHECK(g_nst.hw_errors == 0, "...and is not counted as HW (hw=%u)", g_nst.hw_errors);
    }

    {
        reset_state(&pipe);
        pthread_mutex_lock(&g_stats_lock);
        g_diff_acc = g_diff_rej = g_diff1 = 0;
        g_hashwin.live                    = 0;
        memset(g_hashwin.slot, 0, sizeof g_hashwin.slot);
        g_start = now_s() - 100;
        pthread_mutex_unlock(&g_stats_lock);

        uint32_t s4 = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(s4);
        double dd = g_pool_st[0].diff1;
        CHECK(dd > 0 && g_diff_acc == 0,
                "the nonce is credited though no pool ever confirmed it "
                "(diff1=%.0f accepted-diff=%.0f)",
                dd, g_diff_acc);
        CHECK(g_diff1 == dd, "...and the device-wide diff-1 sum carries it too (%.0f)", g_diff1);

        double hi = dd * 4294.967296 / 100.0, lo = dd * 4294.967296 / 102.0;
        win_age_one_second();
        mc_api_stats st;
        api_snapshot(&st, &pipe);
        CHECK(st.elapsed >= 100 && st.elapsed <= 102,
                "the session clock is the one the rate divides by (%u s)", st.elapsed);
        CHECK(st.diff_accepted == 0 && st.mhs_av >= lo && st.mhs_av <= hi,
                "GHSavg is the diff-1 sum over the session, not the accepted-"
                "difficulty sum (%.1f MH/s, want %.1f..%.1f, accepted=%.0f)",
                st.mhs_av, lo, hi, st.diff_accepted);
        CHECK(st.mhs_5s > 0 && st.mhs_1m > 0 && st.mhs_5m > 0,
                "the nonce also lands in the rate windows once its second is over "
                "(5s=%.1f 1m=%.1f 5m=%.1f)",
                st.mhs_5s, st.mhs_1m, st.mhs_5m);
        CHECK(g_pool_st[0].best > 0 && st.best_share == g_pool_st[0].best,
                "the device Best Share is the best any pool has seen (%.0f vs %.0f)", st.best_share,
                g_pool_st[0].best);

        g_cs_n = 0;
        core_stats_push();
        CHECK(g_cs_n == 1 && (double)g_cs.ghs_avg_mhs >= lo && (double)g_cs.ghs_avg_mhs <= hi,
                "the rate core pushes to mm is that same basis (%u MH/s, want "
                "%.1f..%.1f)",
                g_cs.ghs_avg_mhs, lo, hi);

        double   best1 = st.best_share;
        uint32_t s5    = deliver_job(&ctx, "job-basis-2", f_xnonce1);
        deliver_nonce(s5);
        api_snapshot(&st, &pipe);
        CHECK(g_diff1 == 2 * dd && st.mhs_av >= 2 * lo && st.mhs_av <= 2 * hi,
                "a second diff-1 nonce doubles both the sum and GHSavg "
                "(diff1=%.0f ghs=%.1f)",
                g_diff1, st.mhs_av);
        CHECK(st.best_share == best1,
                "...and an equal-difficulty share does not raise Best Share (%.0f)", st.best_share);
    }

    {
        reset_state(&pipe);
        pthread_mutex_lock(&g_stats_lock);
        memset(g_hashwin.slot, 0, sizeof g_hashwin.slot);
        g_hashwin.live = 0;
        time_t base    = now_s();
        g_start        = base - 1800;

        win_add_locked(base - 901, 7.0);
        win_add_locked(base - 890, 1000.0);
        for (int a = 0; a < 800; a++)
            for (int k = 0; k < 5; k++)
                win_add_locked(base - 800 + a, 1.0);

        double w15 = win_mhs(base, 900);
        double w5m = win_mhs(base, 300);
        double w1m = win_mhs(base, 60);
        double w5s = win_mhs(base, 5);

        win_add_locked(base, 100.0);
        double c15                           = win_mhs(base, 900);
        double c5s                           = win_mhs(base, 5);
        g_hashwin.slot[base % MHS_WIN_SLOTS] = 0;
        pthread_mutex_unlock(&g_stats_lock);

        CHECK(MHS_WIN_MAX_S >= 900, "the ring spans the longest advertised window (%d s >= 900 s)",
                MHS_WIN_MAX_S);
        CHECK(w15 == 5000.0 * 4294.967296 / 900.0,
                "MHS 15m carries all 4000 samples of the last 900 s plus the "
                "890 s-old share: eviction is by AGE, not by sample count, so the "
                "window does not shrink as the share rate rises "
                "(%.1f MH/s, want %.1f)",
                w15, 5000.0 * 4294.967296 / 900.0);
        CHECK(w5m == 1500.0 * 4294.967296 / 300.0,
                "MHS 5m is the last 300 s and nothing older (%.1f MH/s, want %.1f)", w5m,
                1500.0 * 4294.967296 / 300.0);
        CHECK(w1m == 300.0 * 4294.967296 / 60.0, "MHS 1m is the last 60 s (%.1f MH/s, want %.1f)",
                w1m, 300.0 * 4294.967296 / 60.0);
        CHECK(w5s == 25.0 * 4294.967296 / 5.0, "MHS 5s is the last 5 s (%.1f MH/s, want %.1f)", w5s,
                25.0 * 4294.967296 / 5.0);
        CHECK(w5m - w1m < 1e-6 && w1m - w5m < 1e-6 && w1m - w5s < 1e-6 && w5s - w1m < 1e-6 &&
                        w15 > w5m,
                "the steady 5 shares/s reads identically in every window it fills, "
                "and only 15m is lifted by the 890 s-old share the shorter windows "
                "correctly exclude (15m=%.1f 5m=%.1f 1m=%.1f 5s=%.1f)",
                w15, w5m, w1m, w5s);

        CHECK(c5s == w5s && c15 == w15,
                "the second in progress is in no window: each divides by whole "
                "seconds, so its credit would be counted over one second too few "
                "(5s=%.1f 15m=%.1f, want %.1f %.1f)",
                c5s, c15, w5s, w15);

        mc_api_stats st;
        api_snapshot(&st, &pipe);
        CHECK(st.mhs_15m == w15, "and that is the value the wire field carries (%.1f MH/s)",
                st.mhs_15m);

        pthread_mutex_lock(&g_stats_lock);
        memset(g_hashwin.slot, 0, sizeof g_hashwin.slot);
        g_hashwin.live = 0;
        base           = now_s();
        g_start        = base - 3;
        for (int a = 3; a >= 1; a--)
            win_add_locked(base - a, 2.0);
        win_add_locked(base, 9.0);
        double young = win_mhs(base, 5);
        pthread_mutex_unlock(&g_stats_lock);
        CHECK(young == 6.0 * 4294.967296 / 3.0,
                "3 s after start the 5 s window covers the 3 whole seconds and "
                "divides by 3 (%.1f MH/s, want %.1f)",
                young, 6.0 * 4294.967296 / 3.0);
    }

    {
        reset_state(&pipe);
        uint32_t s6 = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(s6);
        CHECK(g_last_valid_work > 1000000000u, "Last Valid Work is a unix-epoch stamp (%u)",
                g_last_valid_work);

        sv1_client rc;
        memset(&rc, 0, sizeof rc);
        sv1_event ev = { .type = SV1_EV_SUBMIT_RESULT, .submit_accepted = 1 };
        on_sv1_event(&ctx, &rc, &ev);

        uint32_t lst = g_pool_st[0].ltime;
        CHECK(lst > 1000000000u,
                "Last Share Time is a unix-epoch stamp, not the monotonic device "
                "clock now_s() (%u)",
                lst);
        CHECK(lst + 5 >= g_last_valid_work && lst <= g_last_valid_work + 5,
                "Last Share Time shares the epoch base of Last Valid Work "
                "(lst=%u lvw=%u)",
                lst, g_last_valid_work);
    }

    {
        char want[65];
        mc_block_hash_hex(f_prevhash, want);
        reset_state(&pipe);
        memset(g_block.hash, 0, sizeof g_block.hash);
        g_block.net_diff = g_block.time = 0.0;
        deliver_job(&ctx, f_job_id, f_xnonce1);
        CHECK(want[0] && !strcmp(g_block.hash, want),
                "the notify's prevhash becomes the coin block hash (%s)", g_block.hash);
        CHECK(g_block.net_diff > 0.0 && g_block.net_diff == mc_net_diff_from_nbits(f_nbits),
                "...and its nbits the network difficulty (%.8f)", g_block.net_diff);
        double t0 = g_block.time;
        CHECK(t0 > 0.0, "...stamped when that block first arrived (%.6f)", t0);
        deliver_job(&ctx, "job-two", f_xnonce1);
        CHECK(g_block.time == t0 && !strcmp(g_block.hash, want),
                "a further notify on the same block does not restamp the time");
    }

    {
        reset_state(&pipe);
        memset(g_block.hash, 0, sizeof g_block.hash);
        g_block.net_diff = g_block.time = 0.0;
        g_block.net_blocks              = 0;

        deliver_job(&ctx, f_job_id, f_xnonce1);
        CHECK(g_block.net_blocks == 1, "the first block seen counts as one (%u)",
                g_block.net_blocks);

        deliver_job(&ctx, "job-two", f_xnonce1);
        CHECK(g_block.net_blocks == 1, "a further notify on the same block does not recount (%u)",
                g_block.net_blocks);

        char other[sizeof f_prevhash];
        snprintf(other, sizeof other, "%s", f_prevhash);
        other[0] = (other[0] == '0') ? '1' : '0';
        {
            sv1_client c;
            fill_client(&c, "job-three", f_xnonce1);
            c.job.prevhash = other;
            sv1_event ev   = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
            on_sv1_event(&ctx, &c, &ev);
        }
        CHECK(g_block.net_blocks == 2, "a new block-hash change bumps the count (%u)",
                g_block.net_blocks);
    }

    {
        reset_state(&pipe);
        pthread_mutex_lock(&g_stats_lock);
        g_block.time = now_mono_s() - 300.0;
        pthread_mutex_unlock(&g_stats_lock);

        mc_api_stats st;
        api_snapshot(&st, &pipe);
        CHECK(st.block_time >= 295.0 && st.block_time <= 305.0,
                "coin block time is the age since the block, not the raw monotonic "
                "stamp (%.3f s)",
                st.block_time);

        pthread_mutex_lock(&g_stats_lock);
        g_block.time = 0.0;
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&st, &pipe);
        CHECK(st.block_time == 0.0,
                "...and with no block yet the age is zero, not a negative uptime "
                "(%.3f s)",
                st.block_time);
    }

    {
        reset_state(&pipe);
        uint32_t ls = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(ls);
        CHECK(g_sent_n == 1, "the share reached the wire (%d)", g_sent_n);
        CHECK(g_send_lock_free == 1,
                "S.lock is FREE while the submit is written, so a 30 s send cannot "
                "stall the work pump (%d of %d writes)",
                g_send_lock_free, g_sent_n);
        CHECK(g_send_guarded == 1,
                "...and g_stratum.send is held instead, so a concurrent teardown cannot "
                "close the transport under it (%d of %d)",
                g_send_guarded, g_sent_n);
        CHECK(pthread_mutex_trylock(&g_stratum.lock) == 0,
                "S.lock is released again once the nonce has been handled");
        pthread_mutex_unlock(&g_stratum.lock);
    }

    {
        reset_state(&pipe);
        uint32_t ds  = deliver_job(&ctx, f_job_id, f_xnonce1);
        g_drop_probe = 1;
        deliver_nonce(ds);
        pthread_join(g_drop_th, NULL);
        CHECK(g_drop_started && g_drop_pending_seen,
                "session_drop BLOCKS while a submit is in flight (started=%d "
                "pending=%d)",
                g_drop_started, g_drop_pending_seen);
        CHECK(g_drop_done, "...and completes once the write returns (%d)", g_drop_done);
        g_drop_probe = 0;
    }

    {
        reset_state(&pipe);
        pthread_mutex_lock(&g_stats_lock);
        g_diff1 = 0;
        memset(g_board_diff1, 0, sizeof g_board_diff1);
        pthread_mutex_unlock(&g_stats_lock);

        uint32_t a1 = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(a1);
        double one = g_diff1;
        CHECK(one > 0 && g_pool_st[0].diff1 == one,
                "a credited nonce moves the device total and its pool alike (%.0f)", one);

        publish_active_pool(-1);
        uint32_t a2 = deliver_job(&ctx, "NOPOOLJOB", f_xnonce1);
        CHECK(g_ring[a2 % JOB_RING].pool == -1,
                "a job issued with no active pool is stamped -1 (%d)", g_ring[a2 % JOB_RING].pool);
        deliver_nonce(a2);
        CHECK(g_diff1 == 2 * one, "the device diff-1 total is credited even so (%.0f, want %.0f)",
                g_diff1, 2 * one);
        CHECK(g_pool_st[0].diff1 == one, "...while no per-pool column is (p0=%.0f)",
                g_pool_st[0].diff1);
        double bsum = 0;
        for (int b = 0; b < HBOARD_COUNT; b++)
            bsum += g_board_diff1[b];
        CHECK(bsum == g_diff1,
                "...and the per-board split still sums to it, so MGHS[..] sums to "
                "GHSavg (%.0f vs %.0f)",
                bsum, g_diff1);

        mc_api_stats st;
        api_snapshot(&st, &pipe);
        CHECK(st.npools == 0 && st.diff1 == g_diff1,
                "api_snapshot publishes the device total (%.0f, npools=%d)", st.diff1, st.npools);
        publish_active_pool(0);
    }

    {
        reset_state(&pipe);
        sv1_clear_inflight(NULL, NULL);
        uint32_t ds = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(ds);
        double   want_d = atof(g_ring[ds % JOB_RING].diff);
        double   dd     = 0;
        uint32_t n      = sv1_clear_inflight(NULL, &dd);
        CHECK(g_sent_n == 1 && n == 1 && dd == want_d,
                "a submitted share is in flight at its job difficulty "
                "(sent=%d, %u at %.0f, want %.0f)",
                g_sent_n, n, dd, want_d);
    }

    {
        reset_state(&pipe);
        uint32_t zs = deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_nonce(zs);
        pthread_mutex_lock(&g_stats_lock);
        g_nst.low_diff                       = 5;
        g_nst.retired                        = 3;
        g_nst.aging                          = 2;
        g_nst.hit_repaired                   = 1;
        g_nst.hw_verify                      = 4;
        g_nst.hw_nosession                   = 1;
        g_nst.hw_nomatch                     = 9;
        g_nst.hw_impossible                  = 2;
        g_nst.hw_errors                      = 16;
        g_nst.hw_board[0]                    = 15;
        g_pool_st[0].ltime                   = 1787128137;
        g_pool_st[0].lsdiff                  = 8192;
        g_pool_st[0].wdiff                   = 8192;
        g_pool_st[0].stale                   = 3;
        g_pool_st[0].dstale                  = 24576;
        g_pool_st[0].works                   = 7;
        g_pool_st[POOL_VALID_NUM - 1].dstale = 8192;
        pthread_mutex_unlock(&g_stats_lock);
        mc_pipeline_on_result(&pipe, 1);
        mc_pipeline_on_result(&pipe, 0);
        pipe.dups      = 2;
        pipe.sv1.stale = 4;
        uint32_t zseq  = g_job_seq;
        int      znext = pipe.sv1.next_id;
        CHECK(g_nst.rx > 0 && g_nst.hit_direct > 0 && g_mw[0][0] > 0,
                "the ledger is populated before the zero (rx=%u hit=%u mw=%llu)", g_nst.rx,
                g_nst.hit_direct, (unsigned long long)g_mw[0][0]);

        mc_api_stats zst;
        char         zb[1024];
        api_snapshot(&zst, &pipe);
        CHECK(zst.diff_stale == 24576 + 8192,
                "summary Difficulty Stale is the sum over every pool slot "
                "(%.0f)",
                zst.diff_stale);
        zero_handler("zero|all,false", &zst, 0, zb, sizeof zb, NULL);

        CHECK(g_nst.hw_verify == 0 && g_nst.hw_nosession == 0 && g_nst.hw_nomatch == 0 &&
                        g_nst.hw_impossible == 0,
                "zero|all clears the HW triage buckets (%u/%u/%u/%u)", g_nst.hw_verify,
                g_nst.hw_nosession, g_nst.hw_nomatch, g_nst.hw_impossible);
        CHECK(g_nst.rx == 0 && g_nst.low_diff == 0 && g_nst.retired == 0 && g_nst.aging == 0 &&
                        g_nst.hit_direct == 0 && g_nst.hit_repaired == 0,
                "...and the rest of the ledger (rx=%u lowdiff=%u retired=%u "
                "aging=%u hit=%u/%u)",
                g_nst.rx, g_nst.low_diff, g_nst.retired, g_nst.aging, g_nst.hit_direct,
                g_nst.hit_repaired);
        CHECK(g_mw[0][0] == 0, "...and the per-chip MW credit (%llu)",
                (unsigned long long)g_mw[0][0]);
        CHECK(strstr(zb, "Zeroed all stats") != NULL, "...and answers Zeroed all stats (%s)", zb);
        api_snapshot(&zst, &pipe);
        CHECK(zst.accepted == 0 && zst.rejected == 0 && zst.dhw == 0 && zst.stale == 0,
                "zero|all clears Accepted/Rejected/DHW/Stale (%u/%u/%u/%u)", zst.accepted,
                zst.rejected, zst.dhw, zst.stale);
        CHECK(zst.diff_accepted == 0 && zst.diff_rejected == 0 && zst.diff_stale == 0 &&
                        zst.diff1 == 0 && zst.best_share == 0 && zst.get_failures == 0 &&
                        zst.remote_failures == 0 && zst.hw_errors == 0,
                "...and the Difficulty*/Best Share/Get Failures/HW totals");
        CHECK(g_pool_st[0].acc == 0 && g_pool_st[0].rej == 0 && g_pool_st[0].ltime == 0 &&
                        g_pool_st[0].lsdiff == 0 && g_pool_st[0].diff1 == 0 &&
                        g_pool_st[0].best == 0,
                "...and the per-pool share counters and last-share fields "
                "(ltime=%u lsdiff=%.0f)",
                g_pool_st[0].ltime, g_pool_st[0].lsdiff);
        CHECK(g_pool_st[0].stale == 0 && g_pool_st[0].dstale == 0,
                "...and the per-pool Stale and Difficulty Stale (%u, %.0f)", g_pool_st[0].stale,
                g_pool_st[0].dstale);
        CHECK(g_pool_st[0].works == 7, "...while the per-pool Works survives (%u)",
                g_pool_st[0].works);
        CHECK(g_job_seq == zseq && pipe.sv1.next_id == znext && g_stratum.pipe == &pipe &&
                        g_stratum.t == &g_mock && g_pool_st[0].wdiff == 8192,
                "...while the job ring, submit id sequence, session and pool "
                "difficulty are left alone");

        deliver_nonce(g_job_seq + 1000);
        api_snapshot(&zst, &pipe);
        uint32_t lhw =
                g_nst.hw_verify + g_nst.hw_nosession + g_nst.hw_nomatch + g_nst.hw_impossible;
        CHECK(lhw == 1 && zst.hw_errors == 1,
                "after a zero the ledger's HW= and the wire's HW[] count the same "
                "one error (ledger=%u wire=%u)",
                lhw, zst.hw_errors);
    }

    {
        reset_state(&pipe);
        CHECK(g_pool_st[0].getworks == 0, "Getworks starts at 0 (%u)", g_pool_st[0].getworks);
        deliver_job(&ctx, f_job_id, f_xnonce1);
        deliver_job(&ctx, "2c7d1b", f_xnonce1);
        deliver_clean_job(&ctx, "2c7d1c", f_xnonce1);
        CHECK(g_pool_st[0].getworks == 3,
                "each mining.notify counts one Getwork, connects do not (%u)",
                g_pool_st[0].getworks);

        reset_state(&pipe);
        pthread_mutex_lock(&g_stats_lock);
        g_pool_st[0].dead = 1;
        pthread_mutex_unlock(&g_stats_lock);
        sv1_client hc;
        memset(&hc, 0, sizeof hc);
        sv1_event hev = { .type = SV1_EV_CONFIGURED, .submit_accepted = 0 };
        on_sv1_event(&ctx, &hc, &hev);
        hev.type = SV1_EV_SUBSCRIBED;
        on_sv1_event(&ctx, &hc, &hev);
        CHECK(g_pool_st[0].dead == 1, "subscribed is not yet the handshake: pool 0 still Dead (%d)",
                g_pool_st[0].dead);
        hev.type = SV1_EV_AUTHORIZED;
        on_sv1_event(&ctx, &hc, &hev);
        CHECK(g_pool_st[0].dead == 1,
                "authorised is not yet a working pool: pool 0 still Dead (%d)", g_pool_st[0].dead);
        ctx.got_job = 0;
        deliver_job(&ctx, f_job_id, f_xnonce1);
        CHECK(g_pool_st[0].dead == 0 && ctx.got_job == 1,
                "its first usable job -> pool 0 Alive (%d)", g_pool_st[0].dead);
    }

    {
        struct {
            int         established, user_drop;
            const char *what;
        } cases[] = {
            { 0, 0, "session ended before authorise" },
            { 1, 0, "session ended after authorise" },
            { 1, 1, "session the user disabled after authorise" },
            { 0, 1, "session the user disabled before authorise" },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            int      dead = !cases[i].established && !cases[i].user_drop;
            pool_set ps;
            pool_init(&ps, 1000, 60000);
            pool_add(&ps, "stratum+tcp://p0:3333", "u0", "x");
            pool_add(&ps, "stratum+tcp://p1:3333", "u1", "x");
            pthread_mutex_lock(&g_stats_lock);
            g_pool_st[0].dead    = 0;
            g_pool_st[0].getfail = 0;
            pthread_mutex_unlock(&g_stats_lock);
            int prio[POOL_VALID_NUM];
            sync_pool_policy(&ps, prio);
            CHECK(pool_pick(&ps, prio) == 0, "%s: pool 0 selected first", cases[i].what);
            pool_note_session_end(&ps, 0, cases[i].established, cases[i].user_drop);
            CHECK(ps.pools[0].alive == !dead, "%s -> pool 0 alive=%d (want %d)", cases[i].what,
                    ps.pools[0].alive, !dead);
            sync_pool_policy(&ps, prio);
            CHECK(pool_pick(&ps, prio) == (dead ? 1 : 0), "%s -> next selection is pool %d",
                    cases[i].what, ps.active);
            CHECK(g_pool_st[0].dead == dead, "%s -> API Status %s (dead=%d)", cases[i].what,
                    dead ? "Dead" : "stays Alive", g_pool_st[0].dead);
            int gf = cases[i].established && !cases[i].user_drop;
            CHECK(g_pool_st[0].getfail == (uint32_t)gf, "%s -> %s Get Failure (%u)", cases[i].what,
                    gf ? "one" : "no", g_pool_st[0].getfail);
        }
    }

    CHECK(meminfo_parse_available_kb("MemTotal:         123456 kB\n"
                                     "MemFree:           11111 kB\n"
                                     "MemAvailable:      39004 kB\n"
                                     "Buffers:            2222 kB\n") == 39004,
            "MemAvailable is read, not MemFree");
    CHECK(meminfo_parse_available_kb("MemAvailable:      39004 kB\n") == 39004,
            "...also as the first line");
    CHECK(meminfo_parse_available_kb("MemTotal: 1 kB\nMemFree: 2 kB\n") == 0,
            "no MemAvailable line -> 0 (unknown), not a guess");
    CHECK(meminfo_parse_available_kb("MemAvailable:  \n") == 0,
            "malformed value -> 0, not garbage");
    CHECK(meminfo_parse_available_kb("") == 0, "empty /proc/meminfo -> 0");
    CHECK(meminfo_parse_available_kb("NotMemAvailable: 5 kB\n") == 0,
            "a line that merely contains the key is not matched");

    {
        atomic_store(&g_main_beat_ms, 0);
        g_cs_n = 0;
        core_stats_push();
        CHECK(g_cs_n == 1 && g_cs.core_health == CORE_HEALTH_OK,
                "no thread has ticked yet -> CORE_HEALTH_OK, not a false stall (%u)",
                g_cs.core_health);

        uint64_t now = now_ms();
        atomic_store(&g_main_beat_ms, now - CORE_MAIN_STALL_MS - 1000);
        g_cs_n = 0;
        core_stats_push();
        CHECK(g_cs_n == 1 && g_cs.core_health == CORE_UNHEALTHY_MAIN_STALL,
                "main stratum loop stalled past %ums sets only its own bit (%u)",
                CORE_MAIN_STALL_MS, g_cs.core_health);

        atomic_store(&g_main_beat_ms, now);
        g_cs_n = 0;
        core_stats_push();
        CHECK(g_cs_n == 1 && g_cs.core_health == CORE_HEALTH_OK,
                "a fresh beat clears the bit again (%u)", g_cs.core_health);

        for (int ch = 0; ch < MC_API_CHAN_N; ch++) {
            atomic_store(&g_mc_api_busy_ms[ch], now - CORE_API_STALL_MS + 1000);
            g_cs_n = 0;
            core_stats_push();
            CHECK(g_cs_n == 1 && g_cs.core_health == CORE_HEALTH_OK,
                    "an API request in progress on channel %d within %ums is no stall (%u)", ch,
                    CORE_API_STALL_MS, g_cs.core_health);

            atomic_store(&g_mc_api_busy_ms[ch], now - CORE_API_STALL_MS - 1000);
            g_cs_n = 0;
            core_stats_push();
            CHECK(g_cs_n == 1 && g_cs.core_health == CORE_UNHEALTHY_API_STALL,
                    "an API request in progress on channel %d past %ums sets only its own bit (%u)",
                    ch, CORE_API_STALL_MS, g_cs.core_health);

            atomic_store(&g_mc_api_busy_ms[!ch], now);
            atomic_store(&g_mc_api_busy_ms[!ch], 0);
            g_cs_n = 0;
            core_stats_push();
            CHECK(g_cs_n == 1 && g_cs.core_health == CORE_UNHEALTHY_API_STALL,
                    "...and a request finished on channel %d meanwhile does not hide it (%u)", !ch,
                    g_cs.core_health);

            atomic_store(&g_mc_api_busy_ms[ch], 0);
            g_cs_n = 0;
            core_stats_push();
            CHECK(g_cs_n == 1 && g_cs.core_health == CORE_HEALTH_OK,
                    "an idle API server waiting for a connection is no stall (%u)",
                    g_cs.core_health);
        }
    }

    {
        uint64_t beat;
        pool_set empty_ps;
        pool_init(&empty_ps, 1000, 60000);
        atomic_store(&g_main_beat_ms, 0);
        pool_session_once(&empty_ps, &pipe);
        beat = atomic_load(&g_main_beat_ms);
        CHECK(beat != 0 && now_ms() - beat < 1000,
                "pool_session_once marks g_main_beat_ms before pool_pick, even "
                "with no pool configured");
    }

    {
        pool_set ps;
        pool_init(&ps, 1000, 60000);
        pool_add(&ps, "stratum+tcp://snap:3333", "snapuser", "x");
        publish_pools(&ps);

        mc_api_stats st;
        api_snapshot(&st, NULL);
        pthread_mutex_lock(&g_pools_lock);
        snprintf(g_pool_pub[0].url, sizeof g_pool_pub[0].url, "%s", "stratum+tcp://moved:4444");
        snprintf(g_pool_pub[0].user, sizeof g_pool_pub[0].user, "%s", "moveduser");
        pthread_mutex_unlock(&g_pools_lock);
        CHECK(st.npools == 1 && st.pool[0].url != g_pool_pub[0].url &&
                        st.pool[0].user != g_pool_pub[0].user &&
                        !strcmp(st.pool[0].url, "stratum+tcp://snap:3333") &&
                        !strcmp(st.pool[0].user, "snapuser"),
                "the pools snapshot keeps its own url and user copy when the running list "
                "changes (%s %s)",
                st.pool[0].url ? st.pool[0].url : "", st.pool[0].user ? st.pool[0].user : "");
        char *buf = malloc(MC_API_REPLY_CAP);
        CHECK(buf != NULL, "reply buffer");
        if (buf) {
            mc_api_pools(&st, buf, MC_API_REPLY_CAP);
            CHECK(strstr(buf, "stratum+tcp://snap:3333") && strstr(buf, "snapuser") &&
                            !strstr(buf, "moved"),
                    "...and the pools reply formats the copy (%.300s)", buf);
            free(buf);
        }
    }

    {
        pthread_t th;
        int       torn = 0, seen[2] = { 0, 0 };
        pool_init(&g_pub_set[0], 1000, 60000);
        pool_init(&g_pub_set[1], 1000, 60000);
        for (int i = 0; i < 3; i++) {
            char url[64], user[16];
            snprintf(url, sizeof url, "stratum+tcp://a%d.example:3333", i);
            snprintf(user, sizeof user, "ua%d", i);
            pool_add(&g_pub_set[0], url, user, "x");
            if (i == 2)
                break;
            snprintf(url, sizeof url, "stratum+tcp://b%d.example:4444", i);
            snprintf(user, sizeof user, "ub%d", i);
            pool_add(&g_pub_set[1], url, user, "x");
        }
        publish_pools(&g_pub_set[1]);
        atomic_store(&g_pub_stop, 0);
        CHECK(pthread_create(&th, NULL, pub_thread, NULL) == 0, "pool publisher thread starts");
        for (int k = 0; k < 20000 && !(k > 2000 && seen[0] && seen[1]); k++) {
            mc_api_stats st;
            api_snapshot(&st, NULL);
            int set = st.npools == 3 ? 0 : 1;
            torn += st.npools != 3 && st.npools != 2;
            for (int i = 0; i < st.npools && i < 3; i++) {
                char url[64], user[16];
                snprintf(url, sizeof url, "stratum+tcp://%c%d.example:%s", set ? 'b' : 'a', i,
                        set ? "4444" : "3333");
                snprintf(user, sizeof user, "u%c%d", set ? 'b' : 'a', i);
                torn += strcmp(st.pool[i].url, url) || strcmp(st.pool[i].user, user);
            }
            seen[set] = 1;
        }
        atomic_store(&g_pub_stop, 1);
        pthread_join(th, NULL);
        CHECK(atomic_load(&g_pub_n) > 0 && seen[0] && seen[1],
                "snapshots run while the pool list is republished (%d publishes, both lists "
                "seen: %d %d)",
                atomic_load(&g_pub_n), seen[0], seen[1]);
        CHECK(torn == 0, "...and every snapshot holds one whole list, never a mix (%d torn)", torn);
    }

    {
        static const char *const urls[]   = { "stratum+tcp://a:3333", "stratum+tcp://b:3333",
              "stratum+tcp://c:3333" };
        static const char *const users[]  = { "ua", "ub", "uc" };
        static const char *const passes[] = { "x", "x", "x" };
        pool_set                 boot;
        pool_init(&boot, 1000, 60000);
        for (int i = 0; i < 3; i++)
            pool_add(&boot, urls[i], users[i], passes[i]);
        publish_pools(&boot);
        publish_active_pool(2);
        g_conf = boot;

        mc_api_stats st;
        memset(&st, 0, sizeof st);
        char *buf = malloc(MC_API_REPLY_CAP);
        CHECK(buf != NULL, "reply buffer");
        if (buf) {
            for (int i = 0; i < POOL_VALID_NUM; i++)
                g_pool_st[i].dead = 0;
            g_pool_enabled[1]       = 0;
            g_conf.pools[1].enabled = 0;
            pthread_mutex_lock(&g_stratum.lock);
            g_stratum.t         = &g_mock;
            g_stratum.user_drop = 0;
            pthread_mutex_unlock(&g_stratum.lock);
            switchpool_handler("switchpool|1", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Switching to pool 1:'stratum+tcp://b:3333'") && g_conf.count == 3 &&
                            !strcmp(g_conf.pools[0].url, urls[1]) &&
                            !strcmp(g_conf.pools[1].url, urls[0]),
                    "switchpool 1 moves b to the front of pools.conf (%s)", buf);
            CHECK(!strcmp(g_pool_pub[0].url, urls[0]),
                    "...while the running list, and its ids, stay as booted");
            CHECK(g_pool_enabled[1] == 1 && g_conf.pools[0].enabled,
                    "...enables the disabled pool b, running and in pools.conf (%d %d)",
                    g_pool_enabled[1], g_conf.pools[0].enabled);
            CHECK(g_pool_prio[0] == 1 && g_pool_prio[1] == 0 && g_pool_prio[2] == 2,
                    "...gives b the top running priority (%d %d %d)", g_pool_prio[0],
                    g_pool_prio[1], g_pool_prio[2]);
            CHECK(g_stratum.user_drop == 1,
                    "...and ends the live session on pool 2 so b is selected (%d)",
                    g_stratum.user_drop);

            g_stratum.user_drop = 0;
            disablepool_handler("disablepool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Msg=Disabling pool 2:'stratum+tcp://c:3333',") &&
                            g_stratum.user_drop == 1,
                    "disablepool 2 replies with its URL and ends the live session, "
                    "which is on pool 2 (%d, %s)",
                    g_stratum.user_drop, buf);
            enablepool_handler("enablepool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Msg=Enabling pool 2:'stratum+tcp://c:3333',"),
                    "enablepool 2 replies with its URL (%s)", buf);

            publish_active_pool(1);
            g_stratum.user_drop = 0;
            disablepool_handler("disablepool|0", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(g_pool_enabled[0] == 0 && g_pool_enabled[1] == 1,
                    "disablepool 0 disables the running pool 0, a (%d %d)", g_pool_enabled[0],
                    g_pool_enabled[1]);
            CHECK(g_stratum.user_drop == 0, "...and leaves the live session on pool 1 running (%d)",
                    g_stratum.user_drop);
            CHECK(!strcmp(g_conf.pools[1].url, urls[0]) && !g_conf.pools[1].enabled &&
                            g_conf.pools[0].enabled,
                    "...and persists it on a, now second in pools.conf, not on b "
                    "(conf: %s=%d %s=%d)",
                    g_conf.pools[0].url, g_conf.pools[0].enabled, g_conf.pools[1].url,
                    g_conf.pools[1].enabled);

            g_pool_enabled[1] = 0;
            publish_active_pool(2);
            enablepool_handler("enablepool|0", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(g_pool_enabled[0] == 1 && g_stratum.user_drop == 1,
                    "enablepool 0, above the live pool 2 in priority, ends its session (%d)",
                    g_stratum.user_drop);
            g_stratum.user_drop = 0;
            g_pool_enabled[1]   = 1;
            publish_active_pool(1);
            disablepool_handler("disablepool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            enablepool_handler("enablepool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(g_pool_enabled[2] == 1 && g_stratum.user_drop == 0,
                    "enablepool 2, below the live pool 1, leaves its session running (%d)",
                    g_stratum.user_drop);
            pthread_mutex_lock(&g_stratum.lock);
            g_stratum.t = NULL;
            pthread_mutex_unlock(&g_stratum.lock);
            publish_active_pool(2);

            removepool_handler("removepool|1", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Removed pool 1:'stratum+tcp://b:3333'") && g_conf.count == 2 &&
                            !strcmp(g_conf.pools[0].url, urls[0]) &&
                            !strcmp(g_conf.pools[1].url, urls[2]),
                    "removepool 1 removes b, the pool the pools reply lists as 1, "
                    "not a (%s)",
                    buf);

            disablepool_handler("disablepool|1", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=E") && g_pool_enabled[1] == 1,
                    "a pool no longer in pools.conf is refused, and left running as "
                    "it was (%s)",
                    buf);

            removepool_handler("removepool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Cannot remove active pool 2:'stratum+tcp://c:3333'") &&
                            g_conf.count == 2,
                    "removepool still refuses the active pool by its running id (%s)", buf);

            pool_set saved = g_conf;
            addpool_handler("addpool|stratum+tcp://a:3333,ua,x", &st, 0, buf, MC_API_REPLY_CAP,
                    NULL);
            CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=53,") &&
                            strstr(buf, "Msg=Pool already exists 0:'stratum+tcp://a:3333',") &&
                            g_conf.count == 2,
                    "addpool of an existing url and user -> E,53, nothing added (%s)", buf);
            addpool_handler("addpool|stratum+tcp://a:3333,other,x", &st, 0, buf, MC_API_REPLY_CAP,
                    NULL);
            CHECK(strstr(buf, "STATUS=S,") && strstr(buf, "Code=55,") &&
                            strstr(buf, "Msg=Added pool 2:'stratum+tcp://a:3333',") &&
                            g_conf.count == 3,
                    "addpool of an existing url with another user adds it (%s)", buf);
            g_conf = saved;

            g_conf.pools[1].enabled = 0;
            removepool_handler("removepool|0", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=66,") &&
                            strstr(buf, "Msg=Cannot remove last pool 0:'stratum+tcp://a:3333',") &&
                            g_conf.count == 2,
                    "removepool of the last enabled pool -> E,66, pool kept (%s)", buf);
            g_conf = saved;

            poolpriority_handler("poolpriority|0,5", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Code=26,") && strstr(buf, "Msg=Invalid pool id 5 - range is 0 - 2,"),
                    "poolpriority 0,5 -> E,26 for the id past the pool count (%s)", buf);
            poolpriority_handler("poolpriority|9,9", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Msg=Invalid pool id 9 - range is 0 - 2,"),
                    "poolpriority 9,9 -> the range error comes before the duplicate (%s)", buf);
            poolpriority_handler("poolpriority|1,1,7", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Code=74,") && strstr(buf, "Msg=Duplicate pool specified 1,"),
                    "poolpriority 1,1,7 -> E,74 at the first bad token (%s)", buf);
            CHECK(g_pool_prio[0] == 1 && g_pool_prio[1] == 0 && g_pool_prio[2] == 2,
                    "...and a rejected list changes no priority (%d %d %d)", g_pool_prio[0],
                    g_pool_prio[1], g_pool_prio[2]);

            pthread_mutex_lock(&g_stratum.lock);
            g_stratum.t         = &g_mock;
            g_stratum.user_drop = 0;
            pthread_mutex_unlock(&g_stratum.lock);
            publish_active_pool(1);
            poolpriority_handler("poolpriority|2,0", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Changed pool priorities") && !strcmp(g_conf.pools[0].url, urls[2]) &&
                            !strcmp(g_conf.pools[1].url, urls[0]),
                    "poolpriority 2,0 puts c before a in pools.conf (%s)", buf);
            CHECK(g_pool_prio[0] == 1 && g_pool_prio[1] == 2 && g_pool_prio[2] == 0,
                    "...ranks c, a, then the unlisted b at runtime (%d %d %d)", g_pool_prio[0],
                    g_pool_prio[1], g_pool_prio[2]);
            CHECK(g_stratum.user_drop == 1,
                    "...and ends the live session on b so c is selected (%d)", g_stratum.user_drop);
            pthread_mutex_lock(&g_stratum.lock);
            g_stratum.t         = NULL;
            g_stratum.user_drop = 0;
            pthread_mutex_unlock(&g_stratum.lock);
            publish_active_pool(2);

            switchpool_handler("switchpool|3", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Invalid pool id 3 - range is 0 - 2"),
                    "ids are checked against the running list (%s)", buf);

            enablepool_handler("enablepool|", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=25,") &&
                            strstr(buf, "Msg=Missing pool id parameter,"),
                    "enablepool with no id -> E,25 Missing pool id parameter (%s)", buf);
            int en0 = g_pool_enabled[0];
            disablepool_handler("disablepool|x", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=S,") && strstr(buf, "Code=48,") &&
                            strstr(buf, "Msg=Disabling pool 0:'stratum+tcp://a:3333',"),
                    "disablepool with a non-numeric id is pool 0, as atoi (%s)", buf);
            g_pool_enabled[0] = en0;

            int en[3]         = { g_pool_enabled[0], g_pool_enabled[1], g_pool_enabled[2] };
            g_pool_enabled[0] = 0;
            g_pool_enabled[1] = 0;
            g_pool_enabled[2] = 1;
            disablepool_handler("disablepool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=51,") &&
                            strstr(buf,
                                    "Msg=Cannot disable last active pool "
                                    "2:'stratum+tcp://c:3333',") &&
                            g_pool_enabled[2] == 1,
                    "disablepool on the last enabled pool -> E,51, pool left enabled (%s)", buf);
            disablepool_handler("disablepool|0", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=I,") && strstr(buf, "Code=50,"),
                    "...an already-disabled pool reports I,50 ahead of E,51 (%s)", buf);
            g_pool_enabled[0] = en[0];
            g_pool_enabled[1] = en[1];
            g_pool_enabled[2] = en[2];

            switchpool_handler("switchpool|", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=25,") &&
                            strstr(buf, "Msg=Missing pool id parameter,"),
                    "switchpool with no id -> E,25 Missing pool id parameter (%s)", buf);
            poolpriority_handler("poolpriority|", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=25,") &&
                            strstr(buf, "Msg=Missing pool id parameter,"),
                    "poolpriority with no id -> E,25 Missing pool id parameter (%s)", buf);

            int pub_n                         = g_pool_pub_n;
            g_pool_pub_n                      = 0;
            static const char *const idonly[] = {
                "enablepool|",
                "disablepool|0",
                "removepool|",
                "removepool|0",
            };
            for (int i = 0; i < (int)(sizeof idonly / sizeof idonly[0]); i++) {
                if (!strncmp(idonly[i], "enable", 6))
                    enablepool_handler(idonly[i], &st, 0, buf, MC_API_REPLY_CAP, NULL);
                else if (!strncmp(idonly[i], "disable", 7))
                    disablepool_handler(idonly[i], &st, 0, buf, MC_API_REPLY_CAP, NULL);
                else
                    removepool_handler(idonly[i], &st, 0, buf, MC_API_REPLY_CAP, NULL);
                CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=8,") &&
                                strstr(buf, "Msg=No pools,"),
                        "`%s` on an empty table -> E,8 No pools first (%s)", idonly[i], buf);
            }
            g_pool_pub_n = pub_n;
            free(buf);
        }
        publish_active_pool(0);
    }

    {
        static const char *const urls[]   = { "stratum+tcp://a:3333", "stratum+tcp://a:3333",
              "stratum+tcp://c:3333" };
        static const char *const users[]  = { "ua", "ua", "uc" };
        static const char *const passes[] = { "p0", "p1", "x" };
        pool_set                 boot;
        pool_init(&boot, 1000, 60000);
        for (int i = 0; i < 3; i++)
            pool_add(&boot, urls[i], users[i], passes[i]);
        publish_pools(&boot);
        publish_active_pool(-1);
        g_conf = boot;

        mc_api_stats st;
        memset(&st, 0, sizeof st);
        char *buf = malloc(MC_API_REPLY_CAP);
        CHECK(buf != NULL, "reply buffer");
        if (buf) {
            disablepool_handler("disablepool|1", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=S,") && g_conf.count == 3 &&
                            !strcmp(g_conf.pools[0].pass, "p0") && g_conf.pools[0].enabled &&
                            !strcmp(g_conf.pools[1].pass, "p1") && !g_conf.pools[1].enabled,
                    "disablepool 1 with pools 0 and 1 identical disables the second "
                    "entry in pools.conf (%s)",
                    buf);

            switchpool_handler("switchpool|1", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=S,") && g_pool_enabled[1] == 1 &&
                            !strcmp(g_conf.pools[0].pass, "p1") && g_conf.pools[0].enabled &&
                            !strcmp(g_conf.pools[1].pass, "p0"),
                    "switchpool 1 moves the second url+user entry, with its own "
                    "password, to the front of pools.conf and enables it (%s)",
                    buf);

            removepool_handler("removepool|0", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            disablepool_handler("disablepool|1", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "is no longer in the pool config") && g_pool_enabled[1] == 1 &&
                            g_conf.count == 2 && g_conf.pools[0].enabled,
                    "after one of two identical entries is removed, disablepool on the "
                    "other is refused and pools.conf is left as it was (%s)",
                    buf);
            free(buf);
        }
        publish_active_pool(0);
    }

    {
        static const char *const urls[]   = { "stratum+tcp://a:3333", "stratum+tcp://b:3333",
              "stratum+tcp://a:3333" };
        static const char *const users[]  = { "ua", "ub", "ua" };
        static const char *const passes[] = { "pass1", "x", "pass2" };
        pool_set                 boot;
        pool_init(&boot, 1000, 60000);
        for (int i = 0; i < 3; i++)
            pool_add(&boot, urls[i], users[i], passes[i]);
        boot.pools[2].enabled = 0;
        publish_pools(&boot);
        publish_active_pool(-1);
        g_conf = boot;

        mc_api_stats st;
        memset(&st, 0, sizeof st);
        char *buf = malloc(MC_API_REPLY_CAP);
        CHECK(buf != NULL, "reply buffer");
        if (buf) {
            switchpool_handler("switchpool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=S,") && g_conf.count == 3 &&
                            !strcmp(g_conf.pools[0].pass, "pass2") && g_conf.pools[0].enabled &&
                            !strcmp(g_conf.pools[1].pass, "pass1") && g_conf.pools[1].enabled &&
                            !strcmp(g_conf.pools[2].url, urls[1]),
                    "switchpool 2 on [a/pass1, b, a/pass2 disabled] writes pools.conf as "
                    "[a/pass2 enabled, a/pass1, b] (%s)",
                    buf);

            disablepool_handler("disablepool|2", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=S,") && !strcmp(g_conf.pools[0].pass, "pass2") &&
                            !g_conf.pools[0].enabled && g_conf.pools[1].enabled,
                    "...and pool 2 still maps to a/pass2 in the reordered pools.conf (%s)", buf);
            free(buf);
        }
        publish_active_pool(0);
    }

    {
        static const char *const urls[]   = { "stratum+tcp://a:3333", "stratum+tcp://b:3333" };
        static const char *const users[]  = { "ua", "ub" };
        static const char *const passes[] = { "x", "x" };
        pool_set                 boot;
        pool_init(&boot, 1000, 60000);
        for (int i = 0; i < 2; i++)
            pool_add(&boot, urls[i], users[i], passes[i]);
        publish_pools(&boot);
        publish_active_pool(-1);
        g_conf = boot;

        mc_api_stats st;
        memset(&st, 0, sizeof st);
        char *buf = malloc(MC_API_REPLY_CAP);
        CHECK(buf != NULL, "reply buffer");
        if (buf) {
            removepool_handler("removepool|1", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=S,") && strstr(buf, "Code=68,") && g_conf.count == 1 &&
                            g_pool_enabled[0] == 1 && g_pool_enabled[1] == 1,
                    "removepool 1 drops b from pools.conf only (%s)", buf);
            disablepool_handler("disablepool|0", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=51,") &&
                            strstr(buf,
                                    "Msg=Cannot disable last active pool "
                                    "0:'stratum+tcp://a:3333',"),
                    "disablepool 0, the last enabled pool in pools.conf -> E,51 (%s)", buf);
            CHECK(g_conf.count == 1 && g_conf.pools[0].enabled && g_pool_enabled[0] == 1 &&
                            g_pool_enabled[1] == 1,
                    "...and changes neither pools.conf nor the running pools "
                    "(conf %d, running %d %d)",
                    g_conf.pools[0].enabled, g_pool_enabled[0], g_pool_enabled[1]);
            free(buf);
        }
        publish_active_pool(0);
    }

    {
        static const char *const urls[]   = { "stratum+tcp://a:3333", "stratum+tcp://b:3333",
              "stratum+tcp://c:3333" };
        static const char *const users[]  = { "ua", "ub", "uc" };
        static const char *const passes[] = { "x", "x", "x" };
        pool_set                 boot;
        pool_init(&boot, 1000, 60000);
        for (int i = 0; i < 3; i++)
            pool_add(&boot, urls[i], users[i], passes[i]);
        publish_pools(&boot);
        publish_active_pool(-1);
        g_conf = boot;

        mc_api_stats st;
        memset(&st, 0, sizeof st);
        char *buf = malloc(MC_API_REPLY_CAP);
        CHECK(buf != NULL, "reply buffer");
        if (buf) {
            switchpool_handler("switchpool|1{", &st, 0, buf, MC_API_REPLY_CAP, NULL);
            CHECK(strstr(buf, "Switching to pool 1:'stratum+tcp://b:3333'") && g_pool_prio[1] == 0,
                    "text switchpool|1{ switches to pool 1 (%s)", buf);

            mc_api_stats snap;
            memset(&snap, 0, sizeof snap);
            api_snapshot(&snap, &pipe);
            for (int i = 0; i < snap.npools; i++)
                snap.pool[i].last_share_time = 0;
            mc_api_lcd(&snap, buf, MC_API_REPLY_CAP);
            CHECK(strstr(buf, "\"Current Pool\":\"stratum+tcp://b:3333\""),
                    "with no live session lcd reports the top-priority pool b (%s)", buf);
            mc_api_lcd_text(&snap, buf, MC_API_REPLY_CAP);
            CHECK(strstr(buf, "Current Pool=stratum+tcp://b:3333"), "...in text lcd too (%s)", buf);
            mc_api_devs(&snap, buf, MC_API_REPLY_CAP);
            CHECK(strstr(buf, "\"Last Share Pool\":1,"),
                    "...and devs reports pool 1 as Last Share Pool (%s)", buf);
            mc_api_devs_text(&snap, buf, MC_API_REPLY_CAP);
            CHECK(strstr(buf, "Last Share Pool=1,"), "...in text devs too (%s)", buf);

            g_pool_enabled[1] = 0;
            api_snapshot(&snap, &pipe);
            for (int i = 0; i < snap.npools; i++)
                snap.pool[i].last_share_time = 0;
            mc_api_lcd(&snap, buf, MC_API_REPLY_CAP);
            CHECK(strstr(buf, "\"Current Pool\":\"stratum+tcp://a:3333\""),
                    "a disabled top-priority pool is skipped for the next enabled one (%s)", buf);
            g_pool_enabled[1] = 1;
            free(buf);
        }
        publish_active_pool(0);
    }

    {
        static asic_chip_telem act;
        memset(&act, 0, sizeof act);
        act.tm.in_work        = 1;
        act.boards            = 1;
        act.chips[0]          = 1;
        act.pvt_t[0][0]       = 60;
        const int64_t years75 = 75LL * 365 * 86400;
        mc_api_stats  st;

        g_wall_off_s = years75;
        nonce_cb(MSG_ASIC_TELEM, &act, (int)sizeof act);
        g_wall_off_s = 0;
        api_snapshot(&st, &pipe);
        CHECK(st.telem.valid == 1 && st.telem.temp_max == 60,
                "the wall clock stepping back 75 years leaves fresh telemetry "
                "fresh (valid=%d tmax=%d)",
                st.telem.valid, st.telem.temp_max);
        g_mono_off_ms += 7000;
        api_snapshot(&st, &pipe);
        CHECK(st.telem.valid == 0 && st.telem.temp_max == ASIC_TEMP_NONE,
                "...and 7 s of uptime later it has expired, though the wall clock "
                "says it is from the future (valid=%d tmax=%d)",
                st.telem.valid, st.telem.temp_max);

        nonce_cb(MSG_ASIC_TELEM, &act, (int)sizeof act);
        g_wall_off_s = years75;
        api_snapshot(&st, &pipe);
        CHECK(st.telem.valid == 1 && st.telem.temp_max == 60,
                "a wall clock stepping forward does not expire fresh telemetry "
                "(valid=%d tmax=%d)",
                st.telem.valid, st.telem.temp_max);

        g_mono_off_ms += (NONCE_STATS_PERIOD_S + 1) * 1000u;
        g_log_n = 0;
        nonce_ledger_tick();
        CHECK(log_find("nonce stats") != NULL,
                "the nonce stats are written once a period of uptime has passed");
        g_wall_off_s = 0;
        g_mono_off_ms += (NONCE_STATS_PERIOD_S + 1) * 1000u;
        g_log_n = 0;
        nonce_ledger_tick();
        CHECK(log_find("nonce stats") != NULL,
                "...and still a period later, after the wall clock stepped back "
                "75 years");
        g_log_n = 0;
        nonce_ledger_tick();
        CHECK(log_find("nonce stats") == NULL, "...but not twice within the same period");
    }

    {
        g_mono_off_ms  = 0;
        g_mono_off_ms  = (uint32_t)(0x90000000u - (uint32_t)now_ms());
        g_pump_circles = 0;
        g_cs_n         = 0;
        pump_circle();
        CHECK(g_cs_n == 1,
                "the first pump circle pushes MSG_CORE_STATS though the uptime in "
                "ms has bit 31 set, as it does 24.8-49.7 days after boot (%d)",
                g_cs_n);
        pump_circle();
        pump_circle();
        CHECK(g_cs_n == 1, "...the next %u circles push nothing (%d)", CORE_STATS_CIRCLES - 1u,
                g_cs_n);
        pump_circle();
        CHECK(g_cs_n == 2, "...and every %u circles push once more (%d)", CORE_STATS_CIRCLES,
                g_cs_n);
        g_mono_off_ms += (int64_t)(0x100000000ull - 0x90000000ull) - 500;
        pump_circle();
        pump_circle();
        g_mono_off_ms += 1000;
        pump_circle();
        CHECK(g_cs_n == 3, "the push carries on across the 32-bit ms wrap (%d)", g_cs_n);
        g_mono_off_ms = 0;
    }

    {
        g_mono_off_ms = 1000;
        deliver_job(&ctx, f_job_id, f_xnonce1);
        g_cs_n = 0;
        core_stats_push();
        CHECK(g_cs_n == 1 && g_cs.job_gap_s == 0,
                "MSG_CORE_STATS reports a zero gap right after a job (%u)", g_cs.job_gap_s);
        g_mono_off_ms += 125000;
        core_stats_push();
        CHECK(g_cs_n == 2 && g_cs.job_gap_s == 125,
                "...and the seconds since that job as time passes (%u)", g_cs.job_gap_s);
        g_log_n = 0;
        g_mono_off_ms += 300000;
        pump_circle();
        CHECK(log_find("no new job") == NULL,
                "core_miner logs nothing about the gap; mm_miner owns it");
        deliver_job(&ctx, f_job_id, f_xnonce1);
        core_stats_push();
        CHECK(g_cs.job_gap_s == 0, "a new job resets the gap (%u)", g_cs.job_gap_s);
        g_mono_off_ms = 0;
    }

    {
        static char cb1_long[2 * 4000 + 1];
        static char cb1_over[2 * MC_COINBASE_MAX + 3];
        static char cb2_fit[2 * MC_COINBASE_MAX + 1];
        static char cb2_over[2 * MC_COINBASE_MAX + 3];
        static char branch[MC_MERKLES_MAX + 1][65];
        sv1_client  c;
        sv1_event   ev = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };

        reset_state(&pipe);
        fill_client(&c, "BIG1", f_xnonce1);
        memset(cb1_long, 'a', sizeof cb1_long - 1);
        size_t n2off = (strlen(cb1_long) + strlen(f_xnonce1)) / 2;
        size_t room  = MC_COINBASE_MAX - 64 - n2off % 64 - (size_t)c.xnonce2_size;
        memset(cb2_fit, 'a', 2 * room);
        cb2_fit[2 * room] = '\0';
        memset(cb2_over, 'a', 2 * room + 2);
        cb2_over[2 * room + 2] = '\0';
        size_t cb_total        = n2off + (size_t)c.xnonce2_size + room;

        c.job.coinb1 = cb1_long;
        c.job.coinb2 = cb2_over;
        g_tmpl_n     = 0;
        g_log_n      = 0;
        on_sv1_event(&ctx, &c, &ev);
        CHECK(g_tmpl_n == 0 && g_job_seq == 0 && g_stratum.have_job == 0,
                "a first job whose coinbase from the nonce2 block on is one byte "
                "over %d - 64 is not sent and leaves no job (sent=%d seq=%u "
                "have=%d)",
                MC_COINBASE_MAX, g_tmpl_n, g_job_seq, g_stratum.have_job);
        g_tmpl_n = 0;
        pump_circle();
        CHECK(g_tmpl_n == 0, "...and the pump sends nothing for it (%d)", g_tmpl_n);

        fill_client(&c, "BIG1", f_xnonce1);
        c.job.coinb1 = cb1_long;
        c.job.coinb2 = cb2_fit;
        g_tmpl_n     = 0;
        on_sv1_event(&ctx, &c, &ev);
        uint32_t keep = g_job_seq;
        CHECK(cb_total > MC_COINBASE_MAX && g_tmpl_n == 1 && g_stratum.have_job == 1 && keep != 0 &&
                        !strcmp(g_tmpl.coinb1, cb1_long) && !strcmp(g_tmpl.coinb2, cb2_fit) &&
                        !strcmp(g_tmpl.merkle, f_merkle),
                "a %zu-byte coinbase whose part from the nonce2 block on is "
                "exactly %d - 64 bytes reaches the ASICs whole (sent=%d coinb1 "
                "%zu/%zu coinb2 %zu/%zu chars)",
                cb_total, MC_COINBASE_MAX, g_tmpl_n, strlen(g_tmpl.coinb1), strlen(cb1_long),
                strlen(g_tmpl.coinb2), strlen(cb2_fit));
        CHECK(g_ring[keep % JOB_RING].hdr_ok, "...and its nonces can be verified");

        char want[64];
        snprintf(want, sizeof want, "coinbase tail %d bytes (max %d)", MC_COINBASE_MAX + 1,
                MC_COINBASE_MAX);
        fill_client(&c, "BIG2", f_xnonce1);
        c.job.coinb1 = cb1_long;
        c.job.coinb2 = cb2_over;
        g_tmpl_n     = 0;
        g_log_n      = 0;
        on_sv1_event(&ctx, &c, &ev);
        const char *l = log_find("BIG2");
        CHECK(g_tmpl_n == 0 && g_job_seq == keep && g_stratum.have_job == 1,
                "a job one coinbase tail byte over the limit is not sent and the "
                "live job stays (sent=%d seq=%u/%u)",
                g_tmpl_n, g_job_seq, keep);
        CHECK(l && strstr(l, want), "...with an error naming its tail length and the limit (%s)",
                l ? l : "no log");
        CHECK(!strcmp(g_ring[keep % JOB_RING].job_id, "BIG1") &&
                        g_ring[keep % JOB_RING].seq == keep,
                "...and its ring slot untouched");
        g_tmpl_n = 0;
        pump_circle();
        CHECK(g_tmpl_n == 1 && g_tmpl.seq == keep && !strcmp(g_tmpl.coinb2, cb2_fit),
                "...so the pump keeps resending the live job, not the refused "
                "one (sent=%d seq=%u)",
                g_tmpl_n, g_tmpl.seq);

        memset(cb1_over, 'a', sizeof cb1_over - 1);
        fill_client(&c, "BIG3", f_xnonce1);
        c.job.coinb1 = cb1_over;
        g_tmpl_n     = 0;
        g_log_n      = 0;
        on_sv1_event(&ctx, &c, &ev);
        l = log_find("BIG3");
        snprintf(want, sizeof want, "coinb1 %zu hex chars (max %d)", sizeof cb1_over - 1,
                MC_COINB_HEX_LEN - 1);
        CHECK(g_tmpl_n == 0 && g_job_seq == keep && !strcmp(g_ring[keep % JOB_RING].job_id, "BIG1"),
                "a coinb1 the job field cannot hold is not sent even with its "
                "tail in limit, and the live job stays (sent=%d seq=%u/%u)",
                g_tmpl_n, g_job_seq, keep);
        CHECK(l && strstr(l, want), "...with an error naming the field limit (%s)",
                l ? l : "no log");

        fill_client(&c, "N2SZ2", f_xnonce1);
        c.xnonce2_size = 2;
        g_tmpl_n       = 0;
        g_log_n        = 0;
        on_sv1_event(&ctx, &c, &ev);
        l = log_find("N2SZ2");
        CHECK(g_tmpl_n == 0 && g_job_seq == keep && !strcmp(g_ring[keep % JOB_RING].job_id, "BIG1"),
                "a job with a 2-byte nonce2 is not sent and the live job stays "
                "(sent=%d seq=%u/%u)",
                g_tmpl_n, g_job_seq, keep);
        CHECK(l && strstr(l, "nonce2 size 2 (min 3)"),
                "...with an error naming the size and the minimum (%s)", l ? l : "no log");

        fill_client(&c, "N2SZ3", f_xnonce1);
        c.xnonce2_size = 3;
        g_tmpl_n       = 0;
        on_sv1_event(&ctx, &c, &ev);
        CHECK(g_tmpl_n == 1 && g_job_seq == keep + 1 && g_tmpl.xnonce2_size == 3,
                "a job with a 3-byte nonce2 is sent (sent=%d n2size=%d)", g_tmpl_n,
                g_tmpl.xnonce2_size);

        for (int i = 0; i <= MC_MERKLES_MAX; i++)
            snprintf(branch[i], sizeof branch[i], "%064x", (unsigned)i + 1);
        fill_client(&c, "MK30", f_xnonce1);
        for (int i = 0; i < MC_MERKLES_MAX; i++)
            c.job.merkle[i] = branch[i];
        c.job.n_merkle = MC_MERKLES_MAX;
        g_tmpl_n       = 0;
        on_sv1_event(&ctx, &c, &ev);
        keep = g_job_seq;
        CHECK(g_tmpl_n == 1 && strlen(g_tmpl.merkle) == (size_t)MC_MERKLE_HEX_LEN - 1,
                "a job with %d merkle branches is sent with all of them "
                "(sent=%d, %zu chars)",
                MC_MERKLES_MAX, g_tmpl_n, strlen(g_tmpl.merkle));

        fill_client(&c, "MK31", f_xnonce1);
        for (int i = 0; i <= MC_MERKLES_MAX; i++)
            c.job.merkle[i] = branch[i];
        c.job.n_merkle = MC_MERKLES_MAX + 1;
        g_tmpl_n       = 0;
        g_log_n        = 0;
        on_sv1_event(&ctx, &c, &ev);
        l = log_find("MK31");
        CHECK(g_tmpl_n == 0 && g_job_seq == keep && g_stratum.have_job == 1 &&
                        !strcmp(g_ring[keep % JOB_RING].job_id, "MK30"),
                "a job with %d merkle branches is not sent and the live job "
                "stays (sent=%d seq=%u/%u)",
                MC_MERKLES_MAX + 1, g_tmpl_n, g_job_seq, keep);
        snprintf(want, sizeof want, "%d merkle branches (max %d)", MC_MERKLES_MAX + 1,
                MC_MERKLES_MAX);
        CHECK(l && strstr(l, want), "...with an error naming the count and the limit (%s)",
                l ? l : "no log");
    }

    {
        static char bad_prevhash[65];
        memset(bad_prevhash, 'z', sizeof bad_prevhash - 1);
        sv1_client c;
        sv1_event  ev = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };

        reset_state(&pipe);
        g_tmpl_n = g_tmpl_unlocked = 0;
        uint32_t keep              = deliver_job(&ctx, "HDR1", f_xnonce1);
        CHECK(g_tmpl_n == 1 && g_tmpl_unlocked == 0,
                "a notify sends its template with the session lock held "
                "(sent=%d unlocked=%d)",
                g_tmpl_n, g_tmpl_unlocked);

        fill_client(&c, "HDRBAD", f_xnonce1);
        c.job.prevhash   = bad_prevhash;
        c.job.clean_jobs = 1;
        g_tmpl_n         = 0;
        g_log_n          = 0;
        uint32_t dead    = g_dead_seq;
        on_sv1_event(&ctx, &c, &ev);
        char want[48];
        snprintf(want, sizeof want, "job %s unverifiable", "HDRBAD");
        CHECK(g_tmpl_n == 0 && g_job_seq == keep && g_dead_seq == dead && g_stratum.have_job == 1 &&
                        !strcmp(g_ring[keep % JOB_RING].job_id, "HDR1") &&
                        !strcmp(g_ring[(keep + 1) % JOB_RING].job_id, ""),
                "a job whose header will not build is not sent and the live job "
                "stays (sent=%d seq=%u/%u dead=%u/%u)",
                g_tmpl_n, g_job_seq, keep, g_dead_seq, dead);
        CHECK(log_find(want) != NULL, "...with the unverifiable error (%s)", want);
        CHECK(g_pending_clean == 1, "...and its clean flag is kept pending (%d)", g_pending_clean);

        g_tmpl_n      = 0;
        uint32_t next = deliver_job(&ctx, "HDR2", f_xnonce1);
        CHECK(g_tmpl_n == 1 && next == keep + 1 && g_dead_seq == keep && g_pending_clean == 0,
                "...so the next good job is clean (sent=%d seq=%u dead=%u)", g_tmpl_n, next,
                g_dead_seq);
    }

    if (fails) {
        fprintf(stderr, "test_core_session: %d FAIL\n", fails);
        return 1;
    }
    printf("test_core_session: all pass\n");
    return 0;
}
