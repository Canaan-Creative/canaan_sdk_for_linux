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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osif.h"
#include "boardconf.h"
#include "miner_ipc.h"

#include "../../tests/include/tst_check.h"

#define WORK_FREQ 297 /* Work frequency the retune target is offset from, MHz */

#define ENUM_N      2     /* Chips enumerated on each board */
#define VF_PASS_CAP 20000 /* Loop ticks after which the run stops */
/* Loop ticks run after the repeated MSG_ASIC_VF */
#define REPEAT_PASSES (10 * (int)HBOARD_COUNT + 10)

/* Retune frequency sent in MSG_ASIC_VF, MHz */
#define VF_TARGET ((uint16_t)((unsigned)WORK_FREQ + 10u))

extern int stub_enum_chips;
extern int stub_genwork_ok;                    /* Non-zero makes stub work generation succeed */
extern int stub_pll_commit_n, stub_pll_freq_n; /* Stub PLL commits and a3197s_set_freq() calls */
extern int stub_seq_n;                         /* Stub PLL commits, including those not logged */
extern uint32_t stub_seq_base[];               /* Base frequency of each logged stub PLL commit */
extern uint8_t  stub_seq_board[];              /* Board of each logged stub PLL commit */
extern int      stub_nonce_arm_n[]; /* Stub a3197s_enable_nonce_return() calls per board */
extern int      stub_work_n;        /* Stub avalon_send_work() calls */

static uint64_t  tst_now_ms;        /* Simulated clock returned by osif_now_ms(), ms */
static char      tst_err_log[256];  /* Text of the last OSIF_LOG_ERR message */
static int       tst_err_n;         /* OSIF_LOG_ERR messages logged */
static int       s_loop_live;       /* Non-zero once the main loop has started */
osif_ipc_recv_cb tst_registered_cb; /* IPC receive callback registered by the product */
static int       s_telem_sends;     /* MSG_ASIC_TELEM messages sent */

static int s_lock_obj; /* Object whose address is the only lock handed out */
/* Lock acquires, releases, current depth, misuses, osif_lock_create() calls */
static int s_lock_acq, s_lock_rel, s_lock_depth, s_lock_bad, s_lock_create_n;
static int s_lock_create_fail; /* Non-zero makes osif_lock_create() fail */

static int s_passes; /* Loop ticks */
/* Ticks with board 0 at its commanded points, no board armed and all PLL-pending */
static int s_pending_seen_unarmed;
static int s_phase;         /* Phase: 0 waiting for the retune, 1 after the repeated MSG_ASIC_VF */
static int s_repeat_passes; /* Ticks in phase 1 */
/* stub_pll_commit_n when the retune applied on every board; -1 before */
static int s_commits_at_repeat = -1;
/* PLL-pending boards when the retune applied on every board; -1 before */
static int s_pending_after_retune = -1;
/* PLL-pending boards after the repeated MSG_ASIC_VF; -1 before */
static int s_pending_after_repeat_cmd = -1;

uint64_t osif_now_ms(void)
{
    return tst_now_ms;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    if (lvl != OSIF_LOG_ERR)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tst_err_log, sizeof tst_err_log, fmt, ap);
    va_end(ap);
    tst_err_n++;
}
int fan_dev_get_rpm(int fan)
{
    (void)fan;
    return 0;
}

static void tst_loop_enter(void);
static void tst_loop_tick(void);

void osif_sleep_ms(uint32_t ms)
{
    tst_now_ms += ms;
    if (s_loop_live)
        tst_loop_tick();
}

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    extern osif_ipc_recv_cb tst_registered_cb;
    tst_registered_cb = cb;
    s_loop_live       = 1;
    tst_loop_enter();
    return OSIF_OK;
}
void osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)data;
    (void)len;
    if (type == MSG_ASIC_TELEM)
        s_telem_sends++;
    return OSIF_OK;
}
osif_status_t osif_lock_create(osif_lock_t **l)
{
    s_lock_create_n++;
    if (s_lock_create_fail)
        return OSIF_ERR;
    if (l)
        *l = (osif_lock_t *)&s_lock_obj;
    return OSIF_OK;
}
void osif_lock_acquire(osif_lock_t *l)
{
    if (l != (osif_lock_t *)&s_lock_obj || s_lock_depth)
        s_lock_bad++;
    s_lock_depth++;
    s_lock_acq++;
}
void osif_lock_release(osif_lock_t *l)
{
    if (l != (osif_lock_t *)&s_lock_obj || !s_lock_depth)
        s_lock_bad++;
    s_lock_depth--;
    s_lock_rel++;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static void fill_job(mc_asic_job *j, unsigned char seed)
{
    unsigned char *p = (unsigned char *)j;
    for (size_t i = 0; i < sizeof *j; i++)
        p[i] = (unsigned char)(seed + i);
}

static void expect_asic_clamp(mc_asic_job *j)
{
    j->version[sizeof j->version - 1]   = 0;
    j->prevhash[sizeof j->prevhash - 1] = 0;
    j->ntime[sizeof j->ntime - 1]       = 0;
    j->nbits[sizeof j->nbits - 1]       = 0;
    j->xnonce1[sizeof j->xnonce1 - 1]   = 0;
    j->mask[sizeof j->mask - 1]         = 0;
    j->coinb1[sizeof j->coinb1 - 1]     = 0;
    j->coinb2[sizeof j->coinb2 - 1]     = 0;
    j->merkle[sizeof j->merkle - 1]     = 0;
}

static void clear_inbound(void)
{
    memset(&g_job, 0, sizeof g_job);
    g_job_cnt = 0;
    for (int i = 0; i < ASIC_PLL_POINTS; i++)
        g_pll_freq[i] = 0;
    g_pll_at_target = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        g_bd[b].pll_pending = g_bd[b].target_pending = 0;
    s_lock_acq = s_lock_rel = s_lock_depth = s_lock_bad = 0;
}

static int pending_count(void)
{
    int n = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        if (g_bd[b].pll_pending)
            n++;
    return n;
}

static int pending_target_count(void)
{
    int n = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        if (g_bd[b].target_pending)
            n++;
    return n;
}
_Static_assert(ENUM_N <= (int)MINER_MAX_CHIPS, "case 8 chain longer than the descriptor's");
_Static_assert(VF_TARGET != (uint16_t)WORK_FREQ, "the retune target must differ");

static void tst_loop_enter(void)
{
    memset(&g_job, 0, sizeof g_job);
    g_job.seq      = 1;
    g_job.n2_base  = 0x2000u;
    g_job.target_h = 0xa5a5a5a5u;
    g_job.target_l = 0x5a5a5a5au;
    g_job_cnt++;
    stub_genwork_ok = 1;
    mc_asic_vf v    = { .freq = { VF_TARGET, VF_TARGET, VF_TARGET, VF_TARGET }, .at_target = 1 };
    ipc_cb(MSG_ASIC_VF, &v, (int)sizeof v);
}

static void tst_loop_tick(void)
{
    if (s_phase == 0) {
        int armed = 0, applied = 0;
        for (int b = 0; b < HBOARD_COUNT; b++) {
            if (stub_nonce_arm_n[b] > 0)
                armed++;
            if (g_bd[b].work_freq == VF_TARGET && g_bd[b].ri_sta &&
                    !memcmp(g_bd[b].cmd_pts, g_bd[b].pll_pts, sizeof g_bd[b].cmd_pts))
                applied++;
        }
        if (!memcmp(g_bd[0].cmd_pts, g_bd[0].pll_pts, sizeof g_bd[0].cmd_pts) && armed == 0 &&
                pending_count() == (int)HBOARD_COUNT)
            s_pending_seen_unarmed++;
        if (applied == (int)HBOARD_COUNT) {
            s_pending_after_retune = pending_count();
            s_commits_at_repeat    = stub_pll_commit_n;
            mc_asic_vf same        = { .freq = { VF_TARGET, VF_TARGET, VF_TARGET, VF_TARGET },
                       .at_target            = 1 };
            ipc_cb(MSG_ASIC_VF, &same, (int)sizeof same);
            s_pending_after_repeat_cmd = pending_count();
            s_phase                    = 1;
        }
    } else if (++s_repeat_passes >= REPEAT_PASSES) {
        g_run = 0;
    }
    if (++s_passes >= VF_PASS_CAP)
        g_run = 0;
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chip(s)/board, "
           "work freq %u -> retune %u MHz\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, (unsigned)WORK_FREQ,
            (unsigned)VF_TARGET);

    mc_asic_job job;

    osif_lock_create(&g_ipc_lock);
    CHECK(g_ipc_lock != NULL,
            "the template lock exists — without it every bracketing assertion "
            "below is satisfied by the calls being compiled out");

    clear_inbound();
    fill_job(&job, 0x11);
    ipc_cb(MSG_ASIC_JOB, &job, (int)sizeof job);
    expect_asic_clamp(&job);
    CHECK(memcmp(&g_job, &job, sizeof job) == 0,
            "a full-length MSG_ASIC_JOB is copied byte-for-byte outside the "
            "8 NUL-clamped fields (%zu bytes)",
            sizeof job);
    CHECK(g_job_cnt == 1, "...and latches the template (cnt %u)", (unsigned)g_job_cnt);
    CHECK(s_lock_acq == 1 && s_lock_rel == 1 && s_lock_bad == 0,
            "...with the copy bracketed by exactly one lock/unlock (%d/%d, %d "
            "misuse) — the scan loop reads this struct from the other thread",
            s_lock_acq, s_lock_rel, s_lock_bad);
    CHECK(pending_target_count() == (int)HBOARD_COUNT,
            "...and the first job marks every board's target pending (%d)", pending_target_count());

    {
        extern int stub_target_fail;
        s_lock_acq = s_lock_rel = 0;
        stub_target_fail        = 1;
        int r                   = asic_set_target(0);
        stub_target_fail        = 0;
        CHECK(r == 0 && g_bd[0].target_pending == 1 && s_lock_acq == 2 && s_lock_rel == 2 &&
                        s_lock_bad == 0,
                "a target write that fails re-arms the board's pending flag, "
                "clear and re-arm each under the lock (ret %d, pending %d, "
                "%d/%d)",
                r, (int)g_bd[0].target_pending, s_lock_acq, s_lock_rel);
        s_lock_acq = s_lock_rel = 0;
        r                       = asic_set_target(0);
        CHECK(r == 1 && g_bd[0].target_pending == 0 && s_lock_acq == 1 && s_lock_rel == 1 &&
                        s_lock_bad == 0,
                "...and the retry clears it under one lock (ret %d, pending %d, "
                "%d/%d)",
                r, (int)g_bd[0].target_pending, s_lock_acq, s_lock_rel);
    }

    clear_inbound();
    fill_job(&job, 0x11);
    ipc_cb(MSG_ASIC_JOB, &job, (int)sizeof job);
    expect_asic_clamp(&job);
    {
        mc_asic_job bad;
        fill_job(&bad, 0x77);
        for (int trunc = 1; trunc <= 3; trunc++) {
            int len    = (int)sizeof bad - trunc;
            s_lock_acq = s_lock_rel = 0;
            ipc_cb(MSG_ASIC_JOB, &bad, len);
            CHECK(memcmp(&g_job, &job, sizeof job) == 0,
                    "a %d-byte-short MSG_ASIC_JOB leaves the last good template "
                    "untouched",
                    trunc);
            CHECK(s_lock_acq == 0 && s_lock_rel == 0, "...and does not even take the lock (%d/%d)",
                    s_lock_acq, s_lock_rel);
        }
        ipc_cb(MSG_ASIC_JOB, &bad, 0);
        ipc_cb(MSG_ASIC_JOB, NULL, (int)sizeof bad);
        CHECK(memcmp(&g_job, &job, sizeof job) == 0,
                "a zero-length and a NULL-payload job are both rejected too");
    }

    clear_inbound();
    {
        struct {
            mc_asic_job   j;
            unsigned char tail[64];
        } wide;
        mc_asic_job zero;
        memset(&zero, 0, sizeof zero);
        fill_job(&wide.j, 0x33);
        memset(wide.tail, 0xEE, sizeof wide.tail);
        uint32_t cnt   = g_job_cnt;
        tst_err_n      = 0;
        tst_err_log[0] = 0;
        ipc_cb(MSG_ASIC_JOB, &wide, (int)sizeof wide);
        CHECK(memcmp(&g_job, &zero, sizeof zero) == 0 && g_job_cnt == cnt && s_lock_acq == 0,
                "an over-long MSG_ASIC_JOB (%zu bytes) is dropped: no template, "
                "no latch, no new job count, no lock",
                sizeof wide);
        CHECK(tst_err_n == 1 && strstr(tst_err_log, "MSG_ASIC_JOB"),
                "...and logged as an ERR (%d line(s), \"%s\")", tst_err_n, tst_err_log);
        ipc_cb(MSG_ASIC_JOB, &wide, (int)sizeof wide);
        CHECK(tst_err_n == 1,
                "...a second job of the same wrong length is not logged again "
                "(%d line(s))",
                tst_err_n);
        ipc_cb(MSG_ASIC_JOB, &wide, (int)sizeof wide - 1);
        CHECK(tst_err_n == 2 && g_job_cnt == cnt,
                "...a different wrong length is logged again (%d line(s))", tst_err_n);
        fill_job(&job, 0x44);
        ipc_cb(MSG_ASIC_JOB, &job, (int)sizeof job);
        expect_asic_clamp(&job);
        CHECK(tst_err_n == 2 && g_job_cnt == cnt + 1u && memcmp(&g_job, &job, sizeof job) == 0,
                "...and the next exact-length job is accepted and bumps the "
                "job count (cnt %u, was %u)",
                (unsigned)g_job_cnt, (unsigned)cnt);
        ipc_cb(MSG_ASIC_JOB, &wide, (int)sizeof wide - 1);
        CHECK(tst_err_n == 3,
                "...a good job resets the dedup: the same wrong length is logged "
                "again (%d line(s))",
                tst_err_n);
    }

    clear_inbound();
    {
        stub_genwork_ok = 1;
        fill_job(&job, 0x21);
        ipc_cb(MSG_ASIC_JOB, &job, (int)sizeof job);
        s_lock_acq = s_lock_rel = 0;
        asic_send_work(0);
        asic_send_work(0);
        asic_send_work(0);
        CHECK(s_lock_acq == 1 && s_lock_rel == 1 && s_lock_bad == 0,
                "three dispatches under one job copy the template once, under the "
                "lock (%d/%d)",
                s_lock_acq, s_lock_rel);
        CHECK(memcmp(&g_cur_job, &g_job, sizeof g_job) == 0 && g_last_job_cnt == g_job_cnt,
                "...and the copy is the template of the current job");
        fill_job(&job, 0x22);
        ipc_cb(MSG_ASIC_JOB, &job, (int)sizeof job);
        s_lock_acq = s_lock_rel = 0;
        asic_send_work(0);
        CHECK(s_lock_acq == 1 && memcmp(&g_cur_job, &g_job, sizeof g_job) == 0,
                "a new job is copied on the next dispatch (%d lock(s))", s_lock_acq);
    }

    clear_inbound();
    {
        mc_asic_job clean;
        memset(&clean, 0xAA, sizeof clean);
        ipc_cb(MSG_ASIC_JOB, &clean, (int)sizeof clean);
        CHECK(g_job.version[sizeof g_job.version - 1] == 0 &&
                        g_job.prevhash[sizeof g_job.prevhash - 1] == 0 &&
                        g_job.ntime[sizeof g_job.ntime - 1] == 0 &&
                        g_job.nbits[sizeof g_job.nbits - 1] == 0 &&
                        g_job.xnonce1[sizeof g_job.xnonce1 - 1] == 0 &&
                        g_job.mask[sizeof g_job.mask - 1] == 0 &&
                        g_job.coinb1[sizeof g_job.coinb1 - 1] == 0 &&
                        g_job.coinb2[sizeof g_job.coinb2 - 1] == 0 &&
                        g_job.merkle[sizeof g_job.merkle - 1] == 0,
                "a job with no NUL anywhere in its 9 hex fields still gets each "
                "field's last byte clamped to 0");
    }

    clear_inbound();
    {
        mc_asic_vf v = { .freq = { 321, 321, 321, 321 } };
        ipc_cb(MSG_ASIC_VF, &v, (int)sizeof v);
        CHECK(g_pll_freq[0] == 321, "MSG_ASIC_VF records the target (got %u)",
                (unsigned)g_pll_freq[0]);
        CHECK(pending_count() == (int)HBOARD_COUNT, "...and marks all %d board(s) pending (got %d)",
                (int)HBOARD_COUNT, pending_count());
        CHECK(g_job_cnt == 0, "...without touching the job latch (cnt %u)", (unsigned)g_job_cnt);
    }

    {
        mc_asic_vf z = { .freq = { 0, 0, 0, 0 } };
        ipc_cb(MSG_ASIC_VF, &z, (int)sizeof z);
        CHECK(g_pll_freq[0] == 321 && pending_count() == (int)HBOARD_COUNT,
                "a 0 MHz target changes neither the recorded target (%u) nor the "
                "pending set (%d) — it means 'unchanged', not 'stop the clocks'",
                (unsigned)g_pll_freq[0], pending_count());
    }

    clear_inbound();
    {
        mc_asic_vf     v = { .freq = { 400, 400, 400, 400 } };
        static uint8_t big[sizeof(mc_asic_vf) + 4];
        memcpy(big, &v, sizeof v);
        ipc_cb(MSG_ASIC_VF, &v, (int)sizeof v - 1);
        ipc_cb(MSG_ASIC_VF, big, (int)sizeof big);
        ipc_cb(MSG_ASIC_VF, NULL, (int)sizeof v);
        CHECK(g_pll_freq[0] == 0 && pending_count() == 0,
                "a short, long or NULL MSG_ASIC_VF is rejected (target %u, %d pending)",
                (unsigned)g_pll_freq[0], pending_count());
    }

    {
        mm_efuse_state st = { .present = 1, .bin_grade = 0x5a };
        static uint8_t big[sizeof(mm_efuse_state) + 4];
        memcpy(big, &st, sizeof st);
        g_bin_grade = 0;
        g_efuse_req = 0;
        ipc_cb(MSG_MM_EFUSE, big, (int)sizeof big);
        ipc_cb(MSG_MM_EFUSE, &st, (int)sizeof st - 1);
        CHECK(g_bin_grade == 0 && g_efuse_req == 0,
                "a long or short MSG_MM_EFUSE is rejected (bin 0x%02x, req %d)",
                (unsigned)g_bin_grade, (int)g_efuse_req);
        ipc_cb(MSG_MM_EFUSE, &st, (int)sizeof st);
        CHECK(g_bin_grade == 0x5a, "...and a full-length one is applied (0x%02x)",
                (unsigned)g_bin_grade);
        g_bin_grade = 0;
    }

    clear_inbound();
    fill_job(&job, 0x55);
    {
        mc_asic_job before = g_job;
        ipc_cb(MSG_ASIC_TELEM, &job, (int)sizeof job);
        ipc_cb(MSG_ASIC_NONCE, &job, (int)sizeof job);
        ipc_cb(0xDEADu, &job, (int)sizeof job);
        CHECK(memcmp(&g_job, &before, sizeof before) == 0 && g_job_cnt == 0 && g_pll_freq[0] == 0 &&
                        pending_count() == 0,
                "three unrecognised types leave the template, the latch, the "
                "target and the pending set alone");
    }

    clear_inbound();
    {
        int n0             = s_lock_create_n;
        s_lock_create_fail = 1;
        tst_registered_cb  = NULL;
        int r              = asic_miner_main_unused();
        s_lock_create_fail = 0;
        CHECK(r == 1 && s_lock_create_n == n0 + 1 && tst_registered_cb == NULL,
                "a lock that cannot be created ends asic_miner before any "
                "channel opens (ret %d)",
                r);
    }

    stub_enum_chips   = ENUM_N;
    stub_pll_commit_n = stub_pll_freq_n = stub_seq_n = 0;
    stub_work_n                                      = 0;
    memset(stub_nonce_arm_n, 0, sizeof(int) * 8);
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].ri_sta = g_bd[bd].stage = 0;
    s_loop_live = 0;
    s_passes = s_repeat_passes = 0;
    s_phase                    = 0;
    s_pending_seen_unarmed     = 0;
    tst_registered_cb          = NULL;
    g_run                      = 1;
    s_telem_sends              = 0;
    uint64_t t0                = tst_now_ms;
    (void)asic_miner_main_unused();
    s_loop_live         = 0;
    const uint64_t span = tst_now_ms - t0;

    CHECK(tst_registered_cb == ipc_cb,
            "main() registers ipc_cb on the datapath channel — without this every "
            "case above is a statement about a function nothing calls");
    CHECK(s_lock_create_n >= 2 && g_ipc_lock != NULL,
            "...and creates the template lock itself (%d create call(s), handle "
            "%s) — the RX thread and the scan loop share that struct",
            s_lock_create_n, g_ipc_lock ? "set" : "NULL");
    CHECK(s_passes < VF_PASS_CAP, "the loop applied the retune on every board (%d passes, cap %d)",
            s_passes, VF_PASS_CAP);
    CHECK(s_pending_seen_unarmed == 0,
            "...taking the target on the pass it ARRIVES, whatever RI is doing "
            "(%d pass(es) still carried a pending flag) — the command sets the "
            "ramp target and pll_step walks to it; it is not held back "
            "until nonce return is armed",
            s_pending_seen_unarmed);
    {
        int at_target = 0, armed = 0;
        for (int b = 0; b < HBOARD_COUNT; b++) {
            if (g_bd[b].work_freq == VF_TARGET)
                at_target++;
            if (stub_nonce_arm_n[b] > 0)
                armed++;
        }
        CHECK(armed == (int)HBOARD_COUNT, "every board reached work mode (%d of %d)", armed,
                (int)HBOARD_COUNT);
        CHECK(at_target == (int)HBOARD_COUNT,
                "...and every board's shadow moved to %u MHz (%d of %d)", (unsigned)VF_TARGET,
                at_target, (int)HBOARD_COUNT);
        CHECK(s_pending_after_retune == 0,
                "...with the pending flag cleared by the apply (%d left) — a flag "
                "that survived would re-commit the PLL every pass forever",
                s_pending_after_retune);
    }
    {
        int at_target = 0;
        for (int i = 0; i < stub_seq_n && i < 2048; i++)
            if (stub_seq_base[i] == (uint32_t)VF_TARGET)
                at_target++;
        CHECK(at_target >= (int)HBOARD_COUNT,
                "...and the retune reached the chain: %d commit(s) at %u MHz, "
                "want at least %d board(s) x %d chip(s)",
                at_target, (unsigned)VF_TARGET, (int)HBOARD_COUNT, ENUM_N);
    }

    CHECK(s_pending_after_repeat_cmd == (int)HBOARD_COUNT,
            "a repeat of the live target still marks every board pending (%d)",
            s_pending_after_repeat_cmd);
    CHECK(s_repeat_passes >= REPEAT_PASSES,
            "...and the loop ran %d further passes with it pending (got %d) — "
            "without them the next assertion is about a chance nobody took",
            REPEAT_PASSES, s_repeat_passes);
    CHECK(s_commits_at_repeat >= 0 && stub_pll_commit_n == s_commits_at_repeat,
            "...over which NO PLL register was committed (%d commits, was %d) — a "
            "redundant retune is a safe-PLL-sequence window for nothing",
            stub_pll_commit_n, s_commits_at_repeat);
    CHECK(pending_count() == 0,
            "...and the repeat's flag was cleared anyway, so it cannot re-fire "
            "every pass forever (%d left)",
            pending_count());

    CHECK(s_lock_bad == 0,
            "no lock was taken twice, released twice or taken on a stray handle "
            "across the whole run (%d misuse)",
            s_lock_bad);

    {
        const int pubs    = s_telem_sends / 2;
        const int periods = (int)(span / TELEM_PERIOD_MS);
        CHECK(pubs >= 1,
                "the loop published telemetry at all over %llu ms (%d publish(es)) "
                "— without this the bound below is met by publishing nothing",
                (unsigned long long)span, pubs);
        CHECK(s_telem_sends == pubs * 2,
                "every publish went to BOTH peers (%d send(s), %d publish(es))", s_telem_sends,
                pubs);
        CHECK(pubs <= periods + 1,
                "%d publish(es) over %llu ms is at most one per %u ms period plus "
                "the one at arming (%d allowed) — a deadline per board would make "
                "it %d",
                pubs, (unsigned long long)span, (unsigned)TELEM_PERIOD_MS, periods + 1,
                (periods + 1) * (int)HBOARD_COUNT);
    }

    return tst_done();
}
