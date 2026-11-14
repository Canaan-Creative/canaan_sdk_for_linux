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

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "status.h"

#include <string.h>

#define MW_STR           64            /* dashboard string field, bytes */
#define MW_URL           257           /* pool URL field, bytes */
#define MW_CREDENTIAL    129           /* pool worker field, bytes */
#define MW_RPC_REPLY_CAP (32u * 1024u) /* core API reply buffer, bytes */

/* dashboard telemetry read from the core API, with the strings dash points into */
typedef struct {
    /* dashboard view of the fields below */
    mc_dashboard dash;
    /* model, uptime s, work mode, power */
    char hwtype[MW_STR], elapsed[MW_STR], workingmode[MW_STR], power[MW_STR];
    /* hashrate TH/s, now and average; shares */
    char realtime[MW_STR], average[MW_STR], accepted[MW_STR], reject[MW_STR];
    /* reject %, fan RPM, fan duty %, pool ping */
    char rejpct[MW_STR], fan[MW_STR], fanr[MW_STR], ping[MW_STR];
    /* active pool, 1-based, its URL and worker */
    char current_pool[MW_STR], address[MW_URL], worker[MW_CREDENTIAL];
    /* MAC address, LVer version */
    char mac[MW_STR], version[MW_STR];
    /* chip DNA, 16 lowercase hex chars (8 bytes) from the core API */
    char dna[MW_STR];
    char pool_url[3][MW_URL], pool_worker[3][MW_CREDENTIAL];
    /* "1" in work, Activation, "1" on an ECHU/ECMM error */
    char sys_status[MW_STR], workingstatus[MW_STR], asic_status[MW_STR];
    /* "1" power error, "1" pool active */
    char power_status[MW_STR], pool_status[MW_STR];
    int  activation;
} mw_telemetry;

void mw_telemetry_init(mw_telemetry *t);
int  mw_parse_summary(mw_telemetry *t, const char *json);
int  mw_parse_pools(mw_telemetry *t, const char *json);
int  mw_parse_stats(mw_telemetry *t, const char *json);
int  mw_parse_version(mw_telemetry *t, const char *json);
int  mw_parse_batch(mw_telemetry *t, const char *json);
int  mw_qr_login_begin(const char *token);
int  mw_qr_login_take(const char *token);
int  mw_reboot_web(void);
int  mw_time_get(char *tz, size_t cap);
int  mw_time_set(const char *tz, long epoch);
int  mw_tz_from_link(const char *link, char *tz, size_t cap);
int  mw_telemetry_refresh(mw_telemetry *t);

static inline void mw_telemetry_link(mw_telemetry *t)
{
    mc_dashboard *d        = &t->dash;
    d->hwtype              = t->hwtype;
    d->elapsed             = t->elapsed;
    d->workingmode         = t->workingmode;
    d->power               = t->power;
    d->realtime_hash       = t->realtime;
    d->average_hash        = t->average;
    d->accepted            = t->accepted;
    d->reject              = t->reject;
    d->rejected_percentage = t->rejpct;
    d->fan_status          = t->fan;
    d->fanr                = t->fanr;
    d->ping                = t->ping;
    d->current_pool        = t->current_pool;
    d->address             = t->address;
    d->worker              = t->worker;
    d->mac                 = t->mac;
    d->version             = t->version;
    for (int i = 0; i < 3; i++) {
        d->pool[i].url    = t->pool_url[i];
        d->pool[i].worker = t->pool_worker[i];
    }
    d->sys_status    = t->sys_status;
    d->workingstatus = t->workingstatus;
    d->asic_status   = t->asic_status;
    d->power_status  = t->power_status;
    d->pool_status   = t->pool_status;
}

static inline void mw_telemetry_copy(mw_telemetry *dst, const mw_telemetry *src)
{
    memcpy(dst, src, sizeof *dst);
    mw_telemetry_link(dst);
}

#endif /* TELEMETRY_H */
