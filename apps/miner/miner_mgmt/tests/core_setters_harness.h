/* SPDX-License-Identifier: MIT */
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

#ifndef CORE_SETTERS_HARNESS_H
#define CORE_SETTERS_HARNESS_H

#include "ctrl_cmds.h"
#include "core_ascset.h"
#include "core_ctrl.h"
#include "core_ident.h"
#include "core_softonoff.h"
#include "mc_api_identity.h"
#include "mc_api_server.h"
#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "mc_api_setter.h"
#include "mc_api_qrlogin.h"
#include "mc_api_bootby.h"
#include "miner_ipc.h"
#include "eeprom_dev.h"
#include "ctrl_args.h"
#include "cali.h"
#include "ini.h"
#include "ini_file.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "../../tests/include/tst_check.h"

#define CS_FIX(name) CORE_SETTERS_FIXTURE_DIR "/" name

#define DAY  0
#define DARK 1

osif_ipc_t        *g_ipc_core_mm;     /* IPC channel from core_miner to mm_miner, unused */
static int         sent_n, send_fail; /* MSG_CTRL_CMD sends captured; nonzero fails sends */
static mc_ctrl_cmd sent_cmd;          /* last MSG_CTRL_CMD payload sent */
static mc_ctrl_ack ack_reply;    /* MSG_CTRL_ACK answered to each send; seq is copied from it */
static int         ack_silent;   /* nonzero: no MSG_CTRL_ACK answers a send */
static int         ack_stale;    /* nonzero: an ACK with the previous seq comes first */
static char        g_oplog[512]; /* last OSIF_LOG_OP line */
static char        g_log[4096];  /* log lines of every level since a test last emptied it */
static int         stod_n;       /* settimeofday() call count */
static int         cfgwr_webpass_fail;         /* nonzero fails cfgwr_write_webpass() */
static int         cfgwr_fail;                 /* nonzero fails cfgwr_write_hwcfg() */
static int         cfgwr_activation_fail;      /* nonzero fails cfgwr_write_activation() */
static char        cfgwr_activation_path[256]; /* last cfgwr_write_activation() path */
static uint64_t    tst_now;
static uint64_t    tst_step = 60000;

static int         eeprom_touch_n; /* eeprom_init() plus eeprom_read() call count */
static char        g_hash_sn[HBOARD_COUNT][HASH_SN_LEN + 1]; /* stats hash_sn per board */
static const char *g_dna = "d";                              /* stats DNA, hex */
static int         g_ps[7];                                  /* stats PS[] fields */
static int         g_board_n, g_chip[HBOARD_COUNT]; /* stats board count; chips per board */
static int         g_workmode;                      /* stats work mode */
static int         g_maxmode;                       /* stats highest work mode */
static int         g_adj = 1;                       /* stats aging state */

osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (send_fail)
        return OSIF_ERR;
    if (type == MSG_CTRL_CMD && data && len >= (int)sizeof(mc_ctrl_cmd)) {
        sent_cmd = *(const mc_ctrl_cmd *)data;
        sent_n++;
        if (!ack_silent) {
            mc_ctrl_ack a = ack_reply;
            if (ack_stale) {
                mc_ctrl_ack old = { .result = MC_SET_OK, .seq = sent_cmd.seq - 1 };
                core_ctrl_ack(&old, (int)sizeof old);
            }
            a.seq = sent_cmd.seq;
            core_ctrl_ack(&a, (int)sizeof a);
        }
    }
    return OSIF_OK;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;
    size_t  n = strlen(g_log);
    if (n + 1 < sizeof g_log) {
        va_start(ap, fmt);
        vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
        va_end(ap);
        strncat(g_log, "\n", sizeof g_log - strlen(g_log) - 1);
    }
    if (lvl != OSIF_LOG_OP)
        return;
    va_start(ap, fmt);
    vsnprintf(g_oplog, sizeof g_oplog, fmt, ap);
    va_end(ap);
}

uint64_t osif_now_ms(void)
{
    return tst_now += tst_step;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv;
    (void)tz;
    stod_n++;
    return 0;
}

static int cfgwr_stub_set_webpass(ini_t *ini, void *ctx)
{
    return ini_set(ini, "syscfg", "webpass", (const char *)ctx);
}
int cfgwr_write_webpass(const char *hex, const char *path)
{
    if (cfgwr_webpass_fail)
        return -1;
    return ini_file_update(path, cfgwr_stub_set_webpass, (void *)hex);
}

static int cfgwr_stub_set_hwcfg(ini_t *ini, void *ctx)
{
    const char *const *pair = (const char *const *)ctx;
    if (ini_set(ini, "hwcfg", "prod", pair[0]) != 0)
        return -1;
    return ini_set(ini, "hwcfg", "model", pair[1]);
}
int cfgwr_write_hwcfg(const char *prod, const char *model, const char *path)
{
    const char *pair[2] = { prod, model };
    if (cfgwr_fail)
        return -1;
    return ini_file_update(path, cfgwr_stub_set_hwcfg, (void *)pair);
}

static int cfgwr_stub_set_activation(ini_t *ini, void *ctx)
{
    (void)ctx;
    return ini_set(ini, "dev_attr", "activation", "1");
}
int cfgwr_write_activation(const char *path)
{
    snprintf(cfgwr_activation_path, sizeof cfgwr_activation_path, "%s", path ? path : "");
    if (cfgwr_activation_fail)
        return -1;
    return ini_file_update(path, cfgwr_stub_set_activation, NULL);
}

int eeprom_init(void)
{
    eeprom_touch_n++;
    return 0;
}
int eeprom_read(uint8_t addr, uint8_t *pbuf, uint8_t data_len, uint8_t miner_id)
{
    (void)addr;
    (void)miner_id;
    eeprom_touch_n++;
    memset(pbuf, 0, data_len);
    return 0;
}

static const char *ascset(const char *tail, int ledmode)
{
    static char  buf[2048];
    char         req[1024];
    mc_api_stats s = { 0 };
    s.prod         = "Avalon Nano3s";
    s.version      = "v";
    s.dna          = g_dna;
    s.mac          = "m";
    s.ledmode      = ledmode;
    memcpy(s.telem.ps, g_ps, sizeof g_ps);
    s.telem.board_n = g_board_n;
    memcpy(s.telem.chip_n, g_chip, sizeof s.telem.chip_n);
    s.workmode = g_workmode;
    s.maxmode  = g_maxmode;
    s.adj      = g_adj;
    memcpy(s.hash_sn, g_hash_sn, sizeof s.hash_sn);
    snprintf(req, sizeof req, "ascset|0,%s", tail);
    sent_n = 0;
    memset(&sent_cmd, 0, sizeof sent_cmd);
    mc_api_dispatch_text(req, "ascset", &s, buf, sizeof buf);
    return buf;
}

static const char *hex_of(const char *s)
{
    static char   out[65];
    unsigned char d[SHA256_DIGEST_SIZE];
    sha256_ctx    c;
    sha256_init(&c);
    sha256_update(&c, s, strlen(s));
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
    return out;
}

#endif /* CORE_SETTERS_HARNESS_H */
