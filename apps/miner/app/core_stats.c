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
#include "core_ident.h"
#include "core_ascset.h"
#include "cali.h"
#include "fault_report.h"
#include "led_fx.h"
#include "mc_api_identity.h"
#include "mc_api_server.h"
#include "mc_errcode.h"
#include "mm_led_cfg.h"

#include <stdio.h>
#include <string.h>

#define MHS_PER_DIFF1 4294.967296 /* MH per difficulty-1 share */
#define TELEM_FRESH_S 6           /* asic telemetry freshness limit, s */

static void win_roll_locked(time_t now)
{
    if (!g_hashwin.live) {
        g_hashwin.live = 1;
        g_hashwin.at   = now;
        return;
    }
    if (now <= g_hashwin.at)
        return;
    long adv = (long)(now - g_hashwin.at);
    if (adv > MHS_WIN_SLOTS)
        adv = MHS_WIN_SLOTS;
    for (long k = 1; k <= adv; k++)
        g_hashwin.slot[(g_hashwin.at + k) % MHS_WIN_SLOTS] = 0;
    g_hashwin.at = now;
}

void win_add_locked(time_t now, double dd)
{
    win_roll_locked(now);
    g_hashwin.slot[g_hashwin.at % MHS_WIN_SLOTS] += dd;
}

static double win_mhs(time_t now, int span)
{
    win_roll_locked(now);
    long lived = (long)(now - g_start);
    if (lived < 1)
        lived = 1;
    if (lived < span)
        span = (int)lived;
    if (span > MHS_WIN_MAX_S)
        span = MHS_WIN_MAX_S;
    double sum = 0;
    for (int a = 1; a <= span; a++) {
        time_t sec = g_hashwin.at - a;
        if (sec < 0)
            break;
        sum += g_hashwin.slot[sec % MHS_WIN_SLOTS];
    }
    return sum * MHS_PER_DIFF1 / (span > 0 ? span : 1);
}

void core_stats_push(void)
{
    pthread_mutex_lock(&g_stats_lock);
    uint32_t el = (uint32_t)(now_s() - g_start);
    double   av = el ?
              (g_diff1 > g_diff_rej ? g_diff1 - g_diff_rej : 0.0) * MHS_PER_DIFF1 / (double)el :
              0.0;
    pthread_mutex_unlock(&g_stats_lock);
    uint64_t main_beat = atomic_load(&g_main_beat_ms);
    uint64_t now       = now_ms();
    uint32_t health    = CORE_HEALTH_OK;
    if (main_beat && now > main_beat && now - main_beat > CORE_MAIN_STALL_MS)
        health |= CORE_UNHEALTHY_MAIN_STALL;
    static fault_report_t api_stall_rep;
    int                   api_stalled = 0;
    for (int i = 0; i < MC_API_CHAN_N; i++) {
        uint64_t api_at = atomic_load(&g_mc_api_busy_ms[i]);
        api_stalled |= api_at && now > api_at && now - api_at > CORE_API_STALL_MS;
    }
    if (api_stalled)
        health |= CORE_UNHEALTHY_API_STALL;
    if (fault_report_step(&api_stall_rep, (uint32_t)api_stalled) != FAULT_REPORT_NONE &&
            api_stalled)
        osif_log(OSIF_LOG_ERR, "API request not finished after %u s", CORE_API_STALL_MS / 1000u);
    mc_core_stats cs = {
        .ghs_avg_mhs = (uint32_t)av, .core_health = health, .job_gap_s = job_gap_s()
    };
    osif_ipc_send(g_ipc_core_mm, MSG_CORE_STATS, &cs, (int)sizeof cs);
}

static uint32_t meminfo_parse_available_kb(const char *text)
{
    for (const char *l = text; l && *l;) {
        if (strncmp(l, "MemAvailable:", 13) == 0) {
            unsigned long kb = 0;
            return sscanf(l + 13, "%lu", &kb) == 1 ? (uint32_t)kb : 0u;
        }
        const char *nl = strchr(l, '\n');
        l              = nl ? nl + 1 : NULL;
    }
    return 0;
}

static uint32_t meminfo_available_kb(void)
{
    char  buf[2048];
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return meminfo_parse_available_kb(buf);
}

void api_snapshot(mc_api_stats *s, void *ctx)
{
    mc_pipeline                        *p = (mc_pipeline *)ctx;
    static _Thread_local mc_api_id_buf  ib; /* MAC, DNA and version string storage */
    static _Thread_local core_hwcfg_buf hb;
    static _Thread_local char           purl[POOL_VALID_NUM][POOL_URL_MAX];
    static _Thread_local char           puser[POOL_VALID_NUM][POOL_USER_MAX];
    memset(s, 0, sizeof *s);
    mc_api_identity_fill(s, &ib);
    core_hwcfg_apply(s, &hb);
    time_t now  = time(NULL);
    time_t mono = now_s();
    s->when     = (uint32_t)now;
    if (CORE_PUB_LED) {
        mm_led_cfg_t led = mm_led_cfg_default;
        s->ledmode       = led.ledmode;
        s->led_effect    = led.effect;
        s->led_bright    = led.bright;
        s->led_temper    = led.temper;
        s->led_rgb       = led.rgb;
        s->led_runeffect = led.ledmode ? LED_FX_DARK : led.effect;
    }
    if (p) {
        pthread_mutex_lock(&g_stratum.lock);
        s->accepted = p->tally.accepted;
        s->rejected = p->tally.rejected;
        s->dhw      = p->dups;
        s->stale    = p->sv1.stale;
        pthread_mutex_unlock(&g_stratum.lock);
    }
    s->asc_count       = CORE_ASC_COUNT;
    int aging_finished = 0;

    struct core_pool_stat pst[POOL_VALID_NUM];
    uint32_t              memfree_kb = meminfo_available_kb();

    pthread_mutex_lock(&g_stats_lock);
    s->elapsed           = (uint32_t)(mono - g_start);
    s->workmode          = g_periph.valid ? g_periph.workmode : CALI_MODE_DEFAULT;
    s->maxmode           = g_periph.valid ? g_periph.maxmode : cali_mode_top(CALI_MODE_BOARD_MASK);
    s->caliall           = g_periph.valid ? g_periph.caliall : 0;
    s->bootby_reason     = g_bootby_reason;
    s->bootby_sub        = g_bootby_sub;
    uint64_t version_now = (uint64_t)now_s();
    snprintf(s->mm_version, sizeof s->mm_version, "%s",
            miner_version_get(&g_mm_version, version_now));
    if (CORE_PUB_LED && g_periph.valid) {
        s->ledmode       = core_setters_ledmode();
        s->led_runeffect = g_periph.runeffect;
        s->led_effect    = g_periph.led_effect;
        s->led_bright    = g_periph.led_bright;
        s->led_temper    = g_periph.led_temper;
        s->led_rgb       = g_periph.led_rgb;
    }
    s->hw_errors = g_nst.hw_errors;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        s->hw_board[b]   = g_nst.hw_board[b];
        s->hu_errcode[b] = g_periph.hu_errcode[b];
    }
    s->mm_errcode  = g_periph.mm_errcode;
    aging_finished = g_periph.aging_finished;
#ifdef CAP_ACTIVATION
    s->activation = g_periph.valid && g_periph.activation;
#else
    s->activation = 1;
#endif
    s->max_pout          = g_periph.cali_max_pout;
    s->target_temp       = g_periph.valid ? g_periph.cali_temp : FAN_TARGET_TEMP;
    s->cali_volt         = g_periph.cali_volt;
    s->cali_pll_start    = g_periph.cali_pll_start;
    s->cali_pll_interval = g_periph.cali_pll_interval;
    s->adj               = aging_finished ? 2 : (g_periph.cali_aged ? 1 : 0);
    for (int i = 0; i < POOL_VALID_NUM; i++)
        if (g_pool_st[i].best > s->best_share)
            s->best_share = g_pool_st[i].best;
    s->diff_accepted = g_diff_acc;
    s->diff_rejected = g_diff_rej;
    s->diff1         = g_diff1;
    s->ping_ms       = g_ping.ms;
    s->memfree_kb    = memfree_kb;
    {
        double d1 = g_diff1 - g_diff_rej;
        if (d1 < 0)
            d1 = 0;
        s->mhs_av = s->elapsed ? d1 * MHS_PER_DIFF1 / (double)s->elapsed : 0.0;
    }
    s->mhs_5s       = win_mhs(mono, 5);
    s->mhs_1m       = win_mhs(mono, 60);
    s->mhs_5m       = win_mhs(mono, 300);
    s->mhs_15m      = win_mhs(mono, 900);
    mc_api_telem *t = &s->telem;
    if (g_chip_at && mono - g_chip_at <= TELEM_FRESH_S) {
        t->valid        = 1;
        t->in_work      = g_chip.tm.in_work;
        t->freq_mhz     = g_chip.tm.freq_mhz;
        t->ghs_mm       = g_chip.tm.mhs_mm / 1000.0;
        t->ghs_spd_chip = g_chip.tm.ghs_spd_mhs / 1000.0;
        t->bin_grade    = g_chip.tm.bin_grade;
    }
    if (g_periph.valid)
        for (int f = 0; f < FAN_COUNT; f++)
            t->fan_rpm[f] = g_periph.fan_rpm[f];
    s->bar        = g_periph.aging_bar;
    t->board_temp = g_periph.valid ? g_periph.board_temp : ASIC_TEMP_NONE;
    if (g_periph.valid && g_periph.ps_valid) {
        t->ps[0] = g_periph.ps_err;
        t->ps[2] = g_periph.ps_vout_mv;
        t->ps[3] = (g_periph.ps_iout_ma + PS_IOUT_PER_A / 2) / PS_IOUT_PER_A;
        t->ps[5] = g_periph.ps_voutcmd_mv ? g_periph.ps_voutcmd_mv : g_periph.rail_mv;
        t->ps[6] = g_periph.ps_pout_w;
    }
    t->fan_pct  = g_periph.valid ? g_periph.fan_pct : 0;
    t->hb_count = 0;
    if (g_periph.valid) {
        int nb      = g_periph.hb_count < HBOARD_COUNT ? g_periph.hb_count : HBOARD_COUNT;
        t->hb_count = nb;
        for (int b = 0; b < nb; b++) {
            t->hb_outlet[b] = g_periph.hb_outlet[b];
            t->hb_inlet[b]  = g_periph.hb_inlet[b];
        }
        t->softoff_rsn = g_periph.softoff_rsn;
    }
    memcpy(s->ssid, g_periph.ssid, sizeof s->ssid);
    s->ssid[sizeof s->ssid - 1] = '\0';
    s->rssi                     = g_periph.rssi;
    s->net_devtype              = g_periph.net_devtype;
    if (CORE_PUB_LCD) {
        s->lcd_bright = g_periph.lcd_bright | ((unsigned)g_periph.lcd_bright_hi << 8);
        s->lcd_onoff  = g_periph.lcd_onoff;
        s->lcd_show   = g_periph.lcd_show;
    }
    if (CORE_PUB_AUDIO)
        s->volume = g_periph.volume;
    s->filtermesh = g_periph.filtermesh;
    memcpy(s->hash_sn, g_periph.hash_sn, sizeof s->hash_sn);
    mc_api_identity_dna(g_periph.dna, ib.dna);
    s->local_work = g_local_work;
    {
        uint32_t lw = 0;
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            lw += g_chip.local_work[bd];
        s->mm_local_work = lw;
    }
    if (g_chip_at && mono - g_chip_at <= TELEM_FRESH_S) {
        int nb      = g_chip.boards < HBOARD_COUNT ? g_chip.boards : HBOARD_COUNT;
        t->board_n  = nb;
        double tsum = 0;
        int    tmax = ASIC_TEMP_NONE, tvalid = 0;
        int    lapped = 1;
        for (int b = 0; b < nb; b++)
            if (g_chip.chips[b] && !g_chip.pvt_lapped[b])
                lapped = 0;
        for (int b = 0; b < nb; b++) {
            int n        = g_chip.chips[b] < MINER_MAX_CHIPS ? g_chip.chips[b] : MINER_MAX_CHIPS;
            t->chip_n[b] = n;
            t->board_err_crc[b] = g_chip.err_crc[b];
            t->board_com_crc[b] = g_chip.com_crc[b];
            for (int c = 0; c < n; c++) {
                t->chip_pvt_t[b][c] = g_chip.pvt_t[b][c];
                t->chip_pvt_v[b][c] = g_chip.pvt_v[b][c];
                t->chip_pass[b][c]  = g_chip.spdlog_pass[b][c];
                t->chip_fail[b][c]  = g_chip.spdlog_fail[b][c];
                for (int k = 0; k < ASIC_PLL_POINTS; k++) {
                    t->chip_pllcnt[b][c][k] = g_chip.pllcnt[b][c][k];
                    t->chip_cf[b][c][k]     = g_chip.cf[b][c][k];
                }
                if (g_chip.pvt_t[b][c] > ASIC_TEMP_NONE) {
                    tsum += g_chip.pvt_t[b][c];
                    if (g_chip.pvt_t[b][c] > tmax)
                        tmax = g_chip.pvt_t[b][c];
                    tvalid++;
                }
            }
        }
        t->temp_max = tvalid ? tmax : ASIC_TEMP_NONE;
        t->temp_avg = (tvalid && lapped) ? (int)(tsum / tvalid) : ASIC_TEMP_NONE;
    } else {
        t->temp_max = t->temp_avg = ASIC_TEMP_NONE;
    }
    memcpy(t->chip_mw, g_mw, sizeof g_mw);
    s->pool_failcnt = g_pool_failcnt;
    memcpy(s->netfail_time, g_netfail_time, sizeof s->netfail_time);
    t->work_state = g_mm_state == MC_WORK_IDLE || g_mm_state == MC_WORK_FAULT ?
            g_mm_state :
            (t->valid && t->in_work ? MC_WORK_WORK : MC_WORK_INIT);
    s->mm_errcode = g_periph.mm_errcode;
    for (int board = 0; board < HBOARD_COUNT; board++) {
        s->hu_errcode[board] = g_periph.hu_errcode[board];
        if (t->valid && board < g_chip.boards && g_chip.chips[board] == 0)
            s->hu_errcode[board] |= ERR_HU_ENUM;
        if (s->hu_errcode[board])
            s->mm_errcode |= ERR_MM_SLAVE;
    }
    memcpy(pst, g_pool_st, sizeof pst);
    static _Thread_local char bhash[sizeof g_block.hash]; /* current block hash copy, hex */
    memcpy(bhash, g_block.hash, sizeof bhash);
    s->net_difficulty = g_block.net_diff;
    s->block_time     = g_block.time > 0.0 ? now_mono_s() - g_block.time : 0.0;
    s->net_blocks     = g_block.net_blocks;
    memcpy(s->diff1_board, g_board_diff1, sizeof s->diff1_board);
    s->last_valid_work = g_last_valid_work;
    s->dev_elapsed     = g_dev_start ? (double)(mono - g_dev_start) : (double)s->elapsed;
    if (s->dev_elapsed < 1.0)
        s->dev_elapsed = 1.0;
    pthread_mutex_unlock(&g_stats_lock);
    s->block_hash = bhash;

    for (int i = 0; i < POOL_VALID_NUM; i++) {
        s->get_failures += pst[i].getfail;
        s->remote_failures += pst[i].rfail;
        s->diff_stale += pst[i].dstale;
    }

    pthread_mutex_lock(&g_pools_lock);
    s->pool_count = g_pool_pub_n;
    s->npools     = g_pool_pub_n;
    s->pool_idle  = g_pool_active < 0 || g_pool_active >= POOL_VALID_NUM || pst[g_pool_active].dead;
    for (int i = 0; i < g_pool_pub_n && i < POOL_VALID_NUM; i++) {
        memcpy(purl[i], g_pool_pub[i].url, sizeof purl[i]);
        memcpy(puser[i], g_pool_pub[i].user, sizeof puser[i]);
        s->pool[i] = (mc_api_pool){
            .id              = i,
            .prio            = g_pool_prio[i],
            .url             = purl[i],
            .user            = puser[i],
            .status          = !g_pool_enabled[i] ? "Disabled" : (pst[i].dead ? "Dead" : "Alive"),
            .accepted        = pst[i].acc,
            .rejected        = pst[i].rej,
            .diff_accepted   = pst[i].dacc,
            .diff_rejected   = pst[i].drej,
            .stale           = pst[i].stale,
            .diff_stale      = pst[i].dstale,
            .work_diff       = pst[i].wdiff,
            .last_share_diff = pst[i].lsdiff,
            .last_share_time = pst[i].ltime,
            .best_share      = pst[i].best,
            .diff1_shares    = pst[i].diff1,
            .getworks        = pst[i].getworks,
            .bad_work        = pst[i].bad,
            .block_version   = pst[i].bver,
            .works           = pst[i].works,
            .get_failures    = pst[i].getfail,
            .remote_failures = pst[i].rfail,
            .block_height    = pst[i].height,
            .stratum_active  = (i == g_pool_active),
            .tx_cnt          = pst[i].txcnt,
            .tx_bytes        = pst[i].txbytes,
            .rx_cnt          = pst[i].rxcnt,
            .rx_bytes        = pst[i].rxbytes,
        };
    }
    pthread_mutex_unlock(&g_pools_lock);
}
