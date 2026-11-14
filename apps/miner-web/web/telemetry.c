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

#include "telemetry.h"
#include "core_api.h"
#include "jread.h"
#include "mc_errcode.h"
#include "web_logfmt.h"

#include <stdio.h>
#include <string.h>

#define MW_MAC_HEX_LEN 12                  /* MAC in the VERSION reply, hex chars */
#define MW_MAC_UNKNOWN "00:00:00:00:00:00" /* MAC shown when none was read */

/* poolstatus(): 0 pools comes back as the error "No pools" (Code 8),
 * not an empty POOLS array, and a configured-but-unconnected pool set leaves no
 * stratum active. Both are the "no active pool" state the UI shows in
 * the address/worker fields — not a parse failure. */
#define MW_NO_ACTIVE_POOL "no active pool"

static void set(char *dst, size_t cap, const char *s)
{
    snprintf(dst, cap, "%s", s);
}

static void setf(char *dst, size_t cap, const char *fmt, double v)
{
    snprintf(dst, cap, fmt, v);
}

void mw_telemetry_init(mw_telemetry *t)
{
    memset(t, 0, sizeof *t);
    mw_telemetry_link(t);
}

static int parse_summary_node(mw_telemetry *t, const json_value *root)
{
    const json_value *s = json_array_get(json_object_get(root, "SUMMARY"), 0);
    if (s) {
        setf(t->elapsed, MW_STR, "%.0f", json_number(json_object_get(s, "Elapsed")));
        setf(t->accepted, MW_STR, "%.0f", json_number(json_object_get(s, "Accepted")));
        setf(t->reject, MW_STR, "%.0f", json_number(json_object_get(s, "Rejected")));
        setf(t->rejpct, MW_STR, "%.2f", json_number(json_object_get(s, "Pool Rejected%")));
    }
    return 0;
}

int mw_parse_summary(mw_telemetry *t, const char *json)
{
    json_value *root = json_parse(json);
    if (!root)
        return -1;
    int rc = parse_summary_node(t, root);
    json_free(root);
    return rc;
}

static int parse_pools_node(mw_telemetry *t, const json_value *root)
{
    const json_value *pools = json_object_get(root, "POOLS");
    if (!json_is(pools, JSON_ARRAY)) {
        /* Accept the specific "No pools" (Code 8) reply as a valid empty-pool
         * state; any other reply without a POOLS array is a parse failure. */
        const json_value *st = json_array_get(json_object_get(root, "STATUS"), 0);
        if ((int)json_number(json_object_get(st, "Code")) != 8)
            return -1;
    }
    size_t n = json_array_size(pools);
    if (n > 3)
        return -1;
    for (size_t i = 0; i < n; i++) {
        const json_value *p = json_array_get(pools, i);
        if (strlen(json_string(json_object_get(p, "URL"))) >= MW_URL ||
                strlen(json_string(json_object_get(p, "User"))) >= MW_CREDENTIAL)
            return -1;
    }
    memset(t->pool_url, 0, sizeof t->pool_url);
    memset(t->pool_worker, 0, sizeof t->pool_worker);
    t->current_pool[0] = 0;
    set(t->pool_status, MW_STR, "0");
    set(t->address, MW_URL, MW_NO_ACTIVE_POOL);
    set(t->worker, MW_CREDENTIAL, MW_NO_ACTIVE_POOL);
    for (size_t i = 0; i < n; i++) {
        const json_value *p = json_array_get(pools, i);
        set(t->pool_url[i], MW_URL, json_string(json_object_get(p, "URL")));
        set(t->pool_worker[i], MW_CREDENTIAL, json_string(json_object_get(p, "User")));
        if (!t->current_pool[0] && json_bool(json_object_get(p, "Stratum Active"))) {
            snprintf(t->current_pool, MW_STR, "%zu", i + 1);
            set(t->address, MW_URL, json_string(json_object_get(p, "URL")));
            set(t->worker, MW_CREDENTIAL, json_string(json_object_get(p, "User")));
            set(t->pool_status, MW_STR, "1");
        }
    }
    return 0;
}

int mw_parse_pools(mw_telemetry *t, const char *json)
{
    json_value *root = json_parse(json);
    if (!root)
        return -1;
    int rc = parse_pools_node(t, root);
    json_free(root);
    return rc;
}

static const json_value *stat_field(const json_value *sum, const char *key, json_type type)
{
    const json_value *v = json_object_find(sum, key);
    return json_is(v, type) ? v : NULL;
}

static void set_int(char *dst, size_t cap, const json_value *v)
{
    snprintf(dst, cap, "%lld", json_int(v));
}

static int parse_stats_node(mw_telemetry *t, const json_value *root)
{
    const json_value *st  = json_array_get(json_object_get(root, "STATS"), 0);
    const json_value *sum = json_object_get(st, "MMID0Summary");
    const json_value *f;

    if (!json_is(sum, JSON_OBJECT))
        return 0;
    if ((f = stat_field(sum, "Ver", JSON_STRING)) != NULL) {
        char v[MW_STR];
        set(v, sizeof v, json_string(f));
        char *dash = strchr(v, '-');
        if (dash)
            *dash = '\0';
        snprintf(t->hwtype, MW_STR, "Avalon %.56s", v);
    }
    if ((f = stat_field(sum, "LVer", JSON_STRING)) != NULL)
        set(t->version, MW_STR, json_string(f));
    if ((f = stat_field(sum, "DNA", JSON_STRING)) != NULL)
        set(t->dna, MW_STR, json_string(f));
    if ((f = stat_field(sum, "Fan1", JSON_NUMBER)) != NULL)
        set_int(t->fan, MW_STR, f);
    if ((f = stat_field(sum, "FanR", JSON_NUMBER)) != NULL)
        set_int(t->fanr, MW_STR, f);
    if ((f = stat_field(sum, "PS", JSON_ARRAY)) != NULL && json_array_size(f))
        set_int(t->power, MW_STR, json_array_get(f, json_array_size(f) - 1));
    if ((f = stat_field(sum, "GHSspd", JSON_NUMBER)) != NULL)
        setf(t->realtime, MW_STR, "%.2f", json_number(f) / 1000.0);
    if ((f = stat_field(sum, "GHSavg", JSON_NUMBER)) != NULL)
        setf(t->average, MW_STR, "%.2f", json_number(f) / 1000.0);
    if ((f = stat_field(sum, "WORKMODE", JSON_NUMBER)) != NULL)
        set_int(t->workingmode, MW_STR, f);
    if ((f = stat_field(sum, "PING", JSON_NUMBER)) != NULL)
        set_int(t->ping, MW_STR, f);
    if ((f = stat_field(sum, "SYSTEMSTATU", JSON_STRING)) != NULL)
        set(t->sys_status, MW_STR, strstr(json_string(f), "In Work") ? "1" : "0");
    if ((f = stat_field(sum, "Activation", JSON_NUMBER)) != NULL) {
        set_int(t->workingstatus, MW_STR, f);
        t->activation = (int)json_int(f);
    }
    unsigned long echu =
            (unsigned long)json_int(json_array_get(stat_field(sum, "ECHU", JSON_ARRAY), 0));
    unsigned long ecmm = (unsigned long)json_int(stat_field(sum, "ECMM", JSON_NUMBER));
    set(t->asic_status, MW_STR, (echu || ecmm) ? "1" : "0");
    set(t->power_status, MW_STR, (ecmm & ERR_MM_POWER) ? "1" : "0");
    return 0;
}

int mw_parse_stats(mw_telemetry *t, const char *json)
{
    json_value *root = json_parse(json);
    if (!root)
        return -1;
    int rc = parse_stats_node(t, root);
    json_free(root);
    return rc;
}

static int parse_version_node(mw_telemetry *t, const json_value *root)
{
    const json_value *v = json_array_get(json_object_get(root, "VERSION"), 0);
    const char       *m = json_string(json_object_get(v, "MAC"));
    if (strlen(m) == MW_MAC_HEX_LEN)
        snprintf(t->mac, MW_STR, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", m, m + 2, m + 4, m + 6, m + 8,
                m + 10);
    return 0;
}

int mw_parse_version(mw_telemetry *t, const char *json)
{
    json_value *root = json_parse(json);
    if (!root)
        return -1;
    int rc = parse_version_node(t, root);
    json_free(root);
    return rc;
}

static void warn_not_understood(const char *cmd, const char *reply)
{
    web_logf(stderr, "WARN",
            "miner %s reply not understood, ignored "
            "(starts: %.120s)\n",
            cmd, reply);
}

int mw_parse_batch(mw_telemetry *t, const char *json)
{
    json_value *root = json_parse(json);
    /* The "summary" member is only present when the core understood the '+'
     * join; an older core answers with a plain error object, which this rejects
     * so the caller can fall back to per-command fetches. */
    if (!root || !json_object_get(root, "summary")) {
        json_free(root);
        return -1;
    }
    parse_summary_node(t, json_object_get(root, "summary"));
    parse_pools_node(t, json_object_get(root, "pools"));
    parse_stats_node(t, json_object_get(root, "stats"));
    parse_version_node(t, json_object_get(root, "version"));
    json_free(root);
    return 0;
}

int mw_telemetry_refresh(mw_telemetry *t)
{
    static char buf[MW_RPC_REPLY_CAP];
    int         any = -1;

    mw_telemetry_init(t);

    /* One '+'-batched request fetches the whole dashboard in a single round
     * trip, against one core snapshot. A core without batching is handled by the
     * per-command path below (mw_parse_batch rejects its error reply). */
    if (core_api_call("summary+pools+stats+version", NULL, buf, sizeof buf) >= 0 &&
            mw_parse_batch(t, buf) == 0) {
        if (!t->mac[0])
            set(t->mac, MW_STR, MW_MAC_UNKNOWN);
        return 0;
    }

    static const struct {
        const char *cmd;
        int (*parse)(mw_telemetry *, const char *);
    } docs[] = {
        { "summary", mw_parse_summary },
        { "pools", mw_parse_pools },
        { "stats", mw_parse_stats },
        { "version", mw_parse_version },
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        if (core_api_call(docs[i].cmd, NULL, buf, sizeof buf) < 0) {
            web_logf(stderr, "WARN",
                    "cannot read miner %s; the dashboard shows it as empty until the "
                    "miner answers\n",
                    docs[i].cmd);
            continue;
        }
        if (docs[i].parse(t, buf) != 0) {
            warn_not_understood(docs[i].cmd, buf);
            continue;
        }
        any = 0;
    }
    if (!t->mac[0])
        set(t->mac, MW_STR, MW_MAC_UNKNOWN);
    return any;
}

int mw_qr_login_begin(const char *token)
{
    char buf[512], param[40];
    snprintf(param, sizeof param, "begin,%s", token);
    return core_api_call("qrlogin", param, buf, sizeof buf) >= 0 &&
            strstr(buf, "\"Started\":1") != NULL;
}

int mw_qr_login_take(const char *token)
{
    char buf[512];
    char param[40];
    snprintf(param, sizeof param, "claim,%s", token);
    if (core_api_call("qrlogin", param, buf, sizeof buf) < 0)
        return 0;
    return strstr(buf, "\"Approved\":1") != NULL;
}

int mw_reboot_web(void)
{
    char buf[512];
    if (core_api_call("ascset", "0,reboot_web", buf, sizeof buf) < 0)
        return 0;
    return strstr(buf, "\"STATUS\":\"S\"") != NULL;
}

int mw_time_get(char *tz, size_t cap)
{
    char buf[512];
    if (cap)
        tz[0] = '\0';
    if (core_api_call("time", "get", buf, sizeof buf) < 0)
        return -1;
    /* reply Msg echoes "... time t:<zone>"; read the zone up to the JSON quote */
    const char *p = strstr(buf, "time t:");
    if (!p)
        return -1;
    p += 7;
    size_t n = 0;
    while (p[n] && p[n] != '"' && p[n] != '\\' && n + 1 < cap) {
        tz[n] = p[n];
        n++;
    }
    if (cap)
        tz[n] = '\0';
    return 0;
}

int mw_time_set(const char *tz, long epoch)
{
    char buf[512], param[128];
    snprintf(param, sizeof param, "set,t:%ld,%s", epoch, tz);
    if (core_api_call("time", param, buf, sizeof buf) < 0)
        return 0;
    /* set success is reported as an INFO status; errors come back as "E" */
    return strstr(buf, "\"STATUS\":\"I\"") != NULL && strstr(buf, "unknown") == NULL;
}

int mw_tz_from_link(const char *link, char *tz, size_t cap)
{
    if (!cap)
        return -1;
    tz[0]         = '\0';
    const char *z = link ? strstr(link, "zoneinfo/") : NULL;
    if (!z || !z[9])
        return -1;
    z += 9;
    if (strlen(z) >= cap)
        return -1;
    strcpy(tz, z);
    return 0;
}
