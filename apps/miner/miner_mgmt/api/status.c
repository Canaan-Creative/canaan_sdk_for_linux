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

#include "status.h"
#include "jwrite.h"
#include <stdio.h>

static void pool_flat(jwriter *w, int n, const mc_pool_view *p)
{
    char key[16];
    snprintf(key, sizeof key, "pool%d", n);
    jw_kv_str(w, key, p->url);
    snprintf(key, sizeof key, "worker%d", n);
    jw_kv_str(w, key, p->worker);
    snprintf(key, sizeof key, "passwd%d", n);
    jw_kv_str(w, key, p->passwd);
}

static void dashboard_fields(jwriter *w, const mc_dashboard *d)
{
    jw_kv_str(w, "hwtype", d->hwtype);
    jw_kv_str(w, "sys_status", d->sys_status);
    jw_kv_str(w, "elapsed", d->elapsed);
    jw_kv_str(w, "workingmode", d->workingmode);
    jw_kv_str(w, "workingstatus", d->workingstatus);
    jw_kv_str(w, "power", d->power);
    jw_kv_str(w, "realtime_hash", d->realtime_hash);
    jw_kv_str(w, "average_hash", d->average_hash);
    jw_kv_str(w, "accepted", d->accepted);
    jw_kv_str(w, "reject", d->reject);
    jw_kv_str(w, "rejected_percentage", d->rejected_percentage);
    jw_kv_str(w, "fan_status", d->fan_status);
    jw_kv_str(w, "fanr", d->fanr);
    jw_kv_str(w, "asic_status", d->asic_status);
    jw_kv_str(w, "ping", d->ping);
    jw_kv_str(w, "power_status", d->power_status);
    jw_kv_str(w, "pool_status", d->pool_status);
    jw_kv_str(w, "current_pool", d->current_pool);
    jw_kv_str(w, "address", d->address);
    jw_kv_str(w, "worker", d->worker);
    jw_kv_str(w, "mac", d->mac);
    jw_kv_str(w, "version", d->version);
    for (int i = 0; i < (int)(sizeof d->pool / sizeof d->pool[0]); i++)
        pool_flat(w, i + 1, &d->pool[i]);
}

size_t mc_status_dashboard_json(const mc_dashboard *d, char *buf, size_t cap)
{
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    dashboard_fields(&w, d);
    jw_obj_close(&w);
    return jw_len(&w);
}
