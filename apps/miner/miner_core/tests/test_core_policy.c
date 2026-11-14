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

#include "osif.h"
#include "miner_ipc.h"
#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "tst_stats_json.h"
#include "mc_api_time.h"
#include "board_sensors.h"

#include "../../tests/include/tst_check.h"

#if defined(BOARD_X3_H)
#define WANT_ASIC_TELEM_SZ  40u
#define WANT_CHIP_TELEM_SZ  2196u
#define WANT_LOCAL_WORK_OFF 2188u
#define WANT_HASH_SN_OFF    204u
#define WANT_PERIPH_SZ      552u
#define WANT_DNA_OFF        286u
#define WANT_TZ_OFF         502u
#define WANT_FILTERMESH_OFF 184u
#elif defined(BOARD_NANO3S_H)
#define WANT_ASIC_TELEM_SZ  36u
#define WANT_CHIP_TELEM_SZ  352u
#define WANT_LOCAL_WORK_OFF 348u
#define WANT_HASH_SN_OFF    192u
#define WANT_PERIPH_SZ      500u
#define WANT_DNA_OFF        233u
#define WANT_TZ_OFF         450u
#define WANT_FILTERMESH_OFF 172u
#else
#error "no IPC layout goldens for this board"
#endif

static int      g_vf_sends;     /* MSG_ASIC_VF messages sent */
static uint16_t g_vf_last;      /* freq[0] of the last MSG_ASIC_VF, MHz */
static char     g_log_err[512]; /* Text of the last OSIF_LOG_ERR message */
static int      g_log_err_n;    /* OSIF_LOG_ERR messages logged */

static board_power g_psu;             /* Reading board_power_read() returns */
static int         g_psu_rc;          /* Return code of board_power_read() */
static int         g_reads, g_writes; /* board_power_read() and board_power_set_volt() calls */

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
    if (type == MSG_ASIC_VF && data && len >= (int)sizeof(mc_asic_vf)) {
        mc_asic_vf vf;
        memcpy(&vf, data, sizeof vf);
        g_vf_last = vf.freq[0];
        g_vf_sends++;
    }
    return OSIF_OK;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;
    if (lvl != OSIF_LOG_ERR)
        return;
    g_log_err_n++;
    va_start(ap, fmt);
    vsnprintf(g_log_err, sizeof g_log_err, fmt, ap);
    va_end(ap);
}
int osif_log_op_sink_open(void)
{
    return -1;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}

int board_sensors_init(void)
{
    return 0;
}
int board_power_read(board_power *out)
{
    g_reads++;
    if (out)
        *out = g_psu;
    return g_psu_rc;
}
int board_power_set_volt(uint16_t mv)
{
    (void)mv;
    g_writes++;
    return 0;
}
int board_ntc_read(int *degc)
{
    if (degc)
        *degc = 30;
    return 0;
}
uint8_t dcdc_mv_to_reg(uint16_t mv)
{
    (void)mv;
    return 0;
}

#define main core_miner_main_unused
#include "core_miner_tu.h"
#undef main

int main(void)
{
    char *buf = malloc(MC_API_REPLY_CAP);
    if (!buf) {
        printf("FAILED (oom)\n");
        return 1;
    }
    mc_api_stats s;
    CHECK(ERR_MM_LAN_DAMAGED == 0x0002u && ERR_MM_POWER == 0x0004u && ERR_MM_FAN == 0x0008u &&
                    ERR_MM_IPLOST == 0x0010u && ERR_MM_OVERHOT == 0x0020u &&
                    ERR_MM_POOL_INACTIVE == 0x0040u && ERR_MM_INTERNUCLEAR == 0x0080u &&
                    ERR_MM_INLETHOT == 0x0100u && ERR_MM_RBOVERFLOW == 0x0200u &&
                    ERR_MM_HW_UNMATCH == 0x0400u && ERR_MM_SLAVE == 0x0800u &&
                    ERR_MM_AGING_FAILED == 0x2000u && ERR_HU_ENUM == 0x0002u &&
                    ERR_HU_COMM == 0x0004u && ERR_HU_MCU == 0x0008u && ERR_HU_LOST == 0x0010u,
            "ECMM and ECHU definitions hold their bit positions");

    const char mm_version[]   = "26091101_2138_1111111";
    const char asic_version[] = "26091102_2138_2222222";
    ctrl_ack_cb(MSG_MM_VERSION, mm_version, sizeof mm_version);
    api_snapshot(&s, NULL);
    CHECK(!strcmp(s.mm_version, mm_version),
            "snapshot receives the mm version through the control-ack callback");
    CHECK(s.pool_idle == 1, "POOLS is 1 without an active pool");
    g_pool_active     = 0;
    g_pool_st[0].dead = 1;
    api_snapshot(&s, NULL);
    CHECK(s.pool_idle == 1, "...and while the active pool is Dead");
    g_pool_st[0].dead = 0;
    api_snapshot(&s, NULL);
    CHECK(s.pool_idle == 0, "POOLS is 0 while the active pool is alive");
    g_pool_active = -1;
    ctrl_ack_cb(MSG_ASIC_VERSION, asic_version, sizeof asic_version);
    nonce_cb(MSG_MM_VERSION, asic_version, sizeof asic_version);
    api_snapshot(&s, NULL);
    CHECK(!strcmp(s.mm_version, mm_version),
            "wrong-channel version messages cannot replace the mm version");
    g_mm_version.received_at = (uint64_t)now_s() - 11;
    api_snapshot(&s, NULL);
    CHECK(!strcmp(s.mm_version, MINER_VERSION_UNKNOWN), "an expired version becomes unknown");
    ctrl_ack_cb(MSG_MM_VERSION, mm_version, sizeof mm_version - 1);
    api_snapshot(&s, NULL);
    CHECK(!strcmp(s.mm_version, MINER_VERSION_UNKNOWN), "malformed report cannot refresh expiry");
    memset(&g_mm_version, 0, sizeof g_mm_version);
    ctrl_ack_cb(MSG_MM_VERSION, mm_version, sizeof mm_version);
    api_snapshot(&s, NULL);
    CHECK(!strcmp(s.mm_version, mm_version),
            "periodic reports repopulate an empty cache after a core restart");

    api_snapshot(&s, NULL);
    CHECK(s.telem.fan_pct == 0 && s.telem.hb_count == 0,
            "before any MSG_MM_PERIPH the mm-owned fields are absent");
    CHECK(s.max_pout == 0 && s.cali_volt == 0 && s.cali_pll_start == 0 &&
                    s.cali_pll_interval == 0 && s.adj == 0,
            "...including the cali row: core resolves none of its own, "
            "so MPO[] and ATA<n>[] are empty until mm publishes its selection");
    CHECK(s.target_temp == FAN_TARGET_TEMP,
            "...except the fan target, seeded from the board's own boot target, "
            "so TarT[] is never 0 (got %d, want %d)",
            s.target_temp, (int)FAN_TARGET_TEMP);
#ifdef CAP_ACTIVATION
    CHECK(s.activation == 0, "before any MSG_MM_PERIPH Activation is 0");
#else
    CHECK(s.activation == 1, "without CAP_ACTIVATION Activation is 1");
#endif

    char tmsg[128];
    int  tcode = 0;
    core_setters_register(NULL);
    mc_api_time_run("time|get", tmsg, sizeof tmsg, &tcode);
    CHECK(tcode == 131 && !strcmp(tmsg, "Time set info: time t:"),
            "time get before any MSG_MM_PERIPH answers an empty zone (%s)", tmsg);

    mm_periph pp;
    memset(&pp, 0, sizeof pp);
    pp.valid     = 1;
    pp.fan_pct   = 37;
    pp.ledmode   = 0;
    pp.runeffect = 1;
    snprintf(pp.tz, sizeof pp.tz, "Asia/Shanghai");
    pp.softoff_rsn = MC_SOFTOFF_ASIC_OVERHOT;
    pp.hb_count    = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        pp.hb_outlet[b] = (int16_t)(50 + b);
        pp.hb_inlet[b]  = (int16_t)(40 + b);
    }
    ctrl_ack_cb(MSG_MM_PERIPH, &pp, (int)sizeof pp);
    api_snapshot(&s, NULL);
    CHECK(s.telem.fan_pct == 37 && s.telem.hb_count == HBOARD_COUNT && s.telem.hb_outlet[0] == 50 &&
                    s.telem.hb_inlet[0] == 40,
            "one push publishes the duty and both hashboard rows");
#ifdef CAP_ACTIVATION
    CHECK(s.activation == 0, "an mm push with activation 0 reads 0");
#else
    CHECK(s.activation == 1, "an mm push with activation 0 reads 1 without CAP_ACTIVATION");
#endif
    CHECK(s.telem.softoff_rsn == MC_SOFTOFF_ASIC_OVERHOT,
            "softoff_rsn propagates through the mm_periph latch (got %u)",
            (unsigned)s.telem.softoff_rsn);
    mc_api_time_run("time|get", tmsg, sizeof tmsg, &tcode);
    CHECK(tcode == 131 && !strcmp(tmsg, "Time set info: time t:Asia/Shanghai"),
            "time get answers the zone mm pushed in MSG_MM_PERIPH (%s)", tmsg);
    memset(pp.tz, 'A', sizeof pp.tz);
    ctrl_ack_cb(MSG_MM_PERIPH, &pp, (int)sizeof pp);
    mc_api_time_run("time|get", tmsg, sizeof tmsg, &tcode);
    CHECK(strlen(tmsg) == strlen("Time set info: time t:") + MM_TZ_NAME_CAP - 1,
            "an unterminated zone is cut at MM_TZ_NAME_CAP - 1 bytes (%s)", tmsg);
    pp.tz[0] = '\0';
    ctrl_ack_cb(MSG_MM_PERIPH, &pp, (int)sizeof pp);

    static asic_chip_telem act;
    memset(&act, 0, sizeof act);
    act.tm.in_work  = 1;
    act.tm.freq_mhz = 400;
    nonce_cb(MSG_ASIC_TELEM, &act, (int)sizeof act);
    api_snapshot(&s, NULL);
    CHECK(s.telem.valid == 1, "the asic cache is fresh too");
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    char sysstat[64];
    CHECK(sj_ok(buf) && !sj_str(buf, "SYSTEMSTATU", sysstat, sizeof sysstat) &&
                    !strncmp(sysstat, "Work: In Work,", 14),
            "historical softoff reason does not hide resumed hashing");

    g_chip_at = now_s() - 7;
    api_snapshot(&s, NULL);
    CHECK(s.telem.valid == 0, "with both producers gone, the asic cache expires (6 s window)");
    CHECK(s.telem.fan_pct == 37 && s.telem.hb_count == HBOARD_COUNT && s.telem.hb_outlet[0] == 50,
            "LATCH (as shipped): the mm cache does NOT — the same snapshot still "
            "serves mm's last duty and hashboard temperatures as current");

    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, "SYSTEMSTATU[Work: In Init, Hash Board: 0]"),
            "estats: absent live status is not inferred from historical softoff reason");
    CHECK(sj_has(buf, "TMax[-273]"), "estats: ...and its temperatures as the sentinel");
    CHECK(sj_has(buf, "FanR[37%]"),
            "estats: while mm's stale duty is still published as FanR[37%%]");
    {
        sj_val v;
        CHECK(sj_find(buf, "HBOTemp", &v) && !strncmp(v.val, "[50", 3) &&
                        (v.val[3] == ',' || v.val[3] == ']'),
                "estats: ...and its stale hashboard temperatures too (HBOTemp[0] is 50)");
    }

    g_chip_at = now_s() - 3600;
    api_snapshot(&s, NULL);
    CHECK(s.telem.fan_pct == 37 && s.telem.hb_count == HBOARD_COUNT,
            "...and still after an hour: nothing ever clears g_periph.valid");

    {
        mm_periph goodp;
        memset(&goodp, 0, sizeof goodp);
        goodp.valid    = 1;
        goodp.fan_pct  = 37;
        goodp.hb_count = HBOARD_COUNT;
        snprintf(goodp.hash_sn[0], sizeof goodp.hash_sn[0], "GOODSN0001");
        ctrl_ack_cb(MSG_MM_PERIPH, &goodp, (int)sizeof goodp);

        mm_periph shortp = goodp;
        shortp.fan_pct   = 99;
        snprintf(shortp.hash_sn[0], sizeof shortp.hash_sn[0], "SHORTSN999");
        g_log_err_n  = 0;
        g_log_err[0] = 0;
        ctrl_ack_cb(MSG_MM_PERIPH, &shortp, (int)sizeof shortp - 1);
        api_snapshot(&s, NULL);
        CHECK(s.telem.fan_pct == 37 && !strcmp(s.hash_sn[0], "GOODSN0001"),
                "one byte short and the snapshot is dropped WHOLE — the duty and the "
                "serial table both keep their previous values, so no field of a "
                "short push is ever half-applied (fan_pct %u, hash_sn[0] '%s')",
                (unsigned)s.telem.fan_pct, s.hash_sn[0]);
        char want[64];
        snprintf(want, sizeof want, "dropped, %d bytes, %d expected", (int)sizeof shortp - 1,
                (int)sizeof shortp);
        CHECK(g_log_err_n == 1 && strstr(g_log_err, "MSG_MM_PERIPH dropped") &&
                        strstr(g_log_err, want),
                "...and the drop arm reports it once, with the received and "
                "expected sizes (%d ERR line(s): %s)",
                g_log_err_n, g_log_err);
        ctrl_ack_cb(MSG_MM_PERIPH, &shortp, (int)sizeof shortp - 1);
        CHECK(g_log_err_n == 1,
                "...and the rate limiter swallows the next one inside the 600 s "
                "window: mm pushes at ~1 Hz, so an unlimited drop line would flush "
                "the log rotation in minutes (saw %d)",
                g_log_err_n);

        struct {
            mm_periph p;
            char      tail;
        } longp;
        memset(&longp, 0, sizeof longp);
        longp.p = shortp;
        ctrl_ack_cb(MSG_MM_PERIPH, &longp, (int)sizeof longp.p + 1);
        api_snapshot(&s, NULL);
        CHECK(s.telem.fan_pct == 37 && !strcmp(s.hash_sn[0], "GOODSN0001"),
                "one byte long is dropped WHOLE as well (fan_pct %u, hash_sn[0] '%s')",
                (unsigned)s.telem.fan_pct, s.hash_sn[0]);
    }

    CHECK(sizeof(asic_telem) == WANT_ASIC_TELEM_SZ,
            "base ASIC telemetry is 36 bytes with 1 fan and 40 bytes with 3 fans");
    CHECK(sizeof(asic_chip_telem) == WANT_CHIP_TELEM_SZ,
            "full ASIC heartbeat is 352 bytes on nano3s and 2196 bytes on x3; "
            "it carries no ASIC error mask");
    CHECK(offsetof(asic_chip_telem, local_work) == WANT_LOCAL_WORK_OFF,
            "local_work is the tail field, at offset 348 on nano3s and 2188 on x3");
    CHECK(offsetof(mm_periph, hash_sn) == WANT_HASH_SN_OFF,
            "MM snapshots keep the LCD/audio state extension ahead of the SN table");
    CHECK(sizeof(mm_periph) == WANT_PERIPH_SZ,
            "MM snapshots carry one hash_sn row per hashboard, the OTP DNA, the "
            "SELECTED cali row, the web password digest, the work mode / CALIALL "
            "bitmap, the [hwcfg] identity plus activation flag and the timezone — "
            "the struct GREW, so a mixed-binary deploy drops the whole snapshot "
            "and all four binaries must be swapped");
    CHECK(offsetof(mm_periph, dna) == WANT_DNA_OFF &&
                    offsetof(mm_periph, cali_max_pout) > offsetof(mm_periph, dna) &&
                    offsetof(mm_periph, workmode) > offsetof(mm_periph, webpass) &&
                    offsetof(mm_periph, prod) > offsetof(mm_periph, caliall) &&
                    offsetof(mm_periph, maxmode) > offsetof(mm_periph, model) &&
                    offsetof(mm_periph, tz) == WANT_TZ_OFF,
            "the cali row, the digest, the work mode / CALIALL pair, the "
            "hwcfg/activation trio, the PD mode ceiling and the timezone are all "
            "APPENDED at the tail, so no field an older binary knows about moves");
    CHECK(offsetof(mm_periph, filtermesh) == WANT_FILTERMESH_OFF,
            "MM display extension preserves the existing telemetry prefix");
    memset(&pp, 0, sizeof pp);
    pp.valid      = 1;
    pp.mm_errcode = ERR_MM_POWER;
    ctrl_ack_cb(MSG_MM_PERIPH, &pp, sizeof pp);
    memset(&act, 0, sizeof act);
    act.boards = HBOARD_COUNT;
    for (int board = 1; board < HBOARD_COUNT; board++)
        act.chips[board] = 12;
    nonce_cb(MSG_ASIC_TELEM, &act, sizeof act);
    api_snapshot(&s, NULL);
    CHECK(s.hu_errcode[0] == ERR_HU_ENUM, "zero-chip board emits ENUM");
    for (int board = 1; board < HBOARD_COUNT; board++)
        CHECK(s.hu_errcode[board] == 0, "healthy board %d has no ENUM", board);
    CHECK(s.mm_errcode == (ERR_MM_POWER | ERR_MM_SLAVE), "ECMM merges MM and slave errors");
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(sj_has(buf, "ECMM[2052]"), "merged ECMM reaches estats");
    act.chips[0] = 12;
    nonce_cb(MSG_ASIC_TELEM, &act, sizeof act);
    api_snapshot(&s, NULL);
    CHECK(s.hu_errcode[0] == 0 && s.mm_errcode == ERR_MM_POWER,
            "enumeration recovery clears ENUM and SLAVE, preserving MM error");
    ctrl_ack_cb(MSG_MM_PERIPH, &pp, sizeof pp - 1);
    api_snapshot(&s, NULL);
    CHECK(s.mm_errcode == ERR_MM_POWER,
            "core infers no INTERNUCLEAR of its own from a short or missing MM "
            "push; ECMM is what MM and enumeration report");
    ctrl_ack_cb(MSG_MM_PERIPH, &pp, sizeof pp);
    api_snapshot(&s, NULL);
    CHECK(s.mm_errcode == ERR_MM_POWER, "a valid MM push leaves ECMM unchanged");
    mc_mm_status status = { .state = MC_WORK_IDLE };
    ctrl_ack_cb(MSG_MM_STATUS, &status, sizeof status);
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(!sj_str(buf, "SYSTEMSTATU", sysstat, sizeof sysstat) &&
                    !strncmp(sysstat, "Work: In Idle,", 14),
            "live MM soft-off overrides stale ASIC work");
    status.state = MC_WORK_FAULT;
    ctrl_ack_cb(MSG_MM_STATUS, &status, sizeof status);
    api_snapshot(&s, NULL);
    mc_api_stats_(&s, buf, MC_API_REPLY_CAP, 0, 0);
    CHECK(!sj_str(buf, "SYSTEMSTATU", sysstat, sizeof sysstat) &&
                    !strncmp(sysstat, "Work: In Fault,", 15),
            "explicit fault state renders");
    status.state = 255;
    ctrl_ack_cb(MSG_MM_STATUS, &status, sizeof status);
    CHECK(g_mm_state == MC_WORK_FAULT, "unknown state does not overwrite valid state");
    status.state = MC_WORK_INIT;
    ctrl_ack_cb(MSG_MM_STATUS, &status, sizeof status - 1);
    CHECK(g_mm_state == MC_WORK_FAULT, "short status is ignored");
    ctrl_ack_cb(MSG_MM_STATUS, &status, sizeof status);
    act.tm.in_work = 1;
    nonce_cb(MSG_ASIC_TELEM, &act, sizeof act);
    api_snapshot(&s, NULL);
    CHECK(s.telem.work_state == MC_WORK_WORK, "resume accepts fresh ASIC work");

    free(buf);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
