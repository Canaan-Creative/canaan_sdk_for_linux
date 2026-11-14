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

#include "mm_telem.h"
#include "mm_internal.h"
#include "fault_report.h"
#include "hashmcu.h"
#include "miner_ipc.h"
#include "asic_temp_pub.h"
#include "board_sensors.h"
#include "osif.h"

#include <pthread.h>
#include <string.h>

#define TELEM_STALE_MS 10000u /* telemetry age limit, ms */

_Static_assert(MM_PLL_POINTS == ASIC_PLL_POINTS,
        "mm_telem.h MM_PLL_POINTS drifted from ASIC_PLL_POINTS");

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* last local poll telemetry */
static mm_telem_t g_tel = { .tmax = ASIC_TEMP_NONE,
    .tavg                         = ASIC_TEMP_NONE,
    .pout_w                       = MM_TELEM_POUT_NONE,
    .ps_vout_mv                   = MM_TELEM_VOUT_NONE,
    .ps_vout12                    = MM_TELEM_VOUT_NONE,
    .workmode                     = MM_TELEM_WORKMODE_NONE };
static uint64_t   g_at; /* last local poll time, ms */

/* last asic_miner telemetry */
static mm_telem_t g_push = { .tmax = ASIC_TEMP_NONE, .tavg = ASIC_TEMP_NONE };
static uint64_t   g_push_at;  /* last asic_miner telemetry time, ms */
static unsigned   g_conn_min; /* first asic IPC connection accepted after the last spawn */
static uint64_t   g_core_at;  /* last core_miner stats time, ms */
static int        g_core_healthy = 1;
static uint32_t   g_core_job_gap_s; /* time since the last pool job, s */

void mm_telem_ingest_core(const void *data, int len)
{
    static fault_report_t core_len_fault;
    int                   bad_len = !data || len != (int)sizeof(mc_core_stats);
    if (fault_report_step(&core_len_fault, (uint32_t)bad_len) != FAULT_REPORT_NONE) {
        if (bad_len)
            osif_log(OSIF_LOG_ERR,
                    "core_miner status message of wrong size (%d "
                    "of %d bytes) ignored — are all miner programs from the "
                    "same build?",
                    len, (int)sizeof(mc_core_stats));
        else
            osif_log(OSIF_LOG_INFO, "core_miner status messages accepted again");
    }
    if (bad_len)
        return;
    mc_core_stats cs;
    memcpy(&cs, data, sizeof cs);
    pthread_mutex_lock(&g_lock);
    g_core_at        = osif_now_ms();
    g_core_healthy   = (cs.core_health == CORE_HEALTH_OK);
    g_core_job_gap_s = cs.job_gap_s;
    pthread_mutex_unlock(&g_lock);
}

void mm_telem_ingest(const void *data, int len, unsigned conn)
{
    static fault_report_t telem_len_fault;
    int                   bad_len = !data || len != (int)sizeof(asic_chip_telem);
    if (fault_report_step(&telem_len_fault, (uint32_t)bad_len) != FAULT_REPORT_NONE) {
        if (bad_len)
            osif_log(OSIF_LOG_ERR,
                    "asic_miner status message of wrong size (%d "
                    "of %d bytes) ignored — are all miner programs from the "
                    "same build?",
                    len, (int)sizeof(asic_chip_telem));
        else
            osif_log(OSIF_LOG_INFO, "asic_miner status messages accepted again");
    }
    if (bad_len)
        return;
    asic_chip_telem ct;
    memcpy(&ct, data, sizeof ct);

    mm_telem_t t = { .tmax = ASIC_TEMP_NONE, .tavg = ASIC_TEMP_NONE };
    t.in_work    = ct.tm.in_work;
    t.asic_live  = (ct.asic_health == ASIC_HEALTH_OK);
    t.ghs_spd    = ct.tm.ghs_spd_mhs / 1000.0;

    double sum    = 0;
    int    navg   = 0;
    t.asic_boards = ct.boards < HBOARD_COUNT ? (int)ct.boards : HBOARD_COUNT;
    for (int b = 0; b < t.asic_boards; b++) {
        int nc = ct.chips[b];
        if (nc > MINER_MAX_CHIPS)
            nc = MINER_MAX_CHIPS;
        t.asic_chips_board[b] = nc;
        if (nc <= 0)
            continue;
        t.asic_chips += nc;

        double degc[MINER_MAX_CHIPS];
        for (int c = 0; c < nc; c++)
            degc[c] = (double)ct.pvt_t[b][c];
        asic_temp_pub p = asic_temp_publish(degc, nc, ct.pvt_lapped[b]);
        if (p.max > t.tmax)
            t.tmax = p.max;
        if (p.avg > ASIC_TEMP_NONE) {
            sum += p.avg;
            navg++;
        }
        t.temp_valid_chips += p.valid;

        for (int k = 0; k < MM_PLL_POINTS && k < ASIC_PLL_POINTS; k++) {
            for (int c = 0; c < nc; c++)
                t.pll_cores[k] += ct.pllcnt[b][c][k];
            if (t.pll_mhz[k] == 0)
                t.pll_mhz[k] = ct.cf[b][0][k];
        }
    }
    if (navg)
        t.tavg = (int)(sum / navg);

    pthread_mutex_lock(&g_lock);
    if ((int)(conn - g_conn_min) < 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_push    = t;
    g_push_at = osif_now_ms();
    pthread_mutex_unlock(&g_lock);
}

static void hashboard_temps(mm_telem_t *t)
{
    static int            inited;
    static fault_report_t fault[HBOARD_COUNT];
    if (!inited) {
        hashmcu_init();
        inited = 1;
    }

    t->hb_count = hashmcu_count();
    for (int i = 0; i < t->hb_count; i++) {
        hashmcu_temps h;
        int           rc = hashmcu_read(i, &h);
        t->hb_outlet[i]  = h.outlet_degc;
        t->hb_inlet[i]   = h.inlet_degc;

        uint32_t code = mm_hbmcu_fault_code(rc, h.outlet_degc != ASIC_TEMP_NONE);
        if (fault_report_step(&fault[i], code) == FAULT_REPORT_NONE)
            continue;
        if (code == MM_HBMCU_OK)
            osif_log(OSIF_LOG_INFO, "hashboard %d temperature readable again", i);
        else if (code == MM_HBMCU_INLET)
            osif_log(OSIF_LOG_WARN,
                    "hashboard %d inlet temperature not readable "
                    "(outlet %d degC)",
                    i, h.outlet_degc);
        else
            osif_log(OSIF_LOG_WARN, "hashboard %d temperature not readable", i);
    }
}

void mm_telem_poll(void)
{
    mm_telem_t t = { .tmax = ASIC_TEMP_NONE,
        .tavg              = ASIC_TEMP_NONE,
        .pout_w            = MM_TELEM_POUT_NONE,
        .ps_vout_mv        = MM_TELEM_VOUT_NONE,
        .ps_vout12         = MM_TELEM_VOUT_NONE };
    for (int i = 0; i < HBOARD_COUNT; i++)
        t.hb_outlet[i] = t.hb_inlet[i] = ASIC_TEMP_NONE;

    board_power pw;
    if (board_power_read(&pw) == 0) {
        t.pout_w     = pw.pout_w;
        t.ps_vout_mv = pw.vout_mv;
        t.ps_vout12  = pw.vout12;
    }

    t.workmode = mm_workmode_running();

    hashboard_temps(&t);

    pthread_mutex_lock(&g_lock);
    g_tel = t;
    g_at  = osif_now_ms();
    pthread_mutex_unlock(&g_lock);
}

void mm_telem_asic_reset(unsigned conn_min)
{
    pthread_mutex_lock(&g_lock);
    g_push     = (mm_telem_t){ .tmax = ASIC_TEMP_NONE, .tavg = ASIC_TEMP_NONE };
    g_push_at  = 0;
    g_conn_min = conn_min;
    pthread_mutex_unlock(&g_lock);
}

void mm_telem_get(mm_telem_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out            = g_tel;
    uint64_t now    = osif_now_ms();
    out->poll_fresh = g_at && (now - g_at) <= TELEM_STALE_MS;
    out->core_live  = g_core_at && (now - g_core_at) <= TELEM_STALE_MS && g_core_healthy;
    out->job_gap_s  = g_core_job_gap_s;
    if (g_push_at && (now - g_push_at) <= TELEM_STALE_MS) {
        out->tmax             = g_push.tmax;
        out->tavg             = g_push.tavg;
        out->temp_valid_chips = g_push.temp_valid_chips;
        out->in_work          = g_push.in_work;
        out->asic_live        = g_push.asic_live;
        out->asic_chips       = g_push.asic_chips;
        out->asic_boards      = g_push.asic_boards;
        memcpy(out->asic_chips_board, g_push.asic_chips_board, sizeof out->asic_chips_board);
        out->ghs_spd = g_push.ghs_spd;
        memcpy(out->pll_cores, g_push.pll_cores, sizeof out->pll_cores);
        memcpy(out->pll_mhz, g_push.pll_mhz, sizeof out->pll_mhz);
    }
    pthread_mutex_unlock(&g_lock);
}
