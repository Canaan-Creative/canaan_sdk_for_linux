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

#include "mc_api_estats.h"
#include "mc_api_cmds.h"
#include "process_version.h"
#include "jwrite.h"
#include "boardconf.h"
#include "core_board.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifndef MINER_PROTO_VER
#error "MINER_PROTO_VER must be defined by the build"
#endif

typedef struct {
    double   dhspd;    /* speed-log fail ratio, % */
    double   wu;       /* work utility */
    double   ghs_avg;  /* average hashrate, GH/s */
    double   freq_avg; /* average frequency, MHz */
    int      chips_all;
    int      hb_n; /* boards with inlet and outlet temperatures, count */
    int      hb_detected;
    uint32_t mh_sum;
    double   mghs_v[HBOARD_COUNT];  /* hashrate per board, GH/s */
    int      mtmax_v[HBOARD_COUNT]; /* hottest chip per board, degrees C */
} estats_derived;

int mc_api_estats_all(const char *req)
{
    if (!req)
        return 0;
    const char *j = strstr(req, "\"parameter\"");
    if (j) {
        j = strchr(j, ':');
        if (j)
            j = strchr(j, '"');
        return j && strncmp(j + 1, "all", 3) == 0;
    }
    const char *bar = strchr(req, '|');
    return bar && strncmp(bar + 1, "all", 3) == 0;
}

static void ap(char *mm, size_t cap, size_t *pn, const char *fmt, ...)
{
    if (*pn + 1 >= cap)
        return;
    va_list a;
    va_start(a, fmt);
    int w = vsnprintf(mm + *pn, cap - *pn, fmt, a);
    va_end(a);
    if (w > 0)
        *pn += (size_t)w;
    if (*pn >= cap)
        *pn = cap - 1;
}

static void apclose(char *mm, size_t cap, size_t *pn)
{
    if (*pn > 0 && mm[*pn - 1] == ' ') {
        mm[*pn - 1] = ']';
        return;
    }
    if (*pn + 1 < cap) {
        mm[(*pn)++] = ']';
        mm[*pn]     = '\0';
    }
}

static void append_chip_estats(const mc_api_stats *s, char *mm, size_t cap, int show_all)
{
    const mc_api_telem *t = &s->telem;
    size_t              n = strlen(mm);
    for (int b = 0; b < HBOARD_COUNT; b++) {
        const int nc = t->chip_n[b];

        ap(mm, cap, &n, " PLL%d[", b);
        for (int k = 0; k < ASIC_PLL_POINTS; k++) {
            int sum = 0;
            for (int c = 0; c < nc; c++)
                sum += t->chip_pllcnt[b][c][k];
            ap(mm, cap, &n, "%d ", sum);
        }
        apclose(mm, cap, &n);

        ap(mm, cap, &n, " SF%d[", b);
        for (int k = 0; k < ASIC_PLL_POINTS; k++)
            ap(mm, cap, &n, "%d ", t->chip_cf[b][0][k]);
        apclose(mm, cap, &n);

        ap(mm, cap, &n, " PVT_T%d[", b);
        for (int c = 0; c < nc; c++)
            ap(mm, cap, &n, "%3d ", t->chip_pvt_t[b][c]);
        apclose(mm, cap, &n);

        ap(mm, cap, &n, " PVT_V%d[", b);
        for (int c = 0; c < nc; c++)
            ap(mm, cap, &n, "%d ", t->chip_pvt_v[b][c]);
        apclose(mm, cap, &n);

        ap(mm, cap, &n, " MW%d[", b);
        for (int c = 0; c < nc; c++)
            ap(mm, cap, &n, "%llu ", (unsigned long long)t->chip_mw[b][c]);
        apclose(mm, cap, &n);
    }

    ap(mm, cap, &n, " CRC[");
    for (int b = 0; b < HBOARD_COUNT; b++)
        ap(mm, cap, &n, "%u ", t->board_err_crc[b]);
    apclose(mm, cap, &n);
    ap(mm, cap, &n, " COMCRC[");
    for (int b = 0; b < HBOARD_COUNT; b++)
        ap(mm, cap, &n, "%u ", t->board_com_crc[b]);
    apclose(mm, cap, &n);
    ap(mm, cap, &n, " ATA%d[", s->workmode);
    if (s->adj != 0)
        ap(mm, cap, &n, "%d-%d-%d-%d-%d ", s->max_pout, s->target_temp, s->cali_volt,
                s->cali_pll_start, s->cali_pll_interval);
    apclose(mm, cap, &n);

    if (!show_all)
        return;

    for (int b = 0; b < HBOARD_COUNT; b++) {
        const int nc = t->chip_n[b];

        for (int l = 0; l < ASIC_PLL_POINTS; l++) {
            ap(mm, cap, &n, " CF%d_%d[", b, l);
            for (int c = 0; c < nc; c++)
                ap(mm, cap, &n, "%3d ", t->chip_cf[b][c][l]);
            apclose(mm, cap, &n);
        }
        for (int l = 0; l < ASIC_PLL_POINTS; l++) {
            ap(mm, cap, &n, " PLLCNT%d_%d[", b, l);
            for (int c = 0; c < nc; c++)
                ap(mm, cap, &n, "%3d ", t->chip_pllcnt[b][c][l]);
            apclose(mm, cap, &n);
        }
        ap(mm, cap, &n, " ERATIO%d[", b);
        for (int c = 0; c < nc; c++) {
            double p = t->chip_pass[b][c], f = t->chip_fail[b][c];
            ap(mm, cap, &n, "%6.2f%% ", (p + f) > 0.0 ? f * 100.0 / (p + f) : 0.0);
        }
        apclose(mm, cap, &n);

        ap(mm, cap, &n, " C_%02d_%02d[", b, 0);
        for (int c = 0; c < nc; c++)
            ap(mm, cap, &n, "%7u ", t->chip_pass[b][c]);
        apclose(mm, cap, &n);
        ap(mm, cap, &n, " C_%02d_%02d[", b, 1);
        for (int c = 0; c < nc; c++)
            ap(mm, cap, &n, "%7u ", t->chip_fail[b][c]);
        apclose(mm, cap, &n);

        ap(mm, cap, &n, " GHSmm%02d[", b);
        for (int c = 0; c < nc; c++) {
            double mh = 0.0;
            for (int l = 0; l < ASIC_PLL_POINTS; l++)
                mh += (double)t->chip_pllcnt[b][c][l] * t->chip_cf[b][c][l];
            ap(mm, cap, &n, "%7.2f ", mh / 1000.0);
        }
        apclose(mm, cap, &n);
    }
}

static void hb_temp_list(char *out, size_t cap, const char *label, const int *v, int n)
{
    out[0] = '\0';
    if (n <= 0)
        return;
    size_t k = (size_t)snprintf(out, cap, " %s[", label);
    for (int b = 0; b < n; b++) {
        int w = snprintf(out + k, cap - k, "%d ", v[b]);
        if (w < 0 || k + (size_t)w >= cap) {
            k = cap - 1;
            break;
        }
        k += (size_t)w;
    }
    out[k - 1] = ']';
    out[k]     = '\0';
}

static void fan_rpm_list(char *out, size_t cap, const int *rpm)
{
    size_t k = 0;
    for (int f = 0; f < FAN_COUNT; f++) {
        int w = snprintf(out + k, cap - k, " Fan%d[%d]", f + 1, rpm[f]);
        if (w < 0 || k + (size_t)w >= cap)
            break;
        k += (size_t)w;
    }
}

static double ghs_from_diff1(double d1, uint32_t elapsed)
{
    return elapsed ? d1 / (double)elapsed * 60.0 * 4.294967296 / 60.0 : 0.0;
}

static void mghs_list(char *out, size_t cap, const double *v, int n)
{
    size_t k = (size_t)snprintf(out, cap, " MGHS[");
    for (int b = 0; b < n; b++) {
        int w = snprintf(out + k, cap - k, "%.2f ", v[b]);
        if (w < 0 || k + (size_t)w >= cap) {
            k = cap - 1;
            break;
        }
        k += (size_t)w;
    }
    out[k - 1] = ']';
    out[k]     = '\0';
}

static void board_vals(char *out, size_t cap, const uint32_t *v)
{
    size_t k = 0;
    for (int b = 0; b < HBOARD_COUNT && k < cap; b++)
        k += (size_t)snprintf(out + k, cap - k, b ? " %u" : "%u", v[b]);
}

static void estats_derive(const mc_api_stats *s, estats_derived *d)
{
    const mc_api_telem *t        = &s->telem;
    double              spd_pass = 0.0, spd_fail = 0.0;
    for (int b = 0; b < t->board_n; b++)
        for (int c = 0; c < t->chip_n[b]; c++)
            if (t->chip_pass[b][c]) {
                spd_pass += t->chip_pass[b][c];
                spd_fail += t->chip_fail[b][c];
            }
    d->dhspd     = spd_pass > 0.0 ? spd_fail / (spd_pass + spd_fail) * 100.0 :
                                    (t->board_n ? 100.0 : 0.0);
    d->wu        = mc_api_work_utility(s);
    d->ghs_avg   = ghs_from_diff1(diff1_work(s), s->elapsed);
    d->chips_all = 0;
    for (int b = 0; b < t->board_n; b++)
        d->chips_all += t->chip_n[b];
    d->freq_avg    = (t->ghs_mm > 0.0 && d->chips_all > 0) ?
               t->ghs_mm * 1000.0 / ((double)d->chips_all * ASIC_CORES_PER_CHIP * 8.0) :
               (double)t->freq_mhz;
    d->hb_n        = t->hb_count < HBOARD_COUNT ? t->hb_count : HBOARD_COUNT;
    d->hb_detected = 0;
    for (int b = 0; b < t->board_n; b++)
        if (t->chip_n[b] > 0)
            d->hb_detected++;
    d->mh_sum = 0;
    for (int b = 0; b < HBOARD_COUNT; b++)
        d->mh_sum += s->hw_board[b];
    for (int b = 0; b < HBOARD_COUNT; b++) {
        d->mghs_v[b] = ghs_from_diff1(s->diff1_board[b], s->elapsed);
        int mx       = ASIC_TEMP_NONE;
        if (b < t->board_n)
            for (int c = 0; c < t->chip_n[b]; c++)
                if (t->chip_pvt_t[b][c] > mx)
                    mx = t->chip_pvt_t[b][c];
        d->mtmax_v[b] = mx;
    }
}

static const char *work_state_text(uint8_t ws)
{
    return ws == MC_WORK_WORK   ? "In Work" :
            ws == MC_WORK_IDLE  ? "In Idle" :
            ws == MC_WORK_FAULT ? "In Fault" :
                                  "In Init";
}

static void build_mm_id0(const mc_api_stats *s, char *mm, size_t cap, int show_all)
{
    const mc_api_telem *t = &s->telem;
    estats_derived      d;
    estats_derive(s, &d);
    char hbi[16 + 12 * HBOARD_COUNT], hbo[16 + 12 * HBOARD_COUNT];
    hb_temp_list(hbi, sizeof hbi, "HBITemp", t->hb_inlet, d.hb_n);
    hb_temp_list(hbo, sizeof hbo, "HBOTemp", t->hb_outlet, d.hb_n);
    char fans[MC_API_FAN_FIELD_CAP * FAN_COUNT + 1];
    fan_rpm_list(fans, sizeof fans, t->fan_rpm);
    char nf[8 * 12];
    snprintf(nf, sizeof nf, "%u %u %u %u %u %u %u %u", s->netfail_time[0], s->netfail_time[1],
            s->netfail_time[2], s->netfail_time[3], s->netfail_time[4], s->netfail_time[5],
            s->netfail_time[6], s->netfail_time[7]);
    char mhs[MC_API_MH_CAP * HBOARD_COUNT], echu[MC_API_ECHU_CAP * HBOARD_COUNT];
    board_vals(mhs, sizeof mhs, s->hw_board);
    board_vals(echu, sizeof echu, s->hu_errcode);
    char mghs[16 + MC_API_MGHS_CAP * HBOARD_COUNT];
    char mtmax[16 + MC_API_MTMAX_CAP * HBOARD_COUNT];
    mghs_list(mghs, sizeof mghs, d.mghs_v, HBOARD_COUNT);
    hb_temp_list(mtmax, sizeof mtmax, "MTmax", d.mtmax_v, HBOARD_COUNT);
    char safe_model[128];
    mc_api_text_field(s->model, safe_model, sizeof safe_model);
    snprintf(mm, cap,
            "Ver[%s-%s] LVer[%s] FW[%s] PVer[" MINER_PROTO_VER
            "] " CORE_FMT_LED CORE_FMT_LCD_ONOFF CORE_FMT_VOLUME "DNA[%s] MEMFREE[%u] PFCnt[%u] "
            "NETFAIL[%s] SYSTEMSTATU[Work: %s, Hash Board: %d] "
            "Elapsed[%u] BOOTBY[0x%02X.%08X] LW[%u] MH[%s] DHW[%u] HW[%u] DH[%.3f%%] "
            "ITemp[%d] OTemp[%d]%s%s TMax[%d] TAvg[%d] TarT[%d]%s FanR[%d%%] "
            "PS[%d %d %d %d %d %d %d] GHSspd[%.2f] DHspd[%.3f%%] GHSmm[%.2f] GHSavg[%.2f] "
            "WU[%.2f] Freq[%.2f]%s Activation[%d]%s MTavg[%d] TA[%d] "
            "Core[A3197S] BIN[%u] PING[%u] POOLS[%d] SoftOFF[%u] ECHU[%s] ECMM[%u]",
            safe_model, s->version, s->mm_version[0] ? s->mm_version : MINER_VERSION_UNKNOWN,
            FW_TYPE, CORE_ARG_LED CORE_ARG_LCD_ONOFF CORE_ARG_VOLUME s->dna, s->memfree_kb,
            s->pool_failcnt, nf, work_state_text(t->work_state), d.hb_detected, s->elapsed,
            s->bootby_reason, s->bootby_sub, s->mm_local_work, mhs, s->dhw, d.mh_sum, d.dhspd,
            ASIC_TEMP_NONE, t->board_temp, hbi, hbo, t->temp_max, t->temp_avg, s->target_temp, fans,
            t->fan_pct, t->ps[0], t->ps[1], t->ps[2], t->ps[3], t->ps[4], t->ps[5], t->ps[6],
            t->ghs_spd_chip, d.dhspd, t->ghs_mm, d.ghs_avg, d.wu, d.freq_avg, mghs, s->activation,
            mtmax, t->temp_avg, d.chips_all, t->bin_grade, s->ping_ms, s->pool_idle,
            (unsigned)t->softoff_rsn, echu, s->mm_errcode);

    append_chip_estats(s, mm, cap, show_all);

    size_t n = strlen(mm);
    ap(mm, cap, &n, " WORKMODE[%d] WORKLEVEL[0] MPO[%d] CALIALL[%d] ADJ[%d]", s->workmode,
            s->max_pout, s->caliall, s->adj);
    if (s->adj != 1)
        ap(mm, cap, &n, " BAR[%d]", s->bar);
}

static void build_lite_id0(const mc_api_stats *s, char *mm, size_t cap, const char *ssid)
{
    const mc_api_telem *t = &s->telem;
    char                echu[MC_API_ECHU_CAP * HBOARD_COUNT];
    board_vals(echu, sizeof echu, s->hu_errcode);
    snprintf(mm, cap,
            "LVer[%s] FW[%s] PVer[" MINER_PROTO_VER "] STATE[%d] MODE[%d] "
            "LEVEL[0] GHSspd[%f] ITemp[%d] OTemp[%d] Limitemp[0] FanR[%d%%] "
            "Filter[%d] " CORE_FMT_LED CORE_FMT_LCD_ONOFF CORE_FMT_LCD_BRIGHT CORE_FMT_VOLUME
            "Activation[%d] ECHU[%s] "
            "ECMM[%u] SSID[%s] RSSI[%d] NetDevType[%d]",
            s->mm_version[0] ? s->mm_version : MINER_VERSION_UNKNOWN, FW_TYPE, (int)t->work_state,
            s->workmode, t->ghs_spd_chip, ASIC_TEMP_NONE, t->board_temp, t->fan_pct, s->filtermesh,
            CORE_ARG_LED CORE_ARG_LCD_ONOFF CORE_ARG_LCD_BRIGHT CORE_ARG_VOLUME s->activation, echu,
            s->mm_errcode, ssid, s->rssi, s->net_devtype);
}

static void json_u32_list(jwriter *w, const char *key, const uint32_t *v, int n)
{
    jw_key_arr(w, key);
    for (int i = 0; i < n; i++)
        jw_val_fmt(w, "%u", v[i]);
    jw_arr_close(w);
}

static void json_int_list(jwriter *w, const char *key, const int *v, int n)
{
    jw_key_arr(w, key);
    for (int i = 0; i < n; i++)
        jw_val_fmt(w, "%d", v[i]);
    jw_arr_close(w);
}

static void json_led(jwriter *w, const mc_api_stats *s)
{
    jw_kv_fmt(w, "LED", "\"%d-%d\"", s->ledmode, s->led_runeffect);
    jw_kv_fmt(w, "LEDUser", "\"%d-%d-%u-%u-%u-%u\"", s->led_effect, s->led_bright, s->led_temper,
            (s->led_rgb >> 16) & 0xFFu, (s->led_rgb >> 8) & 0xFFu, s->led_rgb & 0xFFu);
}

static void json_sysinfo(jwriter *w, const mc_api_stats *s, const estats_derived *d)
{
    const mc_api_telem *t = &s->telem;
    char                safe_model[128], ver[256], key[16];
    mc_api_text_field(s->model, safe_model, sizeof safe_model);
    snprintf(ver, sizeof ver, "%s-%s", safe_model, s->version);
    jw_key_obj(w, "Sysinfo");
    jw_kv_str(w, "Ver", ver);
    jw_kv_str(w, "LVer", s->mm_version[0] ? s->mm_version : MINER_VERSION_UNKNOWN);
    jw_kv_str(w, "FW", FW_TYPE);
    jw_kv_str(w, "PVer", MINER_PROTO_VER);
    if (CORE_PUB_LED)
        json_led(w, s);
    if (CORE_PUB_LCD) {
        jw_kv_fmt(w, "LcdOnoff", "%d", s->lcd_onoff);
        jw_kv_fmt(w, "LcdSwitch", "%d", s->lcd_show);
    }
    if (CORE_PUB_AUDIO)
        jw_kv_fmt(w, "Volume", "%d", s->volume);
    jw_kv_str(w, "DNA", s->dna);
    jw_kv_fmt(w, "MEMFREE", "%u", s->memfree_kb);
    jw_kv_fmt(w, "PFCnt", "%u", s->pool_failcnt);
    json_u32_list(w, "NETFAIL", s->netfail_time, 8);
    jw_kv_fmt(w, "SYSTEMSTATU", "\"Work: %s, Hash Board: %d\"", work_state_text(t->work_state),
            d->hb_detected);
    jw_kv_fmt(w, "Elapsed", "%u", s->elapsed);
    jw_kv_fmt(w, "BOOTBY", "\"0x%02X.%08X\"", s->bootby_reason, s->bootby_sub);
    jw_kv_fmt(w, "ITemp", "%d", ASIC_TEMP_NONE);
    jw_kv_fmt(w, "OTemp", "%d", t->board_temp);
    for (int f = 0; f < FAN_COUNT; f++) {
        snprintf(key, sizeof key, "Fan%d", f + 1);
        jw_kv_fmt(w, key, "%d", t->fan_rpm[f]);
    }
    jw_kv_fmt(w, "FanR", "%d", t->fan_pct);
    json_int_list(w, "PS", t->ps, 7);
    jw_kv_fmt(w, "Activation", "%d", s->activation);
    jw_kv_fmt(w, "PING", "%u", s->ping_ms);
    jw_kv_fmt(w, "POOLS", "%d", s->pool_idle);
    jw_kv_fmt(w, "SoftOFF", "%u", (unsigned)t->softoff_rsn);
    jw_kv_fmt(w, "ECMM", "%u", s->mm_errcode);
    jw_obj_close(w);
}

static void json_hashinfo(jwriter *w, const mc_api_stats *s, const estats_derived *d)
{
    const mc_api_telem *t = &s->telem;
    char                key[16];
    jw_key_obj(w, "Hashinfo");
    jw_kv_fmt(w, "LW", "%u", s->mm_local_work);
    json_u32_list(w, "MH", s->hw_board, HBOARD_COUNT);
    jw_kv_fmt(w, "DHW", "%u", s->dhw);
    jw_kv_fmt(w, "HW", "%u", d->mh_sum);
    jw_kv_fmt(w, "DH", "%.3f", d->dhspd);
    if (d->hb_n > 0) {
        json_int_list(w, "HBITemp", t->hb_inlet, d->hb_n);
        json_int_list(w, "HBOTemp", t->hb_outlet, d->hb_n);
    }
    jw_kv_fmt(w, "TMax", "%d", t->temp_max);
    jw_kv_fmt(w, "TAvg", "%d", t->temp_avg);
    jw_kv_fmt(w, "TarT", "%d", s->target_temp);
    jw_kv_fmt(w, "GHSspd", "%.2f", t->ghs_spd_chip);
    jw_kv_fmt(w, "DHspd", "%.3f", d->dhspd);
    jw_kv_fmt(w, "GHSmm", "%.2f", t->ghs_mm);
    jw_kv_fmt(w, "GHSavg", "%.2f", d->ghs_avg);
    jw_kv_fmt(w, "WU", "%.2f", d->wu);
    jw_kv_fmt(w, "Freq", "%.2f", d->freq_avg);
    jw_key_arr(w, "MGHS");
    for (int b = 0; b < HBOARD_COUNT; b++)
        jw_val_fmt(w, "%.2f", d->mghs_v[b]);
    jw_arr_close(w);
    json_int_list(w, "MTmax", d->mtmax_v, HBOARD_COUNT);
    jw_kv_fmt(w, "MTavg", "%d", t->temp_avg);
    jw_kv_fmt(w, "TA", "%d", d->chips_all);
    jw_kv_str(w, "Core", "A3197S");
    jw_kv_fmt(w, "BIN", "%u", t->bin_grade);
    json_u32_list(w, "ECHU", s->hu_errcode, HBOARD_COUNT);
    for (int b = 0; b < HBOARD_COUNT; b++) {
        snprintf(key, sizeof key, "PLL%d", b);
        jw_key_arr(w, key);
        for (int k = 0; k < ASIC_PLL_POINTS; k++) {
            int sum = 0;
            for (int c = 0; c < t->chip_n[b]; c++)
                sum += t->chip_pllcnt[b][c][k];
            jw_val_fmt(w, "%d", sum);
        }
        jw_arr_close(w);
        snprintf(key, sizeof key, "SF%d", b);
        jw_key_arr(w, key);
        for (int k = 0; k < ASIC_PLL_POINTS; k++)
            jw_val_fmt(w, "%d", t->chip_cf[b][0][k]);
        jw_arr_close(w);
    }
    json_u32_list(w, "CRC", t->board_err_crc, HBOARD_COUNT);
    json_u32_list(w, "COMCRC", t->board_com_crc, HBOARD_COUNT);
    snprintf(key, sizeof key, "ATA%d", s->workmode);
    if (s->adj != 0)
        jw_kv_fmt(w, key, "\"%d-%d-%d-%d-%d\"", s->max_pout, s->target_temp, s->cali_volt,
                s->cali_pll_start, s->cali_pll_interval);
    else
        jw_kv_str(w, key, "");
    jw_kv_fmt(w, "WORKMODE", "%d", s->workmode);
    jw_kv_raw(w, "WORKLEVEL", "0");
    jw_kv_fmt(w, "MPO", "%d", s->max_pout);
    jw_kv_fmt(w, "CALIALL", "%d", s->caliall);
    jw_kv_fmt(w, "ADJ", "%d", s->adj);
    if (s->adj != 1)
        jw_kv_fmt(w, "BAR", "%d", s->bar);
    jw_obj_close(w);
}

static void json_hb_info(jwriter *w, const mc_api_telem *t, int b, int show_all)
{
    const int nc = t->chip_n[b];
    char      key[24];
    snprintf(key, sizeof key, "HB_%d_Info", b);
    jw_key_obj(w, key);
    snprintf(key, sizeof key, "PVT_T%d", b);
    jw_key_arr(w, key);
    for (int c = 0; c < nc; c++)
        jw_val_fmt(w, "%d", t->chip_pvt_t[b][c]);
    jw_arr_close(w);
    snprintf(key, sizeof key, "PVT_V%d", b);
    jw_key_arr(w, key);
    for (int c = 0; c < nc; c++)
        jw_val_fmt(w, "%d", t->chip_pvt_v[b][c]);
    jw_arr_close(w);
    snprintf(key, sizeof key, "MW%d", b);
    jw_key_arr(w, key);
    for (int c = 0; c < nc; c++)
        jw_val_fmt(w, "%llu", (unsigned long long)t->chip_mw[b][c]);
    jw_arr_close(w);
    if (show_all) {
        for (int l = 0; l < ASIC_PLL_POINTS; l++) {
            snprintf(key, sizeof key, "CF%d_%d", b, l);
            jw_key_arr(w, key);
            for (int c = 0; c < nc; c++)
                jw_val_fmt(w, "%d", t->chip_cf[b][c][l]);
            jw_arr_close(w);
        }
        for (int l = 0; l < ASIC_PLL_POINTS; l++) {
            snprintf(key, sizeof key, "PLLCNT%d_%d", b, l);
            jw_key_arr(w, key);
            for (int c = 0; c < nc; c++)
                jw_val_fmt(w, "%d", t->chip_pllcnt[b][c][l]);
            jw_arr_close(w);
        }
        snprintf(key, sizeof key, "ERATIO%d", b);
        jw_key_arr(w, key);
        for (int c = 0; c < nc; c++) {
            double p = t->chip_pass[b][c], f = t->chip_fail[b][c];
            jw_val_fmt(w, "%.2f", (p + f) > 0.0 ? f * 100.0 / (p + f) : 0.0);
        }
        jw_arr_close(w);
        snprintf(key, sizeof key, "C_%02d_%02d", b, 0);
        json_u32_list(w, key, t->chip_pass[b], nc);
        snprintf(key, sizeof key, "C_%02d_%02d", b, 1);
        json_u32_list(w, key, t->chip_fail[b], nc);
        snprintf(key, sizeof key, "GHSmm%02d", b);
        jw_key_arr(w, key);
        for (int c = 0; c < nc; c++) {
            double mh = 0.0;
            for (int l = 0; l < ASIC_PLL_POINTS; l++)
                mh += (double)t->chip_pllcnt[b][c][l] * t->chip_cf[b][c][l];
            jw_val_fmt(w, "%.2f", mh / 1000.0);
        }
        jw_arr_close(w);
    }
    jw_obj_close(w);
}

static void build_mm_json(const mc_api_stats *s, jwriter *w, int show_all)
{
    estats_derived d;
    estats_derive(s, &d);
    jw_key_obj(w, "MMID0Summary");
    json_sysinfo(w, s, &d);
    json_hashinfo(w, s, &d);
    jw_obj_close(w);
    jw_key_obj(w, "HBinfo");
    for (int b = 0; b < HBOARD_COUNT; b++)
        json_hb_info(w, &s->telem, b, show_all);
    jw_obj_close(w);
}

static void build_lite_json(const mc_api_stats *s, jwriter *w)
{
    const mc_api_telem *t = &s->telem;
    jw_key_obj(w, "MMID0Summary");
    jw_kv_str(w, "LVer", s->mm_version[0] ? s->mm_version : MINER_VERSION_UNKNOWN);
    jw_kv_str(w, "FW", FW_TYPE);
    jw_kv_str(w, "PVer", MINER_PROTO_VER);
    jw_kv_fmt(w, "STATE", "%d", (int)t->work_state);
    jw_kv_fmt(w, "MODE", "%d", s->workmode);
    jw_kv_raw(w, "LEVEL", "0");
    jw_kv_fmt(w, "GHSspd", "%f", t->ghs_spd_chip);
    jw_kv_fmt(w, "ITemp", "%d", ASIC_TEMP_NONE);
    jw_kv_fmt(w, "OTemp", "%d", t->board_temp);
    jw_kv_raw(w, "Limitemp", "0");
    jw_kv_fmt(w, "FanR", "%d", t->fan_pct);
    jw_kv_fmt(w, "Filter", "%d", s->filtermesh);
    if (CORE_PUB_LED)
        json_led(w, s);
    if (CORE_PUB_LCD) {
        jw_kv_fmt(w, "LcdOnoff", "%d", s->lcd_onoff);
        jw_kv_fmt(w, "LcdSwitch", "%d", s->lcd_show);
        jw_kv_fmt(w, "LcdBright", "%d", s->lcd_bright);
    }
    if (CORE_PUB_AUDIO)
        jw_kv_fmt(w, "Volume", "%d", s->volume);
    jw_kv_fmt(w, "Activation", "%d", s->activation);
    json_u32_list(w, "ECHU", s->hu_errcode, HBOARD_COUNT);
    jw_kv_fmt(w, "ECMM", "%u", s->mm_errcode);
    jw_kv_str(w, "SSID", s->ssid);
    jw_kv_fmt(w, "RSSI", "%d", s->rssi);
    jw_kv_fmt(w, "NetDevType", "%d", s->net_devtype);
    jw_obj_close(w);
}

size_t mc_api_stats_(const mc_api_stats *s, char *buf, size_t cap, int show_all, int with_pools)
{
    char status[256];
    mc_api_status_elem(status, sizeof status, s->when, 70, "AvalonMiner stats");
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_key_arr(&w, "STATS");
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATS", "0");
    jw_kv_str(&w, "ID", "AVALON0");
    jw_kv_fmt(&w, "Elapsed", "%u", s->elapsed);
    jw_kv_raw(&w, "Calls", "0");
    jw_kv_raw(&w, "Wait", "0.000000");
    jw_kv_raw(&w, "Max", "0.000000");
    jw_kv_raw(&w, "Min", "99999999.000000");
    build_mm_json(s, &w, show_all);
    jw_kv_raw(&w, "MM Count", "1");
    jw_kv_raw(&w, "Nonce Mask", "25");
    jw_obj_close(&w);
    for (int i = 0; with_pools && i < s->npools; i++) {
        const mc_api_pool *p = &s->pool[i];
        jw_val_fmt(&w,
                "{\"STATS\":%d,\"ID\":\"POOL%d\",\"Elapsed\":%u,\"Calls\":0,\"Wait\":0.000000,"
                "\"Max\":0.000000,\"Min\":99999999.000000,\"Pool Calls\":0,\"Pool Attempts\":0,"
                "\"Pool Wait\":0.000000,\"Pool Max\":0.000000,\"Pool Min\":99999999.000000,"
                "\"Pool Av\":0.000000,\"Work Had Roll Time\":false,\"Work Can Roll\":false,"
                "\"Work Had Expire\":false,\"Work Roll Time\":0,\"Work Diff\":%.8f,"
                "\"Min Diff\":%.8f,\"Max Diff\":%.8f,\"Min Diff Count\":1,\"Max Diff Count\":1,"
                "\"Times Sent\":%u,\"Bytes Sent\":%u,\"Times Recv\":%u,\"Bytes Recv\":%u,"
                "\"Net Bytes Sent\":%u,\"Net Bytes Recv\":%u}",
                i + 1, i, s->elapsed, p->work_diff, p->work_diff, p->work_diff, p->tx_cnt,
                p->tx_bytes, p->rx_cnt, p->rx_bytes, p->tx_bytes, p->rx_bytes);
    }
    jw_arr_close(&w);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

size_t mc_api_stats_text(const mc_api_stats *s, char *buf, size_t cap, int show_all, int with_pools)
{
    char mm[MC_API_MM_ID0_CAP];
    build_mm_id0(s, mm, sizeof mm, show_all);
    size_t n = 0;
    ap(buf, cap, &n,
            "STATUS=S,When=%u,Code=70,Msg=AvalonMiner stats,Description=AvalonMiner %s|"
            "STATS=0,ID=AVALON0,Elapsed=%u,Calls=0,Wait=0.000000,"
            "Max=0.000000,Min=99999999.000000,MM ID0=%s,MM Count=1,Nonce Mask=25|",
            s->when, AM_VER, s->elapsed, mm);
    if (with_pools) {
        for (int i = 0; i < s->npools; i++) {
            const mc_api_pool *p = &s->pool[i];
            ap(buf, cap, &n,
                    "STATS=%d,ID=POOL%d,Elapsed=%u,Calls=0,Wait=0.000000,Max=0.000000,"
                    "Min=99999999.000000,Pool Calls=0,Pool Attempts=0,Pool Wait=0.000000,"
                    "Pool Max=0.000000,Pool Min=99999999.000000,Pool Av=0.000000,"
                    "Work Had Roll Time=false,Work Can Roll=false,Work Had Expire=false,"
                    "Work Roll Time=0,Work Diff=%.8f,Min Diff=%.8f,Max Diff=%.8f,"
                    "Min Diff Count=1,Max Diff Count=1,Times Sent=%u,Bytes Sent=%u,"
                    "Times Recv=%u,Bytes Recv=%u,Net Bytes Sent=%u,Net Bytes Recv=%u|",
                    i + 1, i, s->elapsed, p->work_diff, p->work_diff, p->work_diff, p->tx_cnt,
                    p->tx_bytes, p->rx_cnt, p->rx_bytes, p->tx_bytes, p->rx_bytes);
        }
    }
    return n;
}

size_t mc_api_litestats_(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256];
    mc_api_status_elem(status, sizeof status, s->when, 70, "AvalonMiner stats");
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_key_arr(&w, "STATS");
    jw_obj_open(&w);
    build_lite_json(s, &w);
    jw_kv_raw(&w, "MM Count", "1");
    jw_kv_raw(&w, "Nonce Mask", "25");
    jw_obj_close(&w);
    jw_arr_close(&w);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

size_t mc_api_litestats_text(const mc_api_stats *s, char *buf, size_t cap)
{
    char mm[MC_API_MM_ID0_CAP];
    char safe_ssid[sizeof s->ssid];
    snprintf(safe_ssid, sizeof safe_ssid, "%s", s->ssid);
    for (char *c = safe_ssid; *c; c++)
        if (*c == ',' || *c == '|')
            *c = '?';
    build_lite_id0(s, mm, sizeof mm, safe_ssid);
    size_t n = 0;
    ap(buf, cap, &n,
            "STATUS=S,When=%u,Code=70,Msg=AvalonMiner stats,Description=AvalonMiner %s|"
            "MM ID0=%s,MM Count=1,Nonce Mask=25|",
            s->when, AM_VER, mm);
    return n;
}
