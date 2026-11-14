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

#include "mc_api_cmds.h"
#include "mc_api_cmds_internal.h"
#include "board_tables.h"
#include "jwrite.h"
#include <stdio.h>
#include <string.h>

void mc_api_status_elem(char *out, size_t cap, uint32_t when, int code, const char *msg)
{
    snprintf(out, cap,
            "[{\"STATUS\":\"S\",\"When\":%lu,\"Code\":%d,\"Msg\":\"%s\","
            "\"Description\":\"AvalonMiner %s\"}]",
            (unsigned long)when, code, msg, AM_VER);
}

size_t mc_api_version(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256], sec[512 + MINER_VERSION_CAP];
    char safe_prod[128], safe_model[128];
    mc_api_text_field(s->prod, safe_prod, sizeof safe_prod);
    mc_api_text_field(s->model, safe_model, sizeof safe_model);
    mc_api_status_elem(status, sizeof status, s->when, 22, "AvalonMiner versions");
    snprintf(sec, sizeof sec,
            "[{\"AvalonMiner\":\"%s\",\"API\":\"%s\",\"PROD\":\"%s\",\"MODEL\":\"%s\","
            "\"HWTYPE\":\"%s\",\"SWTYPE\":\"%s\",\"FWVERSION\":\"%s\","
            "\"DNA\":\"%s\",\"MAC\":\"%s\","
            "\"UPAPI\":\"2\"}]",
            AM_VER, API_VER, safe_prod, safe_model, s->hwtype, s->swtype, s->version, s->dna,
            s->mac);

    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "VERSION", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

double mc_api_work_utility(const mc_api_stats *s)
{
    return s->elapsed ? diff1_work(s) / (double)s->elapsed * 60.0 : 0.0;
}

double utility(const mc_api_stats *s)
{
    return s->elapsed ? s->accepted / (double)s->elapsed * 60.0 : 0.0;
}

double dev_utility(const mc_api_stats *s)
{
    return s->dev_elapsed > 0.0 ? s->accepted / s->dev_elapsed * 60.0 : 0.0;
}

double total_mh(const mc_api_stats *s)
{
    return s->mhs_av * (double)s->elapsed;
}

double dev_mhs_av(const mc_api_stats *s)
{
    return s->dev_elapsed > 0.0 ? total_mh(s) / s->dev_elapsed : 0.0;
}

double diff1_work(const mc_api_stats *s)
{
    return s->diff1;
}

void dev_pcts(const mc_api_stats *s, double *hwp, double *rejp)
{
    double d1 = diff1_work(s);
    *hwp  = (s->hw_errors + d1) > 0.0 ? (double)s->hw_errors / ((double)s->hw_errors + d1) * 100.0 :
                                        0.0;
    *rejp = d1 > 0.0 ? s->diff_rejected / d1 * 100.0 : 0.0;
}

int active_pool_idx(const mc_api_stats *s)
{
    for (int i = 0; i < s->npools && i < 8; i++)
        if (s->pool[i].stratum_active)
            return i;
    int best = 0, best_off = 1;
    for (int i = 0; i < s->npools && i < 8; i++) {
        int off = s->pool[i].status && !strcmp(s->pool[i].status, "Disabled");
        if (i == 0 || off < best_off || (off == best_off && s->pool[i].prio < s->pool[best].prio)) {
            best     = i;
            best_off = off;
        }
    }
    return best;
}

int latest_share_pool_idx(const mc_api_stats *s)
{
    int      best   = -1;
    uint32_t latest = 0;
    for (int i = 0; i < s->npools && i < 8; i++)
        if (s->pool[i].last_share_time > latest) {
            latest = s->pool[i].last_share_time;
            best   = i;
        }
    return best;
}

const char *dev_status(const mc_api_stats *s)
{
    return s->telem.valid ? "Alive" : "Initialising";
}

uint32_t sum_getworks(const mc_api_stats *s)
{
    uint32_t n = 0;
    for (int i = 0; i < s->npools && i < 8; i++)
        n += s->pool[i].getworks;
    return n;
}

size_t mc_api_summary(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256], sec[1024];
    mc_api_status_elem(status, sizeof status, s->when, 11, "Summary");
    double hwp, devrejp;
    dev_pcts(s, &hwp, &devrejp);
    double poolrejden = s->diff_accepted + s->diff_rejected + s->diff_stale;
    double poolrejp   = poolrejden > 0.0 ? s->diff_rejected / poolrejden * 100.0 : 0.0;
    double poolstalep = poolrejden > 0.0 ? s->diff_stale / poolrejden * 100.0 : 0.0;
    snprintf(sec, sizeof sec,
            "[{\"Elapsed\":%u,\"MHS av\":%.2f,\"MHS 5s\":%.2f,\"MHS 1m\":%.2f,"
            "\"MHS 5m\":%.2f,\"MHS 15m\":%.2f,\"Found Blocks\":0,\"Getworks\":%u,"
            "\"Accepted\":%u,\"Rejected\":%u,\"Hardware Errors\":%u,\"Utility\":%.2f,"
            "\"Discarded\":%u,\"Stale\":%u,\"Get Failures\":%u,\"Local Work\":%u,"
            "\"Remote Failures\":%u,\"Network Blocks\":%u,\"Total MH\":%.4f,"
            "\"Work Utility\":%.2f,"
            "\"Difficulty Accepted\":%.8f,\"Difficulty Rejected\":%.8f,"
            "\"Difficulty Stale\":%.8f,"
            "\"Best Share\":%.0f,\"Device Hardware%%\":%.4f,\"Device Rejected%%\":%.4f,"
            "\"Pool Rejected%%\":%.4f,\"Pool Stale%%\":%.4f,\"Last getwork\":0}]",
            s->elapsed, s->mhs_av, s->mhs_5s, s->mhs_1m, s->mhs_5m, s->mhs_15m, sum_getworks(s),
            s->accepted, s->rejected, s->hw_errors, utility(s), s->discarded, s->stale,
            s->get_failures, s->local_work, s->remote_failures, s->net_blocks, total_mh(s),
            mc_api_work_utility(s), s->diff_accepted, s->diff_rejected, s->diff_stale,
            s->best_share, hwp, devrejp, poolrejp, poolstalep);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "SUMMARY", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

size_t mc_api_config(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256], sec[384];
    mc_api_status_elem(status, sizeof status, s->when, 33, "AvalonMiner config");
    snprintf(sec, sizeof sec,
            "[{\"ASC Count\":%d,\"PGA Count\":0,\"Pool Count\":%d,\"Strategy\":\"Failover\","
            "\"Log Interval\":5,\"Device Code\":\"AVALON\",\"OS\":\"Linux\",\"Hotplug\":\"None\"}]",
            s->asc_count, s->pool_count);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "CONFIG", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

const char *coin_lp(const mc_api_stats *s)
{
    for (int i = 0; i < s->npools && i < 8; i++)
        if (s->pool[i].stratum_active)
            return "true";
    return "false";
}

size_t mc_api_coin(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256], sec[384];
    mc_api_status_elem(status, sizeof status, s->when, 78, "AvalonMiner coin");
    snprintf(sec, sizeof sec,
            "[{\"Hash Method\":\"sha256\",\"Current Block Time\":%.6f,"
            "\"Current Block Hash\":\"%s\",\"LP\":%s,\"Network Difficulty\":%.8f}]",
            s->block_time, s->block_hash ? s->block_hash : "", coin_lp(s), s->net_difficulty);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "COIN", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

size_t mc_api_devdetails(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256];
    mc_api_status_elem(status, sizeof status, s->when, 69, "Device Details");
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "DEVDETAILS",
            "[{\"DEVDETAILS\":0,\"Name\":\"AVALON\",\"ID\":0,"
            "\"Driver\":\"avalon\",\"Kernel\":\"\",\"Model\":\"\","
            "\"Device Path\":\"\"}]");
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

size_t mc_api_devs(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256], sec[768];
    mc_api_status_elem(status, sizeof status, s->when, 9, "1 ASC(s)");
    /*
     * Clients read these fields by position: keep the order. Last Share
     * Difficulty is that of the pool with the latest share; Last Valid Work
     * is the time of the latest diff-1 nonce, s.
     */
    double hwp, devrejp;
    dev_pcts(s, &hwp, &devrejp);
    int lsp = latest_share_pool_idx(s);
    int ap  = active_pool_idx(s);
    snprintf(sec, sizeof sec,
            "[{\"ASC\":0,\"Name\":\"AVALON\",\"ID\":0,\"Enabled\":\"Y\",\"Status\":\"%s\","
            "\"Temperature\":%.2f,\"MHS av\":%.2f,\"MHS 5s\":%.2f,\"MHS 1m\":%.2f,"
            "\"MHS 5m\":%.2f,\"MHS 15m\":%.2f,\"Accepted\":%u,\"Rejected\":%u,"
            "\"Hardware Errors\":%u,\"Utility\":%.2f,\"Last Share Pool\":%d,"
            "\"Last Share Time\":%u,\"Total MH\":%.4f,\"Diff1 Work\":%.0f,"
            "\"Difficulty Accepted\":%.8f,\"Difficulty Rejected\":%.8f,"
            "\"Last Share Difficulty\":%.8f,\"Last Valid Work\":%u,"
            "\"Device Hardware%%\":%.4f,\"Device Rejected%%\":%.4f,\"Device Elapsed\":%.0f}]",
            dev_status(s), (double)(s->telem.valid ? s->telem.temp_max : 0), dev_mhs_av(s),
            s->mhs_5s, s->mhs_1m, s->mhs_5m, s->mhs_15m, s->accepted, s->rejected, s->hw_errors,
            dev_utility(s), lsp >= 0 ? s->pool[lsp].id : (s->npools > 0 ? ap : -1),
            lsp >= 0 ? s->pool[lsp].last_share_time : 0u, total_mh(s), diff1_work(s),
            s->diff_accepted, s->diff_rejected, lsp >= 0 ? s->pool[lsp].last_share_diff : 0.0,
            s->last_valid_work, hwp, devrejp, s->dev_elapsed);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "DEVS", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

void pool_host(const char *url, char *out, size_t cap)
{
    const char *h = url ? url : "";
    const char *p = strstr(h, "://");
    if (p)
        h = p + 3;
    size_t n = 0;
    while (h[n] && h[n] != ':' && h[n] != '/' && n + 1 < cap) {
        out[n] = h[n];
        n++;
    }
    out[n] = '\0';
}

size_t mc_api_pools(const mc_api_stats *s, char *buf, size_t cap)
{
    /* No pools is an error (Code 8), not "0 Pool(s)". */
    if (s->npools == 0)
        return mc_api_status_reply(1, buf, cap, s->when, 'E', 8, "No pools");
    char status[256], sec[8192], elem[4096], msg[32];
    snprintf(msg, sizeof msg, "%d Pool(s)", s->npools);
    mc_api_status_elem(status, sizeof status, s->when, 7, msg);
    size_t off = (size_t)snprintf(sec, sizeof sec, "[");
    for (int i = 0; i < s->npools && i < 8; i++) {
        const mc_api_pool *p                          = &s->pool[i];
        char               shost[MC_API_POOL_URL_CAP] = "";
        if (p->stratum_active)
            pool_host(p->url, shost, sizeof shost);
        double rejden   = p->diff_accepted + p->diff_rejected + p->diff_stale;
        double rejpct   = rejden > 0.0 ? p->diff_rejected * 100.0 / rejden : 0.0;
        double stalepct = rejden > 0.0 ? p->diff_stale * 100.0 / rejden : 0.0;
        char   esc_url[2 * MC_API_POOL_URL_CAP], esc_user[256], esc_shost[2 * MC_API_POOL_URL_CAP];
        json_escape(p->url ? p->url : "", esc_url, sizeof esc_url);
        json_escape(p->user ? p->user : "", esc_user, sizeof esc_user);
        json_escape(shost, esc_shost, sizeof esc_shost);
        /*
         * Fields the core does not track are emitted as 0 or blank. Stratum
         * Difficulty is the live sdiff, so only the active pool carries it.
         */
        snprintf(elem, sizeof elem,
                "%s{\"POOL\":%d,\"URL\":\"%s\",\"Status\":\"%s\",\"Priority\":%d,"
                "\"Quota\":1,\"Long Poll\":\"N\",\"Getworks\":%u,"
                "\"Accepted\":%u,\"Rejected\":%u,\"Works\":%u,\"Discarded\":0,\"Stale\":%u,"
                "\"Get Failures\":%u,\"Remote Failures\":%u,\"User\":\"%s\","
                "\"Last Share Time\":%u,\"Diff1 Shares\":%.0f,\"Proxy Type\":\"\",\"Proxy\":\"\","
                "\"Difficulty Accepted\":%.8f,\"Difficulty Rejected\":%.8f,"
                "\"Difficulty Stale\":%.8f,\"Last Share Difficulty\":%.8f,"
                "\"Work Difficulty\":%.8f,\"Has Stratum\":true,\"Stratum Active\":%s,"
                "\"Stratum URL\":\"%s\",\"Stratum Difficulty\":%.8f,"
                "\"Has Vmask\":true,\"Has GBT\":false,\"Best Share\":%.0f,"
                "\"Pool Rejected%%\":%.4f,\"Pool Stale%%\":%.4f,\"Bad Work\":%u,"
                "\"Current Block Height\":%u,\"Current Block Version\":%u}",
                i ? "," : "", p->id, esc_url, p->status ? p->status : "", p->prio, p->getworks,
                p->accepted, p->rejected, p->works, p->stale, p->get_failures, p->remote_failures,
                esc_user, p->last_share_time, p->diff1_shares, p->diff_accepted, p->diff_rejected,
                p->diff_stale, p->last_share_diff, p->work_diff,
                p->stratum_active ? "true" : "false", esc_shost,
                p->stratum_active ? p->work_diff : 0.0, p->best_share, rejpct, stalepct,
                p->bad_work, p->block_height, p->block_version);
        int w = snprintf(sec + off, sizeof sec - off, "%s", elem);
        if (w < 0 || off + (size_t)w >= sizeof sec)
            break;
        off += (size_t)w;
    }
    snprintf(sec + off, sizeof sec - off, "]");
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "POOLS", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

void json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        char          e[7];
        size_t        el;
        if (c == '"' || c == '\\') {
            e[0] = '\\';
            e[1] = (char)c;
            el   = 2;
        } else if (c < 0x20) {
            el = (size_t)snprintf(e, sizeof e, "\\u%04x", c);
        } else {
            e[0] = (char)c;
            el   = 1;
        }
        if (n + el + 1 > cap)
            break;
        memcpy(out + n, e, el);
        n += el;
    }
    out[n] = '\0';
}
