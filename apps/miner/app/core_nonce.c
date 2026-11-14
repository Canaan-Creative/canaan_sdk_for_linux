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
#include "core_wallpaper.h"
#include "avalon_gen.h"
#include "driver_avalon.h"
#include "fault_report.h"
#include "work.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PUMP_PERIOD_MS 1000u
/* core stats push period, pump circles */
#define CORE_STATS_CIRCLES   3u
#define NONCE_STATS_PERIOD_S (60 * 60)

typedef struct {
    job_tmpl *t;
    uint32_t  n2;             /* extranonce2 */
    uint32_t  nonce, version; /* nonce and version bits from the ASIC */
    uint32_t  repaired;       /* 1 if extranonce2 was reconstructed */
    uint32_t  retired;
    uint32_t  tries; /* share checks made, 2 if a repair was tried */
    double    share_diff;
    uint32_t  h32;         /* top 32 bits of the share hash */
    char      submit[512]; /* mining.submit line */
} nonce_attempt;

static int      g_sub_w; /* next write index into g_sub */
static uint32_t g_pump_circles;

static mc_share_result try_nonce(const job_tmpl *t, const char *sdiff, uint32_t n2,
        uint32_t true_nonce, uint32_t true_version, uint8_t ntime_off, char *submit_out, size_t cap,
        double *out_sdiff, uint32_t *out_h32)
{
    char xn2s[132], nonce_hex[9], ntime_hex[9], vbits_hex[9];
    mc_xnonce2_hex((uint64_t)n2, t->xnonce2_size, xn2s, sizeof xn2s);
    snprintf(nonce_hex, sizeof nonce_hex, "%08x", true_nonce);
    uint32_t base_ntime = (uint32_t)strtoul(t->ntime, NULL, 16);
    snprintf(ntime_hex, sizeof ntime_hex, "%08x", base_ntime + ntime_off);
    uint32_t job_mask = (uint32_t)strtoul(t->mask, NULL, 16);
    snprintf(vbits_hex, sizeof vbits_hex, "%08x", true_version & job_mask);
    const char *vbits = job_mask ? vbits_hex : NULL;
    mc_job      cj    = {
                .version      = t->version,
                .prevhash     = t->prevhash,
                .coinb1       = t->coinb1,
                .coinb2       = t->coinb2,
                .merkle       = t->merkle,
                .nbits        = t->nbits,
                .ntime        = ntime_hex,
                .xnonce1      = t->xnonce1,
                .xnonce2      = xn2s,
                .nonce        = nonce_hex,
                .version_bits = vbits,
                .mask         = t->mask,
    };
    uint8_t root[32], hdr[80], h_be[32];
    if (!t->hdr_ok || mc_merkle_root(&cj, root) != 0)
        return MC_SHARE_BAD;
    mc_header_apply(hdr, t->hdr, root, base_ntime + ntime_off, true_nonce, true_version & job_mask,
            job_mask);
    mc_header_hash(hdr, h_be);

    if (!mc_meets_target_hash(h_be, sdiff)) {
        if (mc_meets_target_hash(h_be, "1")) {
            if (out_sdiff)
                *out_sdiff = mc_share_diff_hash(h_be);
            if (out_h32)
                *out_h32 = mc_share_hash_h32(h_be);
            return MC_SHARE_ABOVE_TARGET;
        }
        return MC_SHARE_BAD;
    }
    if (out_sdiff)
        *out_sdiff = mc_share_diff_hash(h_be);
    if (out_h32)
        *out_h32 = mc_share_hash_h32(h_be);
    mc_pipeline_set_job(g_stratum.pipe, t->job_id, g_user);
    return mc_pipeline_on_nonce(g_stratum.pipe, xn2s, ntime_hex, nonce_hex, vbits, submit_out, cap);
}

static void on_asic_telem(const void *data, int len)
{
    static fault_report_t telem_len_fault; /* wrong-size MSG_ASIC_TELEM report state */
    int                   bad_len = len != (int)sizeof(asic_chip_telem);
    if (fault_report_step(&telem_len_fault, (uint32_t)bad_len) != FAULT_REPORT_NONE) {
        if (bad_len)
            osif_log(OSIF_LOG_ERR,
                    "MSG_ASIC_TELEM dropped, %d "
                    "bytes, %d expected",
                    len, (int)sizeof(asic_chip_telem));
        else
            osif_log(OSIF_LOG_INFO,
                    "MSG_ASIC_TELEM accepted "
                    "again (%d bytes)",
                    len);
    }
    if (bad_len)
        return;
    pthread_mutex_lock(&g_stats_lock);
    memcpy(&g_chip, data, sizeof g_chip);
    g_chip_at = now_s();
    pthread_mutex_unlock(&g_stats_lock);
}

static uint32_t nonce_lap_len(uint16_t miner_id)
{
    uint32_t mine = 0, active = 0;
    pthread_mutex_lock(&g_stats_lock);
    if (miner_id < HBOARD_COUNT)
        mine = g_chip.chips[miner_id];
    for (int b = 0; b < HBOARD_COUNT; b++)
        if (g_chip.chips[b])
            active++;
    pthread_mutex_unlock(&g_stats_lock);
    uint32_t laplen = mine * active;
    if (laplen == 0 || laplen > (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS)
        laplen = (uint32_t)HBOARD_COUNT * MINER_MAX_CHIPS;
    return laplen;
}

static mc_share_result nonce_repair_same_job(const avalon_nonce_out *no, const char *sdiff,
        uint32_t laplen, nonce_attempt *a)
{
    uint32_t cn2 = no->nonce2 - laplen;
    a->tries++;
    mc_share_result rk = try_nonce(a->t, sdiff, cn2, a->nonce, a->version, no->ntime, a->submit,
            sizeof a->submit, &a->share_diff, &a->h32);
    if (rk == MC_SHARE_DUP)
        return MC_SHARE_DUP;
    if (rk != MC_SHARE_SUBMIT && rk != MC_SHARE_ABOVE_TARGET)
        return MC_SHARE_BAD;
    a->n2       = cn2;
    a->repaired = 1;
    return rk;
}

static mc_share_result nonce_repair_prev_job(const avalon_nonce_out *no, uint32_t laplen,
        nonce_attempt *a)
{
    job_tmpl *prev = &g_ring[(no->job_id - 1u) % JOB_RING];
    if (prev->seq == 0 || prev->seq != no->job_id - 1u)
        return MC_SHARE_BAD;
    if (prev->seq <= g_dead_seq || prev->pool != a->t->pool) {
        a->retired = 1;
        return MC_SHARE_BAD;
    }
    uint32_t cn2 = no->last_job_nonce2 + 1u + no->nonce2 - laplen;
    a->tries++;
    mc_share_result rk = try_nonce(prev, prev->diff, cn2, a->nonce, a->version, no->ntime,
            a->submit, sizeof a->submit, &a->share_diff, &a->h32);
    if (rk == MC_SHARE_DUP)
        return MC_SHARE_DUP;
    if (rk != MC_SHARE_SUBMIT && rk != MC_SHARE_ABOVE_TARGET)
        return MC_SHARE_BAD;
    a->t        = prev;
    a->n2       = cn2;
    a->repaired = 1;
    return rk;
}

static mc_share_result nonce_resolve(const avalon_nonce_out *no, job_tmpl *t, const char *sdiff,
        nonce_attempt *a)
{
    a->t              = t;
    a->n2             = no->nonce2;
    a->repaired       = 0;
    a->retired        = 0;
    a->tries          = 1;
    a->share_diff     = 0;
    a->h32            = 0;
    mc_share_result r = try_nonce(t, sdiff, no->nonce2, a->nonce, a->version, no->ntime, a->submit,
            sizeof a->submit, &a->share_diff, &a->h32);
    if (r != MC_SHARE_BAD)
        return r;
    uint32_t laplen = nonce_lap_len(no->miner_id);
    return no->nonce2 >= laplen ? nonce_repair_same_job(no, sdiff, laplen, a) :
                                  nonce_repair_prev_job(no, laplen, a);
}

static void nonce_credit_ok(const avalon_nonce_out *no, const nonce_attempt *a, int pj,
        const char *sdiff)
{
    double dd = atof(sdiff);
    if (!(dd > 0))
        dd = 1;
    if (dd > (double)MC_ASIC_DIFF_MAX)
        dd = (double)MC_ASIC_DIFF_MAX;
    pthread_mutex_lock(&g_stats_lock);
    if (no->miner_id < HBOARD_COUNT && no->asic_id < MINER_MAX_CHIPS)
        g_mw[no->miner_id][no->asic_id]++;
    g_diff1 += dd;
    if (no->miner_id < HBOARD_COUNT)
        g_board_diff1[no->miner_id] += dd;
    if (pj >= 0 && pj < POOL_VALID_NUM) {
        g_pool_st[pj].diff1 += dd;
        if (a->share_diff > g_pool_st[pj].best)
            g_pool_st[pj].best = a->share_diff;
    }
    if (!g_dev_start)
        g_dev_start = now_s();
    g_last_valid_work = (uint32_t)time(NULL);
    win_add_locked(now_s(), dd);
    pthread_mutex_unlock(&g_stats_lock);
}

static void submit_rec_add_locked(const avalon_nonce_out *no, const nonce_attempt *a,
        const char *sdiff)
{
    int w          = g_sub_w++ % SUBMIT_LOG;
    g_sub[w].id    = g_stratum.pipe->sv1.next_id - 1;
    g_sub[w].seq   = a->t->seq;
    g_sub[w].n2    = a->n2;
    g_sub[w].nonce = a->nonce;
    g_sub[w].roll  = no->ntime;
    g_sub[w].age   = g_job_seq - a->t->seq;
    g_sub[w].at_ms = now_ms();
    g_sub[w].h32   = a->h32;
    g_sub[w].sdiff = a->share_diff;
    snprintf(g_sub[w].jdiff, sizeof g_sub[w].jdiff, "%s", sdiff);
}

static void nonce_submit(const avalon_nonce_out *no, const nonce_attempt *a, int pj,
        const char *sdiff)
{
    stratum_transport *t   = g_stratum.t;
    uint32_t           seq = a->t->seq;
    int                id  = g_stratum.pipe->sv1.next_id - 1;
    char               job_id[sizeof a->t->job_id], jdiff[32];
    snprintf(job_id, sizeof job_id, "%s", a->t->job_id);
    snprintf(jdiff, sizeof jdiff, "%s", sdiff);
    sv1_inflight_set_diff(id, atof(sdiff));

    pthread_mutex_lock(&g_stats_lock);
    submit_rec_add_locked(no, a, sdiff);
    if (a->repaired)
        g_nst.hit_repaired++;
    else
        g_nst.hit_direct++;
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_stratum.send);
    pthread_mutex_unlock(&g_stratum.lock);
    stratum_trace_line(1, a->submit);
    int serr = t->send_line(t->io, a->submit);
    pthread_mutex_lock(&g_stats_lock);
    if (serr == 0 && !g_ping.inflight) {
        g_ping.sent_ms  = now_ms();
        g_ping.inflight = 1;
    }
    if (pj >= 0 && pj < POOL_VALID_NUM && serr != 0)
        g_pool_st[pj].rfail++;
    pthread_mutex_unlock(&g_stats_lock);
    pthread_mutex_unlock(&g_stratum.send);
    pthread_mutex_lock(&g_stratum.lock);

    osif_log(OSIF_LOG_DBG,
            "Submitting share id=%d job=%s diff=%s ntime_roll=%u "
            "(nonce HIT job=%u n2=%u nonce=%08x asic=%u/%u repaired=%u%s)%s",
            id, job_id, jdiff, no->ntime, seq, a->n2, a->nonce, no->miner_id, no->asic_id,
            a->repaired, a->repaired ? ", nonce2 reconstructed" : "", serr ? " — SEND FAILED" : "");
}

static void nonce_charge_hw(const avalon_nonce_out *no, uint32_t *cause)
{
    pthread_mutex_lock(&g_stats_lock);
    g_nst.hw_errors++;
    if (no->miner_id < HBOARD_COUNT)
        g_nst.hw_board[no->miner_id]++;
    (*cause)++;
    pthread_mutex_unlock(&g_stats_lock);
}

static void nonce_retired(const avalon_nonce_out *no)
{
    pthread_mutex_lock(&g_stats_lock);
    g_nst.retired++;
    pthread_mutex_unlock(&g_stats_lock);
    osif_log(OSIF_LOG_DBG,
            "nonce for a retired job seq=%u "
            "asic=%u/%u (dead<=%u, cur=%u, dropped)",
            no->job_id, no->miner_id, no->asic_id, g_dead_seq, g_job_seq);
}

static void nonce_live_job(const avalon_nonce_out *no, job_tmpl *t)
{
    nonce_attempt a;
    a.nonce           = __builtin_bswap32(no->nonce);
    a.version         = __builtin_bswap32(no->version);
    mc_share_result r = nonce_resolve(no, t, t->diff, &a);
    pthread_mutex_lock(&g_stats_lock);
    g_local_work += 2u * a.tries;
    if (t->pool >= 0 && t->pool < POOL_VALID_NUM)
        g_pool_st[t->pool].works++;
    pthread_mutex_unlock(&g_stats_lock);
    int pj = a.t->pool;
    if (r == MC_SHARE_SUBMIT || r == MC_SHARE_ABOVE_TARGET)
        nonce_credit_ok(no, &a, pj, a.t->diff);

    switch (r) {
    case MC_SHARE_SUBMIT:
        nonce_submit(no, &a, pj, a.t->diff);
        break;
    case MC_SHARE_DUP:
        osif_log(OSIF_LOG_DBG,
                "nonce DUP: duplicate nonce for job %u, "
                "counted as a duplicate hardware error",
                no->job_id);
        break;
    case MC_SHARE_ABOVE_TARGET:
        pthread_mutex_lock(&g_stats_lock);
        g_nst.low_diff++;
        pthread_mutex_unlock(&g_stats_lock);
        osif_log(OSIF_LOG_DBG,
                "ASC 0: Share above target "
                "(job=%u, dropped)",
                no->job_id);
        break;
    default:
        if (a.retired) {
            nonce_retired(no);
            break;
        }
        nonce_charge_hw(no, &g_nst.hw_verify);
        osif_log(OSIF_LOG_DBG,
                "nonce verify FAIL job=%u n2=%u last=%u ntime=%u "
                "asic=%u/%u nonce=%08x -> HW",
                no->job_id, no->nonce2, no->last_job_nonce2, no->ntime, no->miner_id, no->asic_id,
                a.nonce);
        break;
    }
}

static void nonce_aging(void)
{
    pthread_mutex_lock(&g_stats_lock);
    g_nst.aging++;
    pthread_mutex_unlock(&g_stats_lock);
}

static void nonce_miss(const avalon_nonce_out *no, const job_tmpl *t)
{
    int nosession  = (g_stratum.t == NULL);
    int impossible = !nosession && no->job_id > g_job_seq;
    /* clang-format off */
    nonce_charge_hw(no,
            nosession ? &g_nst.hw_nosession :
            impossible ? &g_nst.hw_impossible :
            &g_nst.hw_nomatch);
    osif_log(OSIF_LOG_DBG,
            "nonce %s job seq=%u asic=%u/%u "
            "(slot=%u holds seq=%u, cur=%u, dead=%u) -> HW",
            nosession ? "arrived with no pool session" :
            impossible ? "no-match/impossible" :
            "no-match/stale",
            no->job_id, no->miner_id, no->asic_id, (unsigned)(no->job_id % JOB_RING), t->seq,
            g_job_seq, g_dead_seq);
    /* clang-format on */
}

static void on_asic_nonce(const avalon_nonce_out *no)
{
    pthread_mutex_lock(&g_stats_lock);
    g_nst.rx++;
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_stratum.lock);
    job_tmpl *t       = &g_ring[no->job_id % JOB_RING];
    int       matched = t->seq != 0 && t->seq == no->job_id;
    if (g_stratum.pipe && g_stratum.t && matched && no->job_id > g_dead_seq)
        nonce_live_job(no, t);
    else if (matched)
        nonce_retired(no);
    else if (no->job_id == STATIC_WORK_JOB_ID)
        nonce_aging();
    else
        nonce_miss(no, t);
    pthread_mutex_unlock(&g_stratum.lock);
}

void nonce_cb(uint32_t type, const void *data, int len)
{
    if (type == MSG_ASIC_TELEM && data) {
        on_asic_telem(data, len);
        return;
    }
    if (type != MSG_ASIC_NONCE || !data || len != (int)sizeof(avalon_nonce_out))
        return;
    avalon_nonce_out no;
    memcpy(&no, data, sizeof no);
    if (no.valid)
        on_asic_nonce(&no);
}

static void nonce_ledger_tick(void)
{
    static time_t last; /* last nonce stats log, s */
    time_t        now = now_s();
    if (!last)
        last = now;
    if (now - last < NONCE_STATS_PERIOD_S)
        return;
    last = now;
    pthread_mutex_lock(&g_stats_lock);
    uint32_t rx = g_nst.rx, bt = g_nst.low_diff, hits = g_nst.hit_direct + g_nst.hit_repaired,
             hw    = g_nst.hw_verify + g_nst.hw_nosession + g_nst.hw_nomatch + g_nst.hw_impossible,
             stale = g_nst.retired + g_nst.aging;
    uint32_t dc    = g_chip.nonce_discarded;
    dc             = dc >= g_nst.disc_base ? dc - g_nst.disc_base : dc;
    pthread_mutex_unlock(&g_stats_lock);
    pthread_mutex_lock(&g_stratum.lock);
    uint32_t dups = g_stratum.pipe ? g_stratum.pipe->dups : 0u;
    pthread_mutex_unlock(&g_stratum.lock);
    osif_log(OSIF_LOG_INFO,
            "nonce stats: %u nonces, %u sent, %u lowdiff, "
            "%u stale, %u dup, %u hw, %u drop",
            rx, hits, bt, stale, dups, hw, dc);
}

void send_asic_template(const job_tmpl *t)
{
    mc_asic_job tp;
    memset(&tp, 0, sizeof tp);
    tp.seq          = t->seq;
    tp.n2_base      = 0;
    tp.xnonce2_size = t->xnonce2_size;
    mc_asic_target(t->diff, &tp.target_h, &tp.target_l);
    memcpy(tp.version, t->version, sizeof tp.version);
    memcpy(tp.prevhash, t->prevhash, sizeof tp.prevhash);
    memcpy(tp.ntime, t->ntime, sizeof tp.ntime);
    memcpy(tp.nbits, t->nbits, sizeof tp.nbits);
    memcpy(tp.xnonce1, t->xnonce1, sizeof tp.xnonce1);
    memcpy(tp.mask, t->mask, sizeof tp.mask);
    memcpy(tp.coinb1, t->coinb1, sizeof tp.coinb1);
    memcpy(tp.coinb2, t->coinb2, sizeof tp.coinb2);
    memcpy(tp.merkle, t->merkle, sizeof tp.merkle);
    osif_ipc_send(g_ipc_asic_core, MSG_ASIC_JOB, &tp, (int)sizeof tp);
}

static void pump_circle(void)
{
    nonce_ledger_tick();
    if (g_pump_circles++ % CORE_STATS_CIRCLES == 0) {
        tzset();
        core_stats_push();
    }
    core_wallpaper_tick();

    pthread_mutex_lock(&g_stratum.lock);
    if (g_stratum.have_job && g_job_seq != 0) {
        const job_tmpl *slot = &g_ring[g_job_seq % JOB_RING];
        if (slot->seq == g_job_seq)
            send_asic_template(slot);
    }
    pthread_mutex_unlock(&g_stratum.lock);
}

void *pump_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pump_circle();
        osif_sleep_ms(PUMP_PERIOD_MS);
    }
    return NULL;
}
