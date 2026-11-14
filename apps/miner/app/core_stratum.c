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

#include "core_internal.h"
#include "fault_report.h"
#include "work.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHA256_BLOCK_BYTES 64
#define NONCE2_SIZE_MIN    3   /* minimum extranonce2 size, bytes */
#define POOL_TRACE_MAX     512 /* longest pool line traced in full, bytes */

typedef struct {
    mc_pipeline *pipe;
    const char  *user;
    uint64_t     start_ms; /* connect time, ms */
    int          got_job;  /* a job of the session reached the ASICs */
} core_session_ctx;

static uint32_t g_version_mask;
static uint32_t g_ping_win[3];    /* last three submit round trips, ms */
static int      g_netfail_index;  /* current pair index into g_netfail_time */
static char     g_diff[32] = "1"; /* pool difficulty, text */
static int      g_pending_clean;  /* a clean_jobs notify is pending */
static char     g_sess_prevhash[sizeof((job_tmpl *)0)->prevhash]; /* session's last prevhash, hex */
static _Atomic uint64_t g_job_at_ms;                              /* last job arrival time, ms */
_Thread_local int       g_trace_quiet; /* pool_trace() ignores this thread's lines */

static int core_pool_active(void)
{
    pthread_mutex_lock(&g_pools_lock);
    int pa = g_pool_active;
    pthread_mutex_unlock(&g_pools_lock);
    return pa;
}

static void diff_suffix(double v, char *out, size_t cap)
{
    static const char *sfx[] = { "", "K", "M", "G", "T", "P", "E" }; /* SI suffixes */
    uint64_t           val   = v > 0 ? (uint64_t)(v + 0.5) : 0;
    int                i     = 0;
    double             d     = (double)val;
    while (d >= 1000.0 && i < 6) {
        d /= 1000.0;
        i++;
    }
    if (i == 0)
        snprintf(out, cap, "%u", (unsigned)d);
    else
        snprintf(out, cap, "%.3g%s", d, sfx[i]);
}

void pool_trace(int tx, const char *line)
{
    if (g_trace_quiet)
        return;
    size_t n = strlen(line);
    if (tx && strstr(line, "\"method\":\"mining.authorize\""))
        osif_log(OSIF_LOG_DBG, "pool >> mining.authorize");
    else if (n <= POOL_TRACE_MAX)
        osif_log(OSIF_LOG_DBG, "pool %s %s", tx ? ">>" : "<<", line);
    else
        osif_log(OSIF_LOG_DBG, "pool %s %.*s...(%zu bytes)", tx ? ">>" : "<<", POOL_TRACE_MAX, line,
                n);

    int pj = core_pool_active();
    if (pj >= 0 && pj < POOL_VALID_NUM) {
        pthread_mutex_lock(&g_stats_lock);
        if (tx) {
            g_pool_st[pj].txcnt++;
            g_pool_st[pj].txbytes += (uint32_t)n + 1;
        } else {
            g_pool_st[pj].rxcnt++;
            g_pool_st[pj].rxbytes += (uint32_t)n;
        }
        pthread_mutex_unlock(&g_stats_lock);
    }

    if (!tx)
        return;
    const char *m = strstr(line, "\"method\":\"");
    if (!m)
        return;
    m += 10;
    const char *e = strchr(m, '"');
    if (!e || e - m > 40 || !strncmp(m, "mining.submit", 13))
        return;
    osif_log(OSIF_LOG_DBG, "Send %.*s for stratum pool %d", (int)(e - m), m, pj);
}

static void session_drop(void)
{
    pthread_mutex_lock(&g_stratum.lock);
    g_stratum.have_job = 0;
    g_stratum.t        = NULL;
    snprintf(g_diff, sizeof g_diff, "1");
    g_dead_seq = g_job_seq;
    pthread_mutex_unlock(&g_stratum.lock);
    g_pending_clean    = 0;
    g_sess_prevhash[0] = '\0';
    pthread_mutex_lock(&g_stratum.send);
    pthread_mutex_unlock(&g_stratum.send);
    pthread_mutex_lock(&g_stats_lock);
    g_ping.inflight = 0;
    pthread_mutex_unlock(&g_stats_lock);
}

uint32_t job_gap_s(void)
{
    uint64_t at = atomic_load(&g_job_at_ms);

    return at ? (uint32_t)((now_ms() - at) / 1000u) : 0u;
}

static int job_fits(const sv1_client *c, int pa)
{
    const char *cb1     = c->job.coinb1 ? c->job.coinb1 : "";
    const char *cb2     = c->job.coinb2 ? c->job.coinb2 : "";
    size_t      n2off   = (strlen(cb1) + strlen(c->xnonce1)) / 2;
    size_t      cb_len  = n2off + (size_t)c->xnonce2_size + strlen(cb2) / 2;
    size_t      cb_tail = cb_len - (n2off - n2off % SHA256_BLOCK_BYTES) + SHA256_BLOCK_BYTES;
    size_t      mk_len  = 0;

    for (size_t i = 0; i < c->job.n_merkle; i++)
        mk_len += (i ? 1 : 0) + strlen(c->job.merkle[i]);
    if (cb_tail > MC_COINBASE_MAX) {
        osif_log(OSIF_LOG_ERR,
                "Pool %d job %s not sent to the ASICs: "
                "coinbase tail %zu bytes (max %d)",
                pa, c->job.job_id, cb_tail, MC_COINBASE_MAX);
        return 0;
    }
    if (strlen(cb1) >= MC_COINB_HEX_LEN) {
        osif_log(OSIF_LOG_ERR,
                "Pool %d job %s not sent to the ASICs: "
                "coinb1 %zu hex chars (max %d)",
                pa, c->job.job_id, strlen(cb1), MC_COINB_HEX_LEN - 1);
        return 0;
    }
    if (c->job.n_merkle > MC_MERKLES_MAX || mk_len >= MC_MERKLE_HEX_LEN) {
        osif_log(OSIF_LOG_ERR,
                "Pool %d job %s not sent to the ASICs: "
                "%zu merkle branches (max %d), merkle list %zu chars (max %d)",
                pa, c->job.job_id, c->job.n_merkle, MC_MERKLES_MAX, mk_len, MC_MERKLE_HEX_LEN - 1);
        return 0;
    }
    if (c->xnonce2_size < NONCE2_SIZE_MIN) {
        osif_log(OSIF_LOG_ERR,
                "Pool %d job %s not sent to the ASICs: "
                "nonce2 size %d (min %d)",
                pa, c->job.job_id, c->xnonce2_size, NONCE2_SIZE_MIN);
        return 0;
    }
    return 1;
}

static void on_authorized(int pa)
{
    osif_log(OSIF_LOG_INFO, "logged in to pool %d, mining", pa);
}

static void session_got_job(core_session_ctx *ctx, int pa)
{
    if (ctx->got_job)
        return;
    ctx->got_job = 1;
    pthread_mutex_lock(&g_stats_lock);
    if (pa >= 0 && pa < POOL_VALID_NUM)
        g_pool_st[pa].dead = 0;
    pthread_mutex_unlock(&g_stats_lock);
}

static uint32_t ping_sample(void)
{
    uint64_t rtt = now_ms() - g_ping.sent_ms;
    uint32_t sum = 0;
    int      cnt = 0;

    g_ping_win[2] = g_ping_win[1];
    g_ping_win[1] = g_ping_win[0];
    g_ping_win[0] = rtt > 60000 ? 0 : (uint32_t)rtt;
    for (int i = 0; i < 3; i++) {
        if (g_ping_win[i] == 0)
            break;
        sum += g_ping_win[i];
        cnt++;
    }
    if (cnt)
        g_ping.ms = sum / (uint32_t)cnt;
    g_ping.inflight = 0;
    return (uint32_t)(rtt / 1000u);
}

static void log_rejected(const sv1_event *ev, const submit_rec *sh, int found, const char *c_h32,
        const char *dd, const char *live_diff, int pa, const mc_tally *tally)
{
    char sub_at[24] = "?", in_flight[24] = "?";
    char c_job[16] = "?", c_age[16] = "?", c_n2[16] = "?";
    char c_nonce[16] = "?", c_roll[8] = "?";

    if (found >= 0) {
        snprintf(sub_at, sizeof sub_at, "%llu.%03llu", (unsigned long long)(sh->at_ms / 1000u),
                (unsigned long long)(sh->at_ms % 1000u));
        snprintf(in_flight, sizeof in_flight, "%llu", (unsigned long long)(now_ms() - sh->at_ms));
        snprintf(c_job, sizeof c_job, "%u", sh->seq);
        snprintf(c_age, sizeof c_age, "%u", sh->age);
        snprintf(c_n2, sizeof c_n2, "%u", sh->n2);
        snprintf(c_nonce, sizeof c_nonce, "%08x", sh->nonce);
        snprintf(c_roll, sizeof c_roll, "%u", sh->roll);
    }
    osif_log(OSIF_LOG_WARN,
            "Rejected %s Diff %s/%s ASC 0 pool %d (%s), "
            "%u accepted / %u rejected so far; code=%d id=%d job=%s "
            "age=%s sub=%s dt=%s n2=%s nonce=%s roll=%s jobdiff=%s%s",
            c_h32, dd, live_diff, pa, ev->err_msg[0] ? ev->err_msg : "no reason given",
            tally->accepted, tally->rejected, ev->err_code, ev->submit_id, c_job, c_age, sub_at,
            in_flight, c_n2, c_nonce, c_roll, found >= 0 ? sh->jdiff : "?",
            ev->submit_lost ? " untracked" : "");
}

static void on_submit_result(core_session_ctx *ctx, const sv1_event *ev, int pa)
{
    pthread_mutex_lock(&g_stratum.lock);
    mc_pipeline_on_result(ctx->pipe, ev->submit_accepted);
    mc_tally tally = ctx->pipe->tally;
    pthread_mutex_unlock(&g_stratum.lock);

    time_t     now   = time(NULL);
    int        found = -1;
    submit_rec sh    = { 0 };
    char       live_diff[32];
    snprintf(live_diff, sizeof live_diff, "%s", g_diff);
    pthread_mutex_lock(&g_stats_lock);
    if (ev->submit_id) {
        for (int i = 0; i < SUBMIT_LOG; i++) {
            if (g_sub[i].id == ev->submit_id) {
                found = i;
                sh    = g_sub[i];
                break;
            }
        }
    }
    double d = atof(found >= 0 ? sh.jdiff : live_diff);
    if (d <= 0)
        d = 1;
    if (pa >= 0 && pa < POOL_VALID_NUM) {
        if (ev->submit_accepted) {
            g_pool_st[pa].acc++;
            g_pool_st[pa].dacc += d;
            g_pool_st[pa].lsdiff = d;
            g_pool_st[pa].ltime  = (uint32_t)now;
        } else {
            g_pool_st[pa].rej++;
            g_pool_st[pa].drej += d;
        }
    }
    uint32_t lag = 0;
    if (g_ping.inflight && g_ping.sent_ms)
        lag = ping_sample();
    if (ev->submit_accepted)
        g_diff_acc += d;
    else
        g_diff_rej += d;
    pthread_mutex_unlock(&g_stats_lock);

    if (lag > 0)
        osif_log(OSIF_LOG_INFO, "pool %d took %u s to answer a share", pa, lag);

    char c_h32[16] = "?", dd[16] = "?";
    if (found >= 0) {
        snprintf(c_h32, sizeof c_h32, "%08x", sh.h32);
        diff_suffix(sh.sdiff, dd, sizeof dd);
    }

    if (!ev->submit_accepted)
        log_rejected(ev, &sh, found, c_h32, dd, live_diff, pa, &tally);
    else
        osif_log(OSIF_LOG_DBG,
                "Accepted %s Diff %s/%s ASC 0 pool %d%s, "
                "%u accepted / %u rejected so far",
                c_h32, dd, live_diff, pa, ev->submit_lost ? " untracked stratum share" : "",
                tally.accepted, tally.rejected);
}

static void merkle_join(const sv1_client *c, char *merkle, size_t cap)
{
    size_t m = 0;

    merkle[0] = '\0';
    for (size_t i = 0; i < c->job.n_merkle; i++) {
        int w = snprintf(merkle + m, cap - m, "%s%s", i ? "," : "", c->job.merkle[i]);
        if (w < 0 || (size_t)w >= cap - m)
            break;
        m += (size_t)w;
    }
}

static int notify_stats(const sv1_client *c, const job_tmpl *snap, int pa, uint32_t height)
{
    uint32_t bver = (uint32_t)strtoul(c->job.version, NULL, 16);
    char     bhash[65];
    mc_block_hash_hex(snap->prevhash, bhash);
    double ndiff = mc_net_diff_from_nbits(snap->nbits);

    pthread_mutex_lock(&g_stats_lock);
    int new_block = strcmp(g_block.hash, bhash) != 0;
    if (new_block) {
        snprintf(g_block.hash, sizeof g_block.hash, "%s", bhash);
        g_block.time = now_mono_s();
        g_block.net_blocks++;
    }
    g_block.net_diff = ndiff;
    if (pa >= 0 && pa < POOL_VALID_NUM) {
        g_pool_st[pa].getworks++;
        g_pool_st[pa].bver   = bver;
        g_pool_st[pa].height = height;
        if (c->job.n_merkle < 2)
            g_pool_st[pa].bad++;
        if (c->difficulty > 0)
            g_pool_st[pa].wdiff = c->difficulty;
    }
    pthread_mutex_unlock(&g_stats_lock);
    return new_block;
}

static void on_notify(core_session_ctx *ctx, const sv1_client *c, int pa)
{
    if (!job_fits(c, pa)) {
        if (c->job.clean_jobs)
            g_pending_clean = 1;
        return;
    }
    const char *ph    = c->job.prevhash ? c->job.prevhash : "";
    int         clean = c->job.clean_jobs || g_pending_clean || strcmp(ph, g_sess_prevhash) != 0;

    char merkle[MC_MERKLE_HEX_LEN];
    merkle_join(c, merkle, sizeof merkle);

    job_tmpl snap;
    pthread_mutex_lock(&g_stratum.lock);
    uint32_t seq = g_job_seq + 1;
    if (seq == 0)
        seq++;
    snprintf(snap.version, sizeof snap.version, "%s", c->job.version);
    snprintf(snap.prevhash, sizeof snap.prevhash, "%s", ph);
    snprintf(snap.nbits, sizeof snap.nbits, "%s", c->job.nbits);
    snprintf(snap.ntime, sizeof snap.ntime, "%s", c->job.ntime);
    mc_job hj = {
        .version = snap.version, .prevhash = snap.prevhash, .ntime = snap.ntime, .nbits = snap.nbits
    };
    snap.hdr_ok = mc_header_tmpl(&hj, snap.hdr) == 0;
    if (!snap.hdr_ok) {
        pthread_mutex_unlock(&g_stratum.lock);
        osif_log(OSIF_LOG_ERR,
                "job %s unverifiable (header would not "
                "build) — its nonces cannot be checked",
                c->job.job_id);
        if (clean)
            g_pending_clean = 1;
        return;
    }
    g_version_mask = c->version_rolling ? c->version_mask : 0;
    snprintf(g_user, sizeof g_user, "%s", ctx->user ? ctx->user : "");
    g_job_seq = seq;
    if (clean)
        g_dead_seq = seq - 1;
    if (c->difficulty > 0)
        snprintf(g_diff, sizeof g_diff, "%.8g", c->difficulty);
    snap.seq          = seq;
    snap.pool         = pa;
    snap.xnonce2_size = c->xnonce2_size;
    snprintf(snap.xnonce1, sizeof snap.xnonce1, "%s", c->xnonce1);
    snprintf(snap.job_id, sizeof snap.job_id, "%s", c->job.job_id);
    snprintf(snap.diff, sizeof snap.diff, "%s", g_diff);
    snprintf(snap.mask, sizeof snap.mask, "%08x", g_version_mask);
    snprintf(snap.coinb1, sizeof snap.coinb1, "%s", c->job.coinb1 ? c->job.coinb1 : "");
    snprintf(snap.coinb2, sizeof snap.coinb2, "%s", c->job.coinb2 ? c->job.coinb2 : "");
    snprintf(snap.merkle, sizeof snap.merkle, "%s", merkle);
    g_ring[seq % JOB_RING] = snap;
    g_stratum.have_job     = 1;
    send_asic_template(&snap);
    pthread_mutex_unlock(&g_stratum.lock);
    g_pending_clean = 0;
    snprintf(g_sess_prevhash, sizeof g_sess_prevhash, "%s", ph);
    atomic_store(&g_job_at_ms, now_ms());
    session_got_job(ctx, pa);

    osif_log(OSIF_LOG_DBG,
            "Pool %d notify job %s seq=%u diff %s "
            "prevhash %.16s ntime %s nbits %s merkles %u clean=%d",
            pa, snap.job_id, seq, g_diff, snap.prevhash, snap.ntime, snap.nbits,
            (unsigned)c->job.n_merkle, clean);

    uint32_t height = mc_coinbase_height(c->job.coinb1);
    if (height)
        height--;
    if (clean) {
        pthread_mutex_lock(&g_stats_lock);
        g_local_work++;
        pthread_mutex_unlock(&g_stats_lock);
    }
    if (notify_stats(c, &snap, pa, height))
        osif_log(OSIF_LOG_INFO, "pool %d: new block, height %u", pa, height);
}

static void on_submit_untracked(const sv1_client *c, const sv1_event *ev)
{
    static fault_report_t untracked_fault; /* untracked stratum result report state */

    uint32_t bucket = c->untracked ? 32u - (uint32_t)__builtin_clz(c->untracked) : 0u;

    if (fault_report_step(&untracked_fault, bucket) != FAULT_REPORT_NONE)
        osif_log(OSIF_LOG_DBG,
                "untracked stratum result id=%d (not a share; "
                "%u untracked, %u stale, %u submits in flight)",
                ev->submit_id, c->untracked, sv1_stale_shares(), sv1_inflight_count());
}

static int on_sv1_event(void *ctx_, sv1_client *c, const sv1_event *ev)
{
    atomic_store(&g_main_beat_ms, now_ms());
    core_session_ctx *ctx = ctx_;
    int               pa  = core_pool_active();
    switch (ev->type) {
    case SV1_EV_CONFIGURED:
        osif_log(OSIF_LOG_INFO, "pool %d version-rolling %s (mask %08x)", pa,
                c->version_rolling ? "granted" : "NOT granted, mining continues without it",
                c->version_rolling ? c->version_mask : 0u);
        break;
    case SV1_EV_SUBSCRIBED:
        osif_log(OSIF_LOG_DBG,
                "Pool %d subscribed with extranonce1 "
                "%s extran2size %d",
                pa, c->xnonce1, c->xnonce2_size);
        break;
    case SV1_EV_AUTHORIZED:
        on_authorized(pa);
        break;
    case SV1_EV_PING:
        osif_log(OSIF_LOG_DBG, "Pool %d ping", pa);
        break;
    case SV1_EV_GET_VERSION:
        osif_log(OSIF_LOG_DBG, "Pool %d asked our version", pa);
        break;
    case SV1_EV_SET_VERSION_MASK:
        osif_log(OSIF_LOG_INFO,
                "pool %d changed the version-rolling mask to %08x "
                "(rolling %s)",
                pa, c->version_mask, c->version_rolling ? "on" : "off");
        break;
    case SV1_EV_SHOW_MESSAGE:
        osif_log(OSIF_LOG_INFO, "message from pool %d: %s", pa, ev->err_msg);
        break;
    case SV1_EV_RECONNECT:
        osif_log(OSIF_LOG_WARN,
                "pool %d asked us to reconnect elsewhere; "
                "ignored, staying on the configured pool",
                pa);
        break;
    case SV1_EV_ERROR:
        osif_log(OSIF_LOG_WARN,
                "pool %d sent a message that could not be "
                "understood; ignored (state %d)",
                pa, c->state);
        break;
    case SV1_EV_SET_DIFFICULTY:
        osif_log(OSIF_LOG_DBG,
                "pool %d difficulty changed to %.8g "
                "from the next job (now %s)",
                pa, c->diff_after > 0 ? c->diff_after : c->next_diff, g_diff);
        break;
    case SV1_EV_SET_EXTRANONCE:
        osif_log(OSIF_LOG_DBG,
                "Pool %d confirmed "
                "mining.extranonce.subscribe with extranonce1 %s extran2size %d",
                pa, c->xnonce1, c->xnonce2_size);
        break;
    case SV1_EV_SUBMIT_RESULT:
        on_submit_result(ctx, ev, pa);
        break;
    case SV1_EV_NOTIFY:
        on_notify(ctx, c, pa);
        break;
    case SV1_EV_SUBMIT_UNTRACKED:
        on_submit_untracked(c, ev);
        break;
    default:
        break;
    }
    if (!ctx->got_job && c->state >= SV1_AUTHORIZED &&
            now_ms() - ctx->start_ms >= STRATUM_RECV_TIMEOUT_S * 1000u) {
        osif_log(OSIF_LOG_WARN,
                "pool %d sent no usable job in %d s; "
                "trying the next pool",
                pa, STRATUM_RECV_TIMEOUT_S);
        return 1;
    }
    return 0;
}

static void pool_note_session_end(pool_set *ps, int idx, int established, int user_drop)
{
    if (user_drop)
        return;
    if (!established)
        pool_mark_dead(ps, idx);
    pthread_mutex_lock(&g_stats_lock);
    if (idx < POOL_VALID_NUM) {
        if (!established)
            g_pool_st[idx].dead = 1;
        else
            g_pool_st[idx].getfail++;
    }
    pthread_mutex_unlock(&g_stats_lock);
}

static int pool_pick(pool_set *ps, const int *prio)
{
    int best = -1;
    for (int i = 0; i < ps->count; i++)
        if (ps->pools[i].enabled && ps->pools[i].alive && (best < 0 || prio[i] < prio[best]))
            best = i;
    ps->active = best;
    return best;
}

void pool_session_once(pool_set *ps, mc_pipeline *pipe)
{
    atomic_store(&g_main_beat_ms, now_ms());
    int prio[POOL_VALID_NUM];
    sync_pool_policy(ps, prio);

    int idx = pool_pick(ps, prio);
    if (idx < 0) {
        for (int i = 0; i < ps->count; i++)
            pool_mark_alive(ps, i);
        osif_sleep_ms((uint32_t)pool_backoff_next(ps));
        return;
    }
    const pool_t *p = &ps->pools[idx];

    char purl[POOL_URL_MAX];
    snprintf(purl, sizeof purl, "%s", p->url);
    int   xnsub = 0;
    char *h     = strchr(purl, '#');
    if (h) {
        xnsub = !strcmp(h, "#xnsub");
        *h    = '\0';
    }

    char host[POOL_URL_MAX];
    int  port = 0;
    if (stratum_parse_url(purl, host, sizeof host, &port) != 0) {
        osif_log(OSIF_LOG_ERR, "pool %d has an invalid URL (%s); skipping it", idx, p->url);
        pool_note_session_end(ps, idx, 0, 0);
        return;
    }

    stratum_transport t;
    int               rc = stratum_tcp_connect(&t, host, port);
    if (rc != STRATUM_CONNECT_OK) {
        /* clang-format off */
        const char *why = rc == STRATUM_CONNECT_RESOLVE ? "name resolution" :
                rc == STRATUM_CONNECT_TIMEOUT ? "connect timed out" :
                strerror(errno);
        /* clang-format on */
        osif_log(OSIF_LOG_WARN, "cannot connect to pool %d (%s:%d): %s; retrying", idx, host, port,
                why);
        pool_note_session_end(ps, idx, 0, 0);
        return;
    }
    osif_log(OSIF_LOG_INFO, "connected to pool %d (%s:%d)", idx, host, port);
    publish_active_pool(idx);

    pthread_mutex_lock(&g_stats_lock);
    if (g_netfail_time[g_netfail_index] != 0) {
        g_netfail_time[g_netfail_index + 1] = (uint32_t)time(NULL);
        g_netfail_index                     = (g_netfail_index + 2) % 8;
    }
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_stratum.lock);
    g_stratum.t         = &t;
    g_stratum.have_job  = 0;
    g_stratum.user_drop = 0;
    pthread_mutex_unlock(&g_stratum.lock);

    int outranked = pool_outranked(idx);

    sv1_client sv1;
    sv1_init(&sv1);
    sv1.extranonce_subscribe = xnsub;
    core_session_ctx ctx     = { .pipe = pipe, .user = p->user, .start_ms = now_ms() };
    int              end     = STRATUM_END_STOPPED;
    if (!outranked) {
        end = stratum_run(&sv1, &t, p->user, p->pass, POOL_USER_AGENT, on_sv1_event, &ctx);
    } else {
        pthread_mutex_lock(&g_stratum.lock);
        g_stratum.user_drop = 1;
        pthread_mutex_unlock(&g_stratum.lock);
    }
    int established = stratum_session_established(&sv1) && ctx.got_job;
    if (established)
        pool_backoff_reset(ps);
    session_drop();
    double dstale = 0;
    pthread_mutex_lock(&g_stratum.lock);
    int      user_drop = g_stratum.user_drop;
    uint32_t cleared   = sv1_clear_inflight(&pipe->sv1, &dstale);
    uint32_t stale     = pipe->sv1.stale;
    pthread_mutex_unlock(&g_stratum.lock);
    if (cleared) {
        pthread_mutex_lock(&g_stats_lock);
        if (idx < POOL_VALID_NUM) {
            g_pool_st[idx].stale += cleared;
            g_pool_st[idx].dstale += dstale;
        }
        pthread_mutex_unlock(&g_stats_lock);
        osif_log(OSIF_LOG_WARN,
                "Lost %u shares due to stratum "
                "disconnect on pool %d (stale total %u)",
                cleared, idx, stale);
    }
    sv1_free(&sv1);

    stratum_tcp_close(&t);
    publish_active_pool(-1);

    pthread_mutex_lock(&g_stats_lock);
    g_netfail_time[g_netfail_index]     = (uint32_t)time(NULL);
    g_netfail_time[g_netfail_index + 1] = 0;
    g_pool_failcnt++;
    pthread_mutex_unlock(&g_stats_lock);

    osif_log(OSIF_LOG_WARN,
            "Stratum connection to pool %d "
            "interrupted: %s — reconnecting",
            idx, stratum_end_str(end));
    pool_note_session_end(ps, idx, established, user_drop);
    if (!user_drop)
        osif_sleep_ms((uint32_t)pool_backoff_next(ps));
}
