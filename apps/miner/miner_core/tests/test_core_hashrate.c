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
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osif.h"
#include "miner_ipc.h"
#include "driver_avalon.h"
#include "avalon_gen.h"
#include "stratum_net.h"

#include "../../tests/include/tst_check.h"

#ifndef STRATUM_FIXTURES_DIR
/* Directory holding the stratum/ fixtures when the build does not set it */
#define STRATUM_FIXTURES_DIR "."
#endif

static int mock_send(void *io, const char *line);

static mc_asic_job g_tmpl;          /* Last MSG_ASIC_JOB template sent */
static int         g_tmpl_n;        /* MSG_ASIC_JOB templates sent */
static int         g_tmpl_locked;   /* last template sent with g_stratum.lock held */
static char        g_log[128][256]; /* Captured osif_log() lines */
static int         g_log_n;         /* osif_log() lines captured */

static int64_t g_mono_off_ms; /* Offset added to CLOCK_MONOTONIC, ms; applied in whole seconds */

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
static int  g_send_fail;    /* mock_send fails when set */
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

static int tmpl_lock_held(void);

osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (type == MSG_ASIC_JOB && data && len == (int)sizeof(mc_asic_job)) {
        memcpy(&g_tmpl, data, sizeof g_tmpl);
        g_tmpl_n++;
        g_tmpl_locked = tmpl_lock_held();
    }
    return OSIF_OK;
}

static int gen_chip(uint8_t c, avalon_job_in *out)
{
    if (mc_gen_chip_work(&g_tmpl, c, out) != 0)
        return -1;
    out->user_note[0] = c;
    out->user_note[1] = g_tmpl.seq;
    out->user_note[2] = __builtin_bswap32((uint32_t)c);
    out->user_note[3] = 0;
    return 0;
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

void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

static const char *log_find(const char *needle)
{
    for (int i = 0; i < g_log_n; i++)
        if (strstr(g_log[i], needle))
            return g_log[i];
    return NULL;
}

static int test_clock_gettime(clockid_t id, struct timespec *ts)
{
    int rc = clock_gettime(id, ts);
    if (rc == 0 && id == CLOCK_MONOTONIC)
        ts->tv_sec += g_mono_off_ms / 1000;
    return rc;
}
#define clock_gettime test_clock_gettime

#define main core_miner_main_unused
#include "core_miner_tu.h"
#undef main

static int tmpl_lock_held(void)
{
    if (pthread_mutex_trylock(&g_stratum.lock) == EBUSY)
        return 1;
    pthread_mutex_unlock(&g_stratum.lock);
    return 0;
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
    return g_send_fail ? -1 : 0;
}

static void fill_client(sv1_client *c, const char *job_id, const char *prevhash, double difficulty)
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
    c->job.prevhash  = (char *)prevhash;
    c->job.coinb1    = f_coinb1;
    c->job.coinb2    = f_coinb2;
    c->job.merkle[0] = f_merkle;
    c->job.n_merkle  = 1;
}

static uint32_t deliver_job(core_session_ctx *ctx, const char *job_id, const char *prevhash,
        double difficulty)
{
    sv1_client c;
    fill_client(&c, job_id, prevhash, difficulty);
    sv1_event ev = { .type = SV1_EV_SET_DIFFICULTY, .submit_accepted = 0 };
    on_sv1_event(ctx, &c, &ev);
    ev.type = SV1_EV_NOTIFY;
    on_sv1_event(ctx, &c, &ev);
    return g_job_seq;
}

static void deliver_nonce_full(uint32_t seq, uint32_t n2, uint32_t last_n2, uint32_t note2,
        uint32_t nonce, uint32_t version)
{
    avalon_nonce_out no;
    memset(&no, 0, sizeof no);
    no.valid           = 1;
    no.job_id          = seq;
    no.nonce2          = n2;
    no.last_job_nonce2 = last_n2;
    no.asic_id         = (uint16_t)__builtin_bswap32(note2);
    no.miner_id        = (uint16_t)(__builtin_bswap32(note2) >> 16);
    no.nonce           = __builtin_bswap32(nonce);
    no.version         = __builtin_bswap32(version);
    no.ntime           = f_ntime_off;
    nonce_cb(MSG_ASIC_NONCE, &no, (int)sizeof no);
}

static void deliver_nonce_at(uint32_t seq, uint32_t n2, uint32_t note2, uint32_t nonce,
        uint32_t version)
{
    deliver_nonce_full(seq, n2, 0, note2, nonce, version);
}

static void deliver_share(uint32_t seq, uint32_t n2, uint32_t note2)
{
    deliver_nonce_at(seq, n2, note2, f_true_nonce, f_true_version);
}

static void reset_state(mc_pipeline *p)
{
    mc_pipeline_init(p);
    memset(g_ring, 0, sizeof g_ring);
    g_job_seq  = 0;
    g_dead_seq = 0;
    zero_all_stats();
    for (int i = 0; i < POOL_VALID_NUM; i++)
        g_pool_st[i].works = 0;
    g_sent_n = 0;
    g_tmpl_n = 0;
    g_log_n  = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        g_chip.chips[b] = MINER_MAX_CHIPS;
    g_stratum.pipe     = p;
    g_stratum.t        = &g_mock;
    g_stratum.have_job = 0;
    publish_active_pool(0);
}

int main(void)
{
    if (load_share() != 0) {
        fprintf(stderr, "  FAIL: no usable accepted share in happy.shares.tsv\n");
        return 2;
    }
    printf("test_core_hashrate: MINER_MAX_CHIPS=%d\n", (int)MINER_MAX_CHIPS);

    mc_pipeline      pipe;
    core_session_ctx ctx = { .pipe = &pipe, .user = "poolworker.001" };
    double           d   = strtod(f_diff, NULL);

    reset_state(&pipe);
    pump_circle();
    CHECK(g_tmpl_n == 0, "no job: the pump sends nothing");

    uint32_t seq = deliver_job(&ctx, f_job_id, f_prevhash, d);
    CHECK(g_tmpl_n == 1 && g_tmpl.seq == seq, "the notify sends one work template (n=%d seq=%u)",
            g_tmpl_n, g_tmpl.seq);
    CHECK(PUMP_PERIOD_MS == 1000u, "one pump circle per second (%u ms)", PUMP_PERIOD_MS);
    for (int k = 1; k <= 2; k++) {
        pump_circle();
        CHECK(g_tmpl_n == 1 + k && g_tmpl.seq == seq,
                "circle %d resends the current job (n=%d seq=%u)", k, g_tmpl_n, g_tmpl.seq);
        CHECK(g_tmpl_locked, "circle %d sends the template under g_stratum.lock", k);
        CHECK(g_pool_st[0].works == 0, "circle %d adds no Works (%u)", k, g_pool_st[0].works);
    }
    {
        CHECK(log_find("nonce stats") == NULL, "no nonce stats line before one period of uptime");
        g_mono_off_ms = (NONCE_STATS_PERIOD_S + 1) * 1000u;
        nonce_ledger_tick();
        g_mono_off_ms = 0;
        const char *l = log_find("nonce stats");
        CHECK(l && strstr(l, " nonces, ") && strstr(l, " hw, "),
                "the nonce stats reach the log with their denominator (%s)", l ? l : "-");
    }
    {
        avalon_job_in a, b;
        CHECK(gen_chip(0, &a) == 0 && gen_chip((uint8_t)(MINER_MAX_CHIPS - 1), &b) == 0 &&
                        a.user_note[0] == 0 && b.user_note[0] == (uint32_t)MINER_MAX_CHIPS - 1,
                "asic gen hands chip c extranonce2 c");
    }

    reset_state(&pipe);
    pump_circle();
    CHECK(g_tmpl_n == 0, "no pool job: sends nothing (n=%d)", g_tmpl_n);

    reset_state(&pipe);
    {
        sv1_client c;
        fill_client(&c, f_job_id, f_prevhash, d);
        c.version_rolling = 1;
        c.version_mask    = 0x1fffe000u;
        sv1_event ev      = { .type = SV1_EV_CONFIGURED, .submit_accepted = 0 };
        on_sv1_event(&ctx, &c, &ev);
        const char *l = log_find("version-rolling");
        CHECK(l != NULL, "the granted mask is logged at all%s", l ? "" : " (no line)");
        CHECK(l && strstr(l, "1fffe000"), "the log carries the negotiated mask (%s)", l ? l : "-");
        g_log_n           = 0;
        c.version_rolling = 0;
        c.version_mask    = 0;
        on_sv1_event(&ctx, &c, &ev);
        l = log_find("version-rolling");
        CHECK(l && strstr(l, "NOT"), "a REFUSED mask is logged as such (%s)", l ? l : "-");
    }

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_prevhash, d);
    deliver_share(seq, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS, 0);
    CHECK(g_sent_n == 1,
            "a nonce one lap behind its echoed extranonce2 is corrected "
            "and submitted (%d)",
            g_sent_n);
    CHECK(g_nst.hw_errors == 0, "...and is not counted as a hardware error (%u)", g_nst.hw_errors);
    CHECK(g_pool_st[0].works == 1 && g_local_work == 4,
            "...and adds 1 Works and 4 Local Work (works=%u local=%u)", g_pool_st[0].works,
            g_local_work);

#if HBOARD_COUNT > 1
    reset_state(&pipe);
    g_chip.chips[HBOARD_COUNT - 1] = MINER_MAX_CHIPS - 1;
    seq                            = deliver_job(&ctx, f_job_id, f_prevhash, d);
    deliver_share(seq, (uint32_t)HBOARD_COUNT * (MINER_MAX_CHIPS - 1),
            __builtin_bswap32((uint32_t)(HBOARD_COUNT - 1) << 16));
    CHECK(g_sent_n == 1 && g_nst.hw_errors == 0,
            "unequal chip counts: a nonce on the short board is corrected by its own lap "
            "(%d sent, %u hw)",
            g_sent_n, g_nst.hw_errors);

    reset_state(&pipe);
    g_chip.chips[HBOARD_COUNT - 1] = MINER_MAX_CHIPS - 1;
    seq                            = deliver_job(&ctx, f_job_id, f_prevhash, d);
    deliver_share(seq, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS, 0);
    CHECK(g_sent_n == 1 && g_nst.hw_errors == 0,
            "unequal chip counts: a nonce on a full board keeps its own lap "
            "(%d sent, %u hw)",
            g_sent_n, g_nst.hw_errors);

    reset_state(&pipe);
    g_chip.chips[HBOARD_COUNT - 1] = 0;
    seq                            = deliver_job(&ctx, f_job_id, f_prevhash, d);
    deliver_share(seq, (uint32_t)(HBOARD_COUNT - 1) * MINER_MAX_CHIPS, 0);
    CHECK(g_sent_n == 1 && g_nst.hw_errors == 0,
            "a board with no chips does not count toward the lap (%d sent, %u hw)", g_sent_n,
            g_nst.hw_errors);
#endif

    reset_state(&pipe);
    uint32_t j1 = deliver_job(&ctx, f_job_id, f_prevhash, d);
    uint32_t j2 = deliver_job(&ctx, "NEWJOBID", f_prevhash, d);
    CHECK(j2 == j1 + 1, "the second notify took the next ring slot (%u -> %u)", j1, j2);
    deliver_nonce_full(j2, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS - 1, 0, 0, f_true_nonce,
            f_true_version);
    CHECK(g_sent_n == 1,
            "a nonce for the previous job is reconstructed via "
            "last_job_nonce2 (%d)",
            g_sent_n);

    reset_state(&pipe);
    j1 = deliver_job(&ctx, f_job_id, f_prevhash, d);
    j2 = deliver_job(&ctx, "NEWJOBID", f_prevhash, 4.0e9);
    deliver_nonce_full(j2, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS - 1, 0, 0, f_true_nonce,
            f_true_version);
    CHECK(g_sent_n == 1 && g_nst.hit_repaired == 1 && g_nst.low_diff == 0,
            "a nonce repaired onto the previous job is checked against that job's "
            "difficulty, not the current one's (sent=%d repaired=%u lowdiff=%u)",
            g_sent_n, g_nst.hit_repaired, g_nst.low_diff);

    reset_state(&pipe);
    j1 = deliver_job(&ctx, f_job_id, f_prevhash, 4.0e9);
    j2 = deliver_job(&ctx, "NEWJOBID", f_prevhash, d);
    deliver_nonce_full(j2, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS - 1, 0, 0, f_true_nonce,
            f_true_version);
    CHECK(g_sent_n == 0 && g_nst.low_diff == 1 && g_nst.hw_errors == 0,
            "...so it is above target when only the current job's difficulty "
            "would pass it (sent=%d lowdiff=%u hw=%u)",
            g_sent_n, g_nst.low_diff, g_nst.hw_errors);

    reset_state(&pipe);
    seq             = deliver_job(&ctx, f_job_id, f_prevhash, d);
    g_ping.inflight = 0;
    g_ping.sent_ms  = 0;
    g_send_fail     = 1;
    deliver_share(seq, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS, 0);
    g_send_fail = 0;
    CHECK(g_sent_n == 1 && g_ping.inflight == 0 && g_ping.sent_ms == 0,
            "a submit whose send failed does not arm the ping timer "
            "(sent=%d inflight=%d)",
            g_sent_n, g_ping.inflight);
    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_prevhash, d);
    deliver_share(seq, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS, 0);
    CHECK(g_sent_n == 1 && g_ping.inflight == 1 && g_ping.sent_ms != 0,
            "...a successful one does (inflight=%d)", g_ping.inflight);
    g_ping.inflight = 0;
    g_ping.sent_ms  = 0;

    reset_state(&pipe);
    j1 = deliver_job(&ctx, f_job_id, f_prevhash, d);
    {
        sv1_client c;
        fill_client(&c, "NEWJOBID",
                "00000000000000000000000000000000"
                "00000000000000000000000000000001",
                d);
        c.job.clean_jobs = 1;
        sv1_event ev     = { .type = SV1_EV_NOTIFY, .submit_accepted = 0 };
        on_sv1_event(&ctx, &c, &ev);
        j2 = g_job_seq;
    }
    CHECK(j2 == j1 + 1 && g_dead_seq == j1,
            "a clean notify retires the previous job (j1=%u j2=%u dead=%u)", j1, j2, g_dead_seq);
    uint32_t lw = g_local_work;
    deliver_nonce_full(j2, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS - 1, 0, 0, f_true_nonce,
            f_true_version);
    CHECK(g_sent_n == 0 && g_nst.hit_repaired == 0 && g_nst.hw_errors == 0 && g_nst.retired == 1,
            "a nonce that needs the previous job after a clean notify retired it is "
            "counted retired, not submitted (sent=%d repaired=%u hw=%u retired=%u)",
            g_sent_n, g_nst.hit_repaired, g_nst.hw_errors, g_nst.retired);
    CHECK(g_pool_st[0].works == 1 && g_local_work - lw == 2 && g_pool_st[0].diff1 == 0,
            "...and adds 1 Works, 2 Local Work and no diff1 (works=%u local=+%u "
            "diff1=%g)",
            g_pool_st[0].works, g_local_work - lw, g_pool_st[0].diff1);

    reset_state(&pipe);
    j1 = deliver_job(&ctx, f_job_id, f_prevhash, d);
    session_drop();
    g_stratum.t = &g_mock;
    publish_active_pool(1);
    j2 = deliver_job(&ctx, "NEWJOBID",
            "00000000000000000000000000000000"
            "00000000000000000000000000000001",
            d);
    deliver_nonce_full(j2, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS - 1, 0, 0, f_true_nonce,
            f_true_version);
    CHECK(g_sent_n == 0 && g_nst.hit_repaired == 0 && g_nst.retired == 1,
            "the repair does not reach a previous job of another pool across a session "
            "drop (sent=%d repaired=%u retired=%u)",
            g_sent_n, g_nst.hit_repaired, g_nst.retired);
    CHECK(g_pool_st[1].works == 1 && g_pool_st[1].diff1 == 0 && g_pool_st[0].diff1 == 0,
            "...and credits no pool (works=%u diff1[1]=%g diff1[0]=%g)", g_pool_st[1].works,
            g_pool_st[1].diff1, g_pool_st[0].diff1);
    publish_active_pool(0);
    {
        uint32_t w = g_pool_st[0].works;
        deliver_share(j1, 0, 0);
        CHECK(g_nst.retired == 2 && g_pool_st[0].works == w,
                "a nonce for a retired job adds no Works (retired=%u works=%u)", g_nst.retired,
                g_pool_st[0].works);
    }

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_prevhash, d);
    {
        avalon_nonce_out b[2];
        memset(b, 0, sizeof b);
        for (int i = 0; i < 2; i++) {
            b[i].valid  = 1;
            b[i].job_id = seq;
        }
        nonce_cb(MSG_ASIC_NONCE, b, (int)sizeof b);
        nonce_cb(MSG_ASIC_NONCE, b, (int)sizeof b[0] - 1);
        CHECK(g_nst.rx == 0,
                "a datagram longer or shorter than one nonce is "
                "dropped (%u)",
                g_nst.rx);
        nonce_cb(MSG_ASIC_NONCE, b, (int)sizeof b[0]);
        CHECK(g_nst.rx == 1, "a single-nonce datagram decodes as one (%u)", g_nst.rx);
    }

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_prevhash, d);
    {
        int           bad = -1;
        avalon_job_in jc;
        for (int c = 0; c < MINER_MAX_CHIPS; c++) {
            if (gen_chip((uint8_t)c, &jc) != 0) {
                bad = c;
                break;
            }
            uint32_t w = __builtin_bswap32(jc.user_note[2]);
            if ((w >> 16) != 0 || (w & 0xffffu) != (uint32_t)c) {
                bad = c;
                break;
            }
        }
        CHECK(bad < 0,
                "every generated work stamps its own (board, chip) identity in "
                "note word 2 (first bad chip %d)",
                bad);
        avalon_job_in j0;
        gen_chip(0, &j0);
        deliver_share(seq, j0.user_note[0], j0.user_note[2]);
        CHECK(g_sent_n == 1, "the chip-0 share is submitted (%d)", g_sent_n);
        CHECK(g_mw[0][0] == 1, "...and credits MW[0][0] (%llu)", (unsigned long long)g_mw[0][0]);
        CHECK(g_pool_st[0].works == 1 && g_local_work == 2,
                "...and adds 1 Works and 2 Local Work (works=%u local=%u)", g_pool_st[0].works,
                g_local_work);
        g_sent_n = 0;
        deliver_share(seq, j0.user_note[0], j0.user_note[2]);
        CHECK(g_sent_n == 0 && g_pool_st[0].works == 2,
                "a duplicate of it still adds 1 Works (sent=%d works=%u)", g_sent_n,
                g_pool_st[0].works);
    }

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_prevhash, d);
    avalon_job_in hj0;
    gen_chip(0, &hj0);

    deliver_nonce_at(seq, hj0.user_note[0], hj0.user_note[2], 0xdeadbeefu, f_true_version);
    CHECK(g_nst.hw_errors == 1, "a genuinely unverifiable nonce is one HW error (%u)",
            g_nst.hw_errors);
    CHECK(g_pool_st[0].works == 1, "...and adds 1 Works when no repair is tried (%u)",
            g_pool_st[0].works);
    {
        const char *l = log_find("verify FAIL");
        CHECK(l && strstr(l, "last="),
                "the verify-FAIL line reports the echoed "
                "extranonce2 words (%s)",
                l ? l : "-");
    }

    g_log_n     = 0;
    g_stratum.t = NULL;
    deliver_nonce_at(seq + 500u, 0, 0, f_true_nonce, f_true_version);
    CHECK(log_find("no pool session") != NULL || log_find("NO pool session") != NULL,
            "a nonce that arrived with no pool session says so (%s)", g_log_n ? g_log[0] : "-");
    g_stratum.t = &g_mock;

    g_log_n = 0;
    deliver_nonce_at(seq + 500u, 0, 0, f_true_nonce, f_true_version);
    CHECK(log_find("no-match") != NULL,
            "an unknown job seq is still reported as a "
            "no-match (%s)",
            g_log_n ? g_log[0] : "-");
    CHECK(g_nst.hw_errors == 3, "HW[] still counts exactly the three old causes (%u)",
            g_nst.hw_errors);
    CHECK(g_pool_st[0].works == 1, "unmatched nonces add no Works (%u)", g_pool_st[0].works);
    deliver_nonce_at(seq, (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS, 0, 0xdeadbeefu, f_true_version);
    CHECK(g_nst.hw_errors == 4 && g_pool_st[0].works == 2 && g_local_work == 6,
            "an unverifiable nonce whose repair is tried adds 1 Works and 4 Local Work "
            "(hw=%u works=%u local=%u)",
            g_nst.hw_errors, g_pool_st[0].works, g_local_work);

    reset_state(&pipe);
    seq = deliver_job(&ctx, f_job_id, f_prevhash, 4.0e9);
    {
        avalon_job_in j0;
        gen_chip(0, &j0);
        deliver_share(seq, j0.user_note[0], j0.user_note[2]);
    }
    CHECK(g_sent_n == 0 && g_nst.hw_errors == 0,
            "a sub-pool-difficulty share is neither submitted nor a HW error "
            "(sent=%d hw=%u)",
            g_sent_n, g_nst.hw_errors);
    CHECK(log_find("Share above target") != NULL, "...and it is visible%s",
            log_find("Share above target") ? "" : " (no line)");

    {
        char     want[16], live[16];
        uint32_t live_mask = f_mask & ~0x00002000u;
        snprintf(want, sizeof want, "%08x", f_true_version & f_mask);
        snprintf(live, sizeof live, "%08x", f_true_version & live_mask);

        reset_state(&pipe);
        seq            = deliver_job(&ctx, f_job_id, f_prevhash, d);
        g_version_mask = live_mask;
        {
            avalon_job_in j0;
            gen_chip(0, &j0);
            deliver_share(seq, j0.user_note[0], j0.user_note[2]);
        }
        CHECK(g_sent_n == 1 && g_nst.hw_errors == 0,
                "a live-mask narrowing loses neither the share nor its honesty "
                "(sent=%d hw=%u)",
                g_sent_n, g_nst.hw_errors);
        CHECK(g_sent_n == 1 && strstr(g_sent[0], want) != NULL,
                "the submit carries the JOB's version_bits %s (%s)", want,
                g_sent_n ? g_sent[0] : "-");
        CHECK(!g_sent_n || strstr(g_sent[0], live) == NULL, "...and never the live mask's %s",
                live);
    }

    {
        uint32_t jver = (uint32_t)strtoul(f_version, NULL, 16);

        reset_state(&pipe);
        {
            sv1_client c;
            fill_client(&c, f_job_id, f_prevhash, d);
            c.version_rolling = 0;
            sv1_event ev      = { .type = SV1_EV_SET_DIFFICULTY, .submit_accepted = 0 };
            on_sv1_event(&ctx, &c, &ev);
            ev.type = SV1_EV_NOTIFY;
            on_sv1_event(&ctx, &c, &ev);
        }
        seq = g_job_seq;
        CHECK(strtoul(g_ring[seq % JOB_RING].mask, NULL, 16) == 0,
                "the job froze mask 0 (notify before the version-rolling grant)");
        g_version_mask = f_mask;
        {
            avalon_job_in j0;
            int           ok     = gen_chip(0, &j0) == 0;
            int           rolled = -1;
            for (int i = 0; i < TOAST_WORK_VERSION_WORDS; i++)
                if (j0.version[i] != jver) {
                    rolled = i;
                    break;
                }
            CHECK(ok && rolled < 0,
                    "no work-image version slot rolls a bit the job's mask does not "
                    "cover (slot %d = %08x, want %08x)",
                    rolled < 0 ? 0 : rolled, j0.version[rolled < 0 ? 0 : rolled], jver);
        }

        reset_state(&pipe);
        seq            = deliver_job(&ctx, f_job_id, f_prevhash, d);
        g_version_mask = 0;
        {
            avalon_job_in j0;
            int           ok    = gen_chip(0, &j0) == 0;
            uint32_t      want  = (jver & ~f_mask) | (f_true_version & f_mask);
            int           found = 0, distinct = 0;
            for (int i = 0; i < TOAST_WORK_VERSION_WORDS; i++) {
                if (j0.version[i] == want)
                    found = 1;
                if (j0.version[i] != j0.version[0])
                    distinct = 1;
            }
            CHECK(ok && distinct,
                    "the work image rolls the JOB's mask, not 8 identical slots (%08x)",
                    j0.version[0]);
            CHECK(found,
                    "the version slot this job's own share is verified in (%08x) "
                    "is one the work image dispatched",
                    want);
        }
    }

    if (fails) {
        fprintf(stderr, "test_core_hashrate: %d FAIL\n", fails);
        return 1;
    }
    printf("test_core_hashrate: all pass\n");
    return 0;
}
