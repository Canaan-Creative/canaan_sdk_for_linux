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
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "osif.h"
#include "miner_ipc.h"
#include "pipeline.h"
#include "board_sensors.h"
#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "tst_stats_json.h"
#include "tst_board_expect.h"

#include "../../tests/include/tst_check.h"

#define MM_ERR(x) ((unsigned)((x) & ~(unsigned)ERR_MM_SLAVE))

#define WANT_GOOD \
    (s.filtermesh == 4 && s.rssi == -41 && s.net_devtype == 1 && MM_ERR(s.mm_errcode) == 0x0101 && \
            s.telem.fan_pct == 55 && s.telem.softoff_rsn == MC_SOFTOFF_ASIC_OVERHOT && \
            !strcmp(s.ssid, "goodmm"))

#define WANT_STALE \
    (s.filtermesh == 7 && s.rssi == -88 && s.net_devtype == 0 && MM_ERR(s.mm_errcode) == 0x0202 && \
            s.telem.fan_pct == 99 && s.telem.softoff_rsn == MC_SOFTOFF_POWER_FAULT && \
            !strcmp(s.ssid, "stalemm"))

/* Argument and result of an api_snapshot() taken on another thread */
struct snap_arg {
    mc_api_stats s;    /* Snapshot filled by the thread */
    mc_pipeline *pipe; /* Pipeline passed to api_snapshot() */
    volatile int done; /* Set once the snapshot has returned */
};

extern board_power stub_power;    /* Reading the stub board_power_read() returns */
extern int         stub_power_rc; /* Return code of the stub board_power_read() */

static char          log_last[1024]; /* Text of the last osif_log() message */
static int           log_err_n;      /* OSIF_LOG_ERR messages logged */
static mc_core_stats cs_last;        /* Last MSG_CORE_STATS payload sent */

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
    if (type == MSG_CORE_STATS && data && len == (int)sizeof cs_last)
        memcpy(&cs_last, data, sizeof cs_last);
    return OSIF_OK;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_last, sizeof log_last, fmt, ap);
    va_end(ap);
    if (lvl == OSIF_LOG_ERR)
        log_err_n++;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

#define main core_miner_main_unused
#include "core_miner_tu.h"
#undef main

static void *snap_thread(void *arg)
{
    struct snap_arg *a = arg;

    api_snapshot(&a->s, a->pipe);
    a->done = 1;
    return NULL;
}

static void periph_push(void)
{
    if (stub_power_rc != 0)
        return;
    g_periph.valid         = 1;
    g_periph.ps_valid      = 1;
    g_periph.ps_err        = stub_power.err;
    g_periph.ps_vout_mv    = stub_power.vout_mv;
    g_periph.ps_iout_ma    = stub_power.iout_ma;
    g_periph.ps_pout_w     = stub_power.pout_w;
    g_periph.ps_voutcmd_mv = stub_power.voutcmd_mv;
}

int main(void)
{
    printf("descriptor %s: %d fan(s)\n", BOARDCONF_SECTION, (int)FAN_COUNT);

    char span[64], want_span[80], want_absent[16], want_ps[40], want_ps_0[40];
    char want_crc[32], want_comcrc[32], want_crc2[32], want_comcrc2[32];
    snprintf(want_span, sizeof want_span, "%s FanR[", tst_fan_span(span, sizeof span));
    snprintf(want_absent, sizeof want_absent, "Fan%d", FAN_COUNT + 1);
    snprintf(want_ps, sizeof want_ps, "PS[80 0 1908 %d 0 1910 1636]", 86 / PS_IOUT_PER_A);
    snprintf(want_ps_0, sizeof want_ps_0, "PS[0 0 1908 %d 0 1910 1636]", 86 / PS_IOUT_PER_A);
    tst_board_list(want_crc, sizeof want_crc, " CRC", 14);
    tst_board_list(want_comcrc, sizeof want_comcrc, " COMCRC", 5);
    tst_board_list(want_crc2, sizeof want_crc2, " CRC", 21);
    tst_board_list(want_comcrc2, sizeof want_comcrc2, " COMCRC", 9);

    asic_telem tm;
    memset(&tm, 0, sizeof tm);
    tm.freq_mhz = 416;
    tm.in_work  = 1;
    tm.temp_max = 60;
    tm.temp_avg = 55;

    g_periph.valid = 1;
    for (int f = 0; f < FAN_COUNT; f++)
        g_periph.fan_rpm[f] = (uint16_t)(1460 + 10 * f);

    static asic_chip_telem ct;
    memset(&ct, 0, sizeof ct);
    ct.tm = tm;
    nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);

    mc_api_stats s;
    api_snapshot(&s, NULL);
    CHECK(s.telem.valid, "the snapshot accepted a %d-byte MSG_ASIC_TELEM", (int)sizeof ct);

    char *buf = malloc(MC_API_REPLY_CAP);
    if (!buf) {
        printf("FAILED (oom)\n");
        return 1;
    }
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);

    CHECK(sj_ok(buf) && sj_has(buf, want_span), "estats carries every fan's own RPM: \"%s..\"",
            want_span);
    CHECK(!sj_key(buf, want_absent), "...and no field for a fan this board does not have (\"%s\")",
            want_absent);

    for (int f = 0; f < FAN_COUNT; f++)
        g_periph.fan_rpm[f] = (uint16_t)(f == 0 ? 0 : 1470);
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
#if defined(BOARD_X3_H)
    CHECK(sj_has(buf, "Fan1[0] Fan2[1470] Fan3[1470] FanR["),
            "a dead tach zeroes its own fan and leaves the others reading");
#elif defined(BOARD_NANO3S_H)
    CHECK(sj_has(buf, "Fan1[0] FanR["), "a dead tach reports 0, as the field has always meant");
#else
#error "no dead-tach fan goldens for this board"
#endif

    stub_power.vout_mv    = 1908;
    stub_power.iout_ma    = 86;
    stub_power.pout_w     = 1636;
    stub_power.voutcmd_mv = 1910;
    stub_power.err        = 0x0050;
    stub_power_rc         = 0;
    periph_push();
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, want_ps), "the PSU fault word reaches estats PS[0]: \"%s\"", want_ps);

    stub_power.err = 0x0001;
    stub_power_rc  = -1;
    periph_push();
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, want_ps),
            "a failed rail read keeps the last good PS[..], fault word included");

    stub_power.err = 0;
    stub_power_rc  = 0;
    periph_push();
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, want_ps_0), "no fault latched -> PS[0] is the same 0 as before: \"%s\"",
            want_ps_0);

    g_periph.ps_valid = 0;
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, "PS[0 0 0 0 0 0 0]"), "a stale rail cache publishes no fault word");

    ct.boards     = HBOARD_COUNT;
    ct.err_crc[0] = 14;
    ct.com_crc[0] = 5;
    nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, want_crc) && sj_has(buf, want_comcrc),
            "the tallies reach estats: \"%s\" \"%s\"", want_crc, want_comcrc);

    nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, want_crc) && sj_has(buf, want_comcrc),
            "the SAME cumulative datagram again reports the SAME counts, not "
            "double them: \"%s\" \"%s\"",
            want_crc, want_comcrc);

    ct.err_crc[0] = 21;
    ct.com_crc[0] = 9;
    nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, want_crc2) && sj_has(buf, want_comcrc2),
            "a grown tally is reported as the producer states it: \"%s\" \"%s\"", want_crc2,
            want_comcrc2);

    {
        static uint8_t  big[sizeof(asic_chip_telem) + 4];
        asic_chip_telem other = ct;
        other.err_crc[0]      = 99;
        memcpy(big, &other, sizeof other);
        nonce_cb(MSG_ASIC_TELEM, big, (int)sizeof big);
        api_snapshot(&s, NULL);
        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        CHECK(sj_has(buf, want_crc2),
                "a MSG_ASIC_TELEM longer than asic_chip_telem is dropped: \"%s\"", want_crc2);
    }

    ct.err_crc[0] = 0;
    ct.com_crc[0] = 0;
    nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    tst_board_list(want_crc, sizeof want_crc, "CRC", 0);
    tst_board_list(want_comcrc, sizeof want_comcrc, "COMCRC", 0);
    CHECK(sj_has(buf, want_crc) && sj_has(buf, want_comcrc),
            "and a bring-up that zeroed them at the producer zeroes them here — "
            "the consumer holds no tally of its own");

    {
        avalon_nonce_out no;
        memset(&no, 0, sizeof no);
        no.valid  = 1;
        no.job_id = 1;
        for (int b = 0; b < HBOARD_COUNT; b++)
            for (int k = 0; k <= b; k++) {
                no.miner_id = (uint16_t)b;
                nonce_cb(MSG_ASIC_NONCE, &no, (int)sizeof no);
            }
        no.miner_id = (uint16_t)(HBOARD_COUNT + 5);
        nonce_cb(MSG_ASIC_NONCE, &no, (int)sizeof no);

        api_snapshot(&s, NULL);
        uint32_t sum   = 0;
        int      shape = 1;
        for (int b = 0; b < HBOARD_COUNT; b++) {
            sum += s.hw_board[b];
            if (s.hw_board[b] != (uint32_t)(b + 1))
                shape = 0;
        }
        CHECK(shape,
                "each board carries its own MH tally, and a nonce with an "
                "out-of-range miner_id is charged to no board at all (%u)",
                s.hw_board[0]);
        CHECK(sum == (uint32_t)(HBOARD_COUNT * (HBOARD_COUNT + 1) / 2),
                "MH[..] leaves the unattributable nonce out (%u)", sum);
        CHECK(s.hw_errors == sum + 1, "...and \"Hardware Errors\" counts it (%u vs %u)",
                s.hw_errors, sum);

        char want[64];
        snprintf(want, sizeof want, " HW[%u] DH[", sum);
        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        CHECK(sj_has(buf, want), "estats HW[] is the MH[..] sum, not the scalar: \"%s\"", want);
    }

    {
        uint32_t wgf = 0, wrf = 0;
        pthread_mutex_lock(&g_stats_lock);
        for (int i = 0; i < POOL_VALID_NUM; i++) {
            g_pool_st[i].getfail = (uint32_t)(3 + i);
            g_pool_st[i].rfail   = (uint32_t)(1 + i);
            wgf += (uint32_t)(3 + i);
            wrf += (uint32_t)(1 + i);
        }
        uint32_t wpf   = wgf + 7;
        g_pool_failcnt = wpf;
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        CHECK(s.get_failures == wgf, "Get Failures sums every pool slot (%u vs %u)", s.get_failures,
                wgf);
        CHECK(s.remote_failures == wrf, "Remote Failures likewise (%u vs %u)", s.remote_failures,
                wrf);
        char want[48], notwant[48];
        snprintf(want, sizeof want, " PFCnt[%u] NETFAIL[", wpf);
        snprintf(notwant, sizeof notwant, " PFCnt[%u] NETFAIL[", wgf);
        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        CHECK(sj_has(buf, want), "estats PFCnt carries pool_failcnt, not Get Failures: \"%s\"",
                want);
        CHECK(!sj_has(buf, notwant), "estats PFCnt is not the Get Failures sum: \"%s\"", notwant);
    }

    {
        const char *bh = "00000000000000000001cb4150b955d4"
                         "7fd97be3d968826bb45111a859f51935";
        pthread_mutex_lock(&g_stats_lock);
        snprintf(g_block.hash, sizeof g_block.hash, "%s", bh);
        g_block.net_diff = 133869853540305.39;
        g_block.time     = now_mono_s() - 592.181022;
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        CHECK(s.block_hash && !strcmp(s.block_hash, bh),
                "the snapshot copies the current block hash out (%s)",
                s.block_hash ? s.block_hash : "(null)");
        CHECK(s.net_difficulty == 133869853540305.39 && s.block_time >= 592.0 &&
                        s.block_time <= 593.0,
                "...with its network difficulty and its block age (%.3f s)", s.block_time);
    }

    {
        pthread_mutex_lock(&g_stats_lock);
        g_dev_start = 0;
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        CHECK(s.dev_elapsed >= 1.0 && s.dev_elapsed == (double)s.elapsed,
                "with no diff-1 nonce yet the device clock is the session one "
                "(%.0f vs %u)",
                s.dev_elapsed, s.elapsed);
        pthread_mutex_lock(&g_stats_lock);
        g_start     = now_s() - 600;
        g_dev_start = now_s() - 120;
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        CHECK(s.elapsed == 600 && s.dev_elapsed == 120.0,
                "the device clock runs from its first nonce, not the session start "
                "(%u / %.0f)",
                s.elapsed, s.dev_elapsed);
        char want[64];
        snprintf(want, sizeof want, "\"Device Elapsed\":120}");
        mc_api_devs(&s, buf, MC_API_REPLY_CAP);
        CHECK(strstr(buf, want) != NULL, "devs reports it: \"%s\"", want);
    }

    {
        pthread_mutex_lock(&g_stats_lock);
        g_last_valid_work = 1787128137u;
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        CHECK(s.last_valid_work == 1787128137u, "the snapshot publishes the diff-1 stamp (%u)",
                s.last_valid_work);
    }

    {
        pthread_mutex_lock(&g_stats_lock);
        for (int b = 0; b < HBOARD_COUNT; b++)
            g_board_diff1[b] = 4096.0 * (b + 1);
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        int shape = 1;
        for (int b = 0; b < HBOARD_COUNT; b++)
            if (s.diff1_board[b] != 4096.0 * (b + 1))
                shape = 0;
        CHECK(shape, "each board's own diff-1 credit is copied out (%.0f)", s.diff1_board[0]);
    }

    {
        pthread_mutex_lock(&g_stats_lock);
        g_periph.valid = 1;
        snprintf(g_periph.ssid, sizeof g_periph.ssid, "%s", "labwifi");
        g_periph.rssi          = -53;
        g_periph.net_devtype   = 1;
        g_periph.lcd_bright    = 70;
        g_periph.lcd_bright_hi = 1;
        g_periph.lcd_onoff     = 0;
        g_periph.lcd_show      = 3;
        g_periph.volume        = 25;
        g_periph.led_effect    = 3;
        g_periph.led_bright    = 42;
        g_periph.led_temper    = 1234;
        g_periph.led_rgb       = 0x010203;
        g_periph.filtermesh    = 70000;
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        if (CORE_PUB_LED) {
            CHECK(s.led_effect == 3 && s.led_bright == 42 && s.led_temper == 1234 &&
                            s.led_rgb == 0x010203,
                    "LEDUser comes from mm live snapshot");
        }
        int lcd_copy = CORE_PUB_LCD ? (s.lcd_bright == 326 && s.lcd_onoff == 0 && s.lcd_show == 3) :
                                      (s.lcd_bright == 0 && s.lcd_onoff == 0 && s.lcd_show == 0);
        CHECK(!strcmp(s.ssid, "labwifi") && s.rssi == -53 && s.net_devtype == 1 && lcd_copy &&
                        s.filtermesh == 70000,
                "api_snapshot copies the litestats fields out of g_periph, "
                "filtermesh at a value PAST 255: it is cumulative filter-mesh "
                "run-minutes and a uint32_t on the wire, and an expectation "
                "under 256 would not catch a narrowing of that field");
        if (CORE_PUB_AUDIO) {
            CHECK(s.volume == 25, "api_snapshot copies live volume");
        }
        mc_api_litestats_text(&s, buf, MC_API_REPLY_CAP);
        if (CORE_PUB_LCD) {
            CHECK(strstr(buf, "LcdOnoff[0] LcdSwitch[3]") != NULL,
                    "litestats publishes live LCD state");
        }
        if (CORE_PUB_AUDIO) {
            CHECK(strstr(buf, "Volume[25]") != NULL, "litestats publishes live volume");
        }
        CHECK(strstr(buf, "Filter[70000] ") != NULL &&
                        (strstr(buf, "LcdBright[326] ") != NULL) == (CORE_PUB_LCD != 0) &&
                        strstr(buf, "SSID[labwifi] RSSI[-53] NetDevType[1]") != NULL,
                "the copied wifi/filter/lcd fields reach the litestats wire string, "
                "and a board with no LCD emits no LcdBright at all. Filter[] is "
                "asserted past 255 so a narrowed wire field renders Filter[112] "
                "here instead of passing");
    }

    {
        char   wide[sizeof g_periph.ssid], want[sizeof g_periph.ssid + 32];
        size_t w = 0;
        while (w + 4 < sizeof wide) {
            memcpy(wide + w, "\\xe4", 4);
            w += 4;
        }
        while (w < sizeof wide - 1)
            wide[w++] = 'Z';
        wide[w] = '\0';

        pthread_mutex_lock(&g_stats_lock);
        g_periph.valid = 1;
        snprintf(g_periph.ssid, sizeof g_periph.ssid, "%s", wide);
        pthread_mutex_unlock(&g_stats_lock);
        api_snapshot(&s, NULL);
        CHECK(strlen(s.ssid) == sizeof wide - 1 && !strcmp(s.ssid, wide),
                "a full-width SSID crosses mm_periph -> mc_api_stats whole: "
                "`wpa_cli status` reports the SSID in wpa_supplicant's escaped "
                "form, where one byte becomes \\xNN, so 32 raw bytes are up to "
                "%zu characters. A narrower field cuts mid-escape and the phone "
                "app's utf8.decode then fails on the tail (got %zu char(s))",
                sizeof wide - 1, strlen(s.ssid));

        mc_api_litestats_text(&s, buf, MC_API_REPLY_CAP);
        snprintf(want, sizeof want, "SSID[%s] RSSI[", wide);
        CHECK(strstr(buf, want) != NULL,
                "...and reaches the litestats wire string whole, with the field "
                "after it still present — MC_API_MM_ID0_CAP has room for it");
    }

    {
        mm_periph good, stale;
        int       err_before;

        memset(&good, 0, sizeof good);
        good.valid       = 1;
        good.fan_pct     = 55;
        good.mm_errcode  = 0x0101;
        good.softoff_rsn = MC_SOFTOFF_ASIC_OVERHOT;
        good.rssi        = -41;
        good.net_devtype = 1;
        good.filtermesh  = 4;
        good.hb_count    = HBOARD_COUNT;
        snprintf(good.ssid, sizeof good.ssid, "%s", "goodmm");

        stale             = good;
        stale.fan_pct     = 99;
        stale.mm_errcode  = 0x0202;
        stale.softoff_rsn = MC_SOFTOFF_POWER_FAULT;
        stale.rssi        = -88;
        stale.net_devtype = 0;
        stale.filtermesh  = 7;
        snprintf(stale.ssid, sizeof stale.ssid, "%s", "stalemm");

        err_before = log_err_n;
        ctrl_ack_cb(MSG_MM_PERIPH, &good, (int)sizeof good);
        CHECK(log_err_n == err_before,
                "a full-length MSG_MM_PERIPH (%d bytes) is accepted in silence", (int)sizeof good);
        api_snapshot(&s, NULL);
        CHECK(WANT_GOOD,
                "...and every mm-owned field it carries reaches the "
                "snapshot");

        err_before = log_err_n;
        ctrl_ack_cb(MSG_MM_PERIPH, &stale, (int)sizeof stale - 1);
        CHECK(log_err_n == err_before + 1,
                "a short MSG_MM_PERIPH is reported exactly once, at ERR");
        CHECK(strstr(log_last, "MSG_MM_PERIPH dropped") != NULL &&
                        strstr(log_last, "expected") != NULL,
                "...and the line names the message and the expected length");
        api_snapshot(&s, NULL);
        CHECK(WANT_GOOD,
                "...and NOT ONE of its fields lands: the last good "
                "snapshot stands unchanged");

        err_before = log_err_n;
        ctrl_ack_cb(MSG_MM_PERIPH, &stale, (int)sizeof stale - 1);
        CHECK(log_err_n == err_before, "a second short push inside the repeat window logs nothing");
        api_snapshot(&s, NULL);
        CHECK(WANT_GOOD,
                "...and is dropped just the same — the rate limit "
                "gates the diagnostic, not the discard");

        {
            char big[sizeof(mm_periph) + 8];
            memset(big, 0xAB, sizeof big);
            memcpy(big, &stale, sizeof stale);
            ctrl_ack_cb(MSG_MM_PERIPH, big, (int)sizeof big);
            api_snapshot(&s, NULL);
            CHECK(WANT_GOOD, "a push %d bytes longer than expected is dropped too",
                    (int)(sizeof big - sizeof(mm_periph)));
        }
        ctrl_ack_cb(MSG_MM_PERIPH, &stale, (int)sizeof stale);
        api_snapshot(&s, NULL);
        CHECK(WANT_STALE, "...and the next full-length push is accepted");
    }

    {
        mc_mm_bootby bb;

        api_snapshot(&s, NULL);
        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        CHECK(sj_has(buf, "BOOTBY[0x01.00000000]"),
                "until mm says otherwise estats reports the MC_BOOTBY_POWRON "
                "default, so the assertion below measures the IPC and not the "
                "initialiser");

        bb.reason = MC_BOOTBY_SYS;
        bb.sub    = MC_BOOTBY_SUB_WDT;
        ctrl_ack_cb(MSG_MM_BOOTBY, &bb, (int)sizeof bb);
        api_snapshot(&s, NULL);
        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        CHECK(sj_has(buf, "BOOTBY[0x06.000000F1]"),
                "MSG_MM_BOOTBY is the ONLY thing that puts a boot reason on the "
                "wire — reason and subreason do not reach core_miner through "
                "ini keys — and the reason renders %%02X uppercase, the "
                "subreason %%08X");

        bb.reason = MC_BOOTBY_UPGRADE;
        bb.sub    = 0x12345678u;
        ctrl_ack_cb(MSG_MM_BOOTBY, &bb, (int)sizeof bb - 1);
        api_snapshot(&s, NULL);
        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        CHECK(sj_has(buf, "BOOTBY[0x06.000000F1]"),
                "...and a SHORT datagram is dropped whole: the previous reason "
                "stands rather than half a new one landing");
    }

    {
        cali_param            own, row0_low;
        mm_periph             mp;
        static const uint16_t chip_counts[] = { (uint16_t)MINER_MAX_CHIPS, 0,
            (uint16_t)(MINER_MAX_CHIPS - 1), (uint16_t)(MINER_MAX_CHIPS + 1) };

        cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, CALI_MODE_DEFAULT, 0, &own, NULL);
        row0_low = *cali_default_param(0, 0);

        memset(&mp, 0, sizeof mp);
        mp.valid             = 1;
        mp.hb_count          = HBOARD_COUNT;
        mp.cali_max_pout     = own.max_pout;
        mp.cali_temp         = own.temp;
        mp.cali_volt         = own.volt;
        mp.cali_pll_start    = own.pll_start;
        mp.cali_pll_interval = own.pll_interval;
        mp.cali_aged         = 1;
        mp.workmode          = 2;
        mp.caliall           = 0x05;
        ctrl_ack_cb(MSG_MM_PERIPH, &mp, (int)sizeof mp);

        api_snapshot(&s, NULL);
        CHECK(s.workmode == 2 && s.caliall == 0x05,
                "WORKMODE[] and CALIALL[] come from mm's snapshot, not from a "
                "core-side read of systemcfg.ini and "
                "/data/factory/hashrate_cali.ini (got %d / 0x%02x)",
                s.workmode, (unsigned)s.caliall);

        for (unsigned i = 0; i < sizeof chip_counts / sizeof chip_counts[0]; i++) {
            ct.chips[0] = chip_counts[i];
            nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);
            api_snapshot(&s, NULL);
            CHECK(s.max_pout == (int)own.max_pout && s.target_temp == (int)own.temp &&
                            s.cali_volt == (int)own.volt &&
                            s.cali_pll_start == (int)own.pll_start &&
                            s.cali_pll_interval == (int)own.pll_interval,
                    "the row on the wire is the one mm PUBLISHED, unmoved "
                    "at %u enumerated chips: re-deriving here would make "
                    "MPO[]/TarT[]/ATA[] a function of a "
                    "runtime enumeration; mm resolves on g_board.max_asic for its "
                    "own fan and rail actuation and core renders that "
                    "selection (MPO[%d] TarT[%d] %d-%d-%d)",
                    (unsigned)chip_counts[i], s.max_pout, s.target_temp, s.cali_volt,
                    s.cali_pll_start, s.cali_pll_interval);
        }

        ct.chips[0] = 0;
        nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);
        api_snapshot(&s, NULL);
        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        {
            char want[64], reject[64];
            snprintf(want, sizeof want, " MPO[%d]", (int)own.max_pout);
            snprintf(reject, sizeof reject, " MPO[%d]", (int)row0_low.max_pout);
            CHECK(sj_has(buf, want),
                    "estats MPO[] carries the board's own row on a zero "
                    "enumeration: \"%s\"",
                    want);
            CHECK(own.max_pout == row0_low.max_pout || !sj_has(buf, reject),
                    "...and never the table's row-0 mode 0 value \"%s\"", reject);
            snprintf(want, sizeof want, " TarT[%d]", (int)own.temp);
            snprintf(reject, sizeof reject, " TarT[%d]", (int)row0_low.temp);
            CHECK(sj_has(buf, want), "the fan target estats publishes is the same row: \"%s\"",
                    want);
            CHECK(own.temp == row0_low.temp || !sj_has(buf, reject),
                    "...and not row-0 mode 0's \"%s\" — mm_fan/mm_vf actuate on "
                    "g_board.max_asic, so this reporting path must name the row "
                    "the machine is actually running",
                    reject);
        }

        mp.cali_max_pout     = 4001;
        mp.cali_temp         = 61;
        mp.cali_volt         = 2049;
        mp.cali_pll_start    = 313;
        mp.cali_pll_interval = 7;
        mp.cali_aged         = 1;
        ctrl_ack_cb(MSG_MM_PERIPH, &mp, (int)sizeof mp);
        ct.chips[0] = MINER_MAX_CHIPS;
        nonce_cb(MSG_ASIC_TELEM, &ct, (int)sizeof ct);
        api_snapshot(&s, NULL);
        CHECK(s.max_pout == 4001 && s.target_temp == 61 && s.cali_volt == 2049 &&
                        s.cali_pll_start == 313 && s.cali_pll_interval == 7,
                "a row NO factory table entry and no hashrate_cali.ini record "
                "could produce crosses mm_periph -> mc_api_stats verbatim — this "
                "is the assertion a restored cali_resolve() in api_snapshot "
                "fails, because it would overwrite all five fields with a "
                "table/ini row (MPO[%d] TarT[%d] %d-%d-%d)",
                s.max_pout, s.target_temp, s.cali_volt, s.cali_pll_start, s.cali_pll_interval);

        mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
        {
            char want[64];
            CHECK(sj_has(buf, "MPO[4001]"), "...and MPO[] on the wire is mm's published max_pout");
            CHECK(sj_has(buf, "TarT[61]"), "...and TarT[] is mm's published fan target");
            snprintf(want, sizeof want, " ATA%d[4001-61-2049-313-7]", s.workmode);
            CHECK(sj_has(buf, want),
                    "...and the ATA<mode> cali tuple is the same row, all five "
                    "fields in order: \"%s\"",
                    want);
        }

        mp.cali_aged      = 0;
        mp.aging_finished = 0;
        ctrl_ack_cb(MSG_MM_PERIPH, &mp, (int)sizeof mp);
        api_snapshot(&s, NULL);
        CHECK(s.adj == 0,
                "ADJ[0] when mm reports the row came from the factory table "
                "(cali_aged 0) and aging has not finished (got %d)",
                s.adj);
        mp.cali_aged = 1;
        ctrl_ack_cb(MSG_MM_PERIPH, &mp, (int)sizeof mp);
        api_snapshot(&s, NULL);
        CHECK(s.adj == 1,
                "ADJ[1] when mm reports a persisted calibration was resolved "
                "(got %d)",
                s.adj);
        mp.aging_finished = 1;
        ctrl_ack_cb(MSG_MM_PERIPH, &mp, (int)sizeof mp);
        api_snapshot(&s, NULL);
        CHECK(s.adj == 2,
                "ADJ[2] when aging finished in this run — both inputs are "
                "mm's, so ADJ cannot disagree with the row beside it "
                "(got %d)",
                s.adj);
    }

    {
        atomic_store(&g_main_beat_ms, now_ms() + 5000);
        memset(&cs_last, 0, sizeof cs_last);
        core_stats_push();
        CHECK(!(cs_last.core_health & CORE_UNHEALTHY_MAIN_STALL),
                "a main beat stamped after the push read its clock is not a "
                "stall (core_health 0x%x)",
                cs_last.core_health);
        atomic_store(&g_main_beat_ms, 0);
    }

    {
        static struct snap_arg a;
        pthread_t              th;

        pthread_mutex_lock(&g_stats_lock);
        g_periph.valid    = 1;
        g_periph.workmode = 0;
        g_periph.maxmode  = 0;
        g_periph.caliall  = 0;
        g_start           = now_s();
        g_bootby_reason   = MC_BOOTBY_POWRON;
        g_bootby_sub      = 0;
        a.pipe            = NULL;
        a.done            = 0;
        pthread_create(&th, NULL, snap_thread, &a);
        {
            struct timespec ts = { 0, 200 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        int blocked       = !a.done;
        g_periph.workmode = 2;
        g_periph.maxmode  = 2;
        g_periph.caliall  = 1;
        g_start           = now_s() - 300;
        g_bootby_reason   = MC_BOOTBY_API;
        g_bootby_sub      = 0x5au;
        pthread_mutex_unlock(&g_stats_lock);
        pthread_join(th, NULL);
        CHECK(blocked && a.s.workmode == 2 && a.s.maxmode == 2 && a.s.caliall == 1 &&
                        a.s.elapsed >= 299,
                "the snapshot reads g_start and g_periph modes under "
                "g_stats_lock (blocked %d, workmode %u maxmode %u caliall %u "
                "elapsed %u)",
                blocked, (unsigned)a.s.workmode, (unsigned)a.s.maxmode, (unsigned)a.s.caliall,
                a.s.elapsed);
        CHECK(a.s.bootby_reason == MC_BOOTBY_API && a.s.bootby_sub == 0x5au,
                "the snapshot reads the boot reason under g_stats_lock "
                "(reason 0x%02x sub 0x%x)",
                (unsigned)a.s.bootby_reason, (unsigned)a.s.bootby_sub);
    }

    {
        static struct snap_arg a;
        static mc_pipeline     pl;
        pthread_t              th;
        struct timespec        ts = { 0, 200 * 1000 * 1000 };

        mc_pipeline_init(&pl);
        pthread_mutex_lock(&g_stratum.lock);
        a.pipe = &pl;
        a.done = 0;
        pthread_create(&th, NULL, snap_thread, &a);
        nanosleep(&ts, NULL);
        int blocked       = !a.done;
        pl.tally.accepted = 7;
        pl.tally.rejected = 3;
        pl.dups           = 2;
        pl.sv1.stale      = 5;
        pthread_mutex_unlock(&g_stratum.lock);
        pthread_join(th, NULL);
        CHECK(blocked && a.s.accepted == 7 && a.s.rejected == 3 && a.s.dhw == 2 && a.s.stale == 5,
                "the snapshot reads the pipeline tallies under g_stratum.lock "
                "(blocked %d, %u/%u/%u/%u)",
                blocked, a.s.accepted, a.s.rejected, a.s.dhw, a.s.stale);
    }

    free(buf);
    return tst_done();
}
