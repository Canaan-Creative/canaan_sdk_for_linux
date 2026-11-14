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
#include "mc_api_setter.h"
#include "board_tables.h"
#include <stdio.h>

size_t mc_api_text_status(char *buf, size_t cap, uint32_t when, char st, int code, const char *msg)
{
    char safe[MC_API_MSG_CAP];
    snprintf(safe, sizeof safe, "%s", msg);
    mc_api_msg_safe(safe, sizeof safe);
    return (size_t)snprintf(buf, cap,
            "STATUS=%c,When=%lu,Code=%d,Msg=%s,Description=AvalonMiner %s|", st,
            (unsigned long)when, code, safe, AM_VER);
}

void mc_api_text_field(const char *in, char *out, size_t cap)
{
    snprintf(out, cap, "%s", in ? in : "");
    mc_api_msg_safe(out, cap);
}

size_t mc_api_version_text(const mc_api_stats *s, char *buf, size_t cap)
{
    char safe_prod[128], safe_model[128];
    mc_api_text_field(s->prod, safe_prod, sizeof safe_prod);
    mc_api_text_field(s->model, safe_model, sizeof safe_model);
    return (size_t)snprintf(buf, cap,
            "STATUS=S,When=%lu,Code=22,Msg=AvalonMiner versions,Description=AvalonMiner %s|"
            "VERSION,AvalonMiner=%s,API=%s,PROD=%s,MODEL=%s,HWTYPE=%s,SWTYPE=%s,"
            "FWVERSION=%s,DNA=%s,MAC=%s,UPAPI=2|",
            (unsigned long)s->when, AM_VER, AM_VER, API_VER, safe_prod, safe_model, s->hwtype,
            s->swtype, s->version, s->dna, s->mac);
}

size_t mc_api_pools_text(const mc_api_stats *s, char *buf, size_t cap)
{
    /* No pools is an error (Code 8). */
    if (s->npools == 0)
        return mc_api_status_reply(0, buf, cap, s->when, 'E', 8, "No pools");
    size_t n = (size_t)snprintf(buf, cap,
            "STATUS=S,When=%lu,Code=7,Msg=%d Pool(s),Description=AvalonMiner %s|",
            (unsigned long)s->when, s->npools, AM_VER);
    for (int i = 0; i < s->npools && i < 8 && n < cap; i++) {
        const mc_api_pool *p                          = &s->pool[i];
        char               shost[MC_API_POOL_URL_CAP] = "";
        if (p->stratum_active)
            pool_host(p->url, shost, sizeof shost);
        char safe_url[MC_API_POOL_URL_CAP], safe_user[256], safe_shost[MC_API_POOL_URL_CAP];
        mc_api_text_field(p->url, safe_url, sizeof safe_url);
        mc_api_text_field(p->user, safe_user, sizeof safe_user);
        mc_api_text_field(shost, safe_shost, sizeof safe_shost);
        double rejden   = p->diff_accepted + p->diff_rejected + p->diff_stale;
        double rejpct   = rejden > 0.0 ? p->diff_rejected * 100.0 / rejden : 0.0;
        double stalepct = rejden > 0.0 ? p->diff_stale * 100.0 / rejden : 0.0;
        n += (size_t)snprintf(buf + n, cap - n,
                "POOL=%d,URL=%s,Status=%s,Priority=%d,Quota=1,Long Poll=N,Getworks=%u,"
                "Accepted=%u,Rejected=%u,Works=%u,Discarded=0,Stale=%u,Get Failures=%u,"
                "Remote Failures=%u,User=%s,"
                "Last Share Time=%u,Diff1 Shares=%.0f,"
                "Proxy Type=,Proxy=,Difficulty Accepted=%.8f,"
                "Difficulty Rejected=%.8f,Difficulty Stale=%.8f,"
                "Last Share Difficulty=%.8f,Work Difficulty=%.8f,"
                "Has Stratum=true,Stratum Active=%s,Stratum URL=%s,"
                "Stratum Difficulty=%.8f,Has Vmask=true,Has GBT=false,Best Share=%.0f,"
                "Pool Rejected%%=%.4f,Pool Stale%%=%.4f,Bad Work=%u,"
                "Current Block Height=%u,Current Block Version=%u|",
                p->id, safe_url, p->status ? p->status : "", p->prio, p->getworks, p->accepted,
                p->rejected, p->works, p->stale, p->get_failures, p->remote_failures, safe_user,
                p->last_share_time, p->diff1_shares, p->diff_accepted, p->diff_rejected,
                p->diff_stale, p->last_share_diff, p->work_diff,
                p->stratum_active ? "true" : "false", safe_shost,
                p->stratum_active ? p->work_diff : 0.0, p->best_share, rejpct, stalepct,
                p->bad_work, p->block_height, p->block_version);
    }
    return n;
}

size_t mc_api_summary_text(const mc_api_stats *s, char *buf, size_t cap)
{
    double hwp, devrejp;
    dev_pcts(s, &hwp, &devrejp);
    double poolrejden = s->diff_accepted + s->diff_rejected + s->diff_stale;
    double poolrejp   = poolrejden > 0.0 ? s->diff_rejected / poolrejden * 100.0 : 0.0;
    double poolstalep = poolrejden > 0.0 ? s->diff_stale / poolrejden * 100.0 : 0.0;
    return (size_t)snprintf(buf, cap,
            "STATUS=S,When=%lu,Code=11,Msg=Summary,Description=AvalonMiner %s|"
            "SUMMARY,Elapsed=%u,MHS av=%.2f,MHS 5s=%.2f,MHS 1m=%.2f,MHS 5m=%.2f,"
            "MHS 15m=%.2f,Found Blocks=0,Getworks=%u,Accepted=%u,Rejected=%u,"
            "Hardware Errors=%u,Utility=%.2f,Discarded=%u,Stale=%u,Get Failures=%u,"
            "Local Work=%u,Remote Failures=%u,Network Blocks=%u,Total MH=%.4f,"
            "Work Utility=%.2f,"
            "Difficulty Accepted=%.8f,Difficulty Rejected=%.8f,"
            "Difficulty Stale=%.8f,Best Share=%.0f,"
            "Device Hardware%%=%.4f,Device Rejected%%=%.4f,Pool Rejected%%=%.4f,"
            "Pool Stale%%=%.4f,Last getwork=0|",
            (unsigned long)s->when, AM_VER, s->elapsed, s->mhs_av, s->mhs_5s, s->mhs_1m, s->mhs_5m,
            s->mhs_15m, sum_getworks(s), s->accepted, s->rejected, s->hw_errors, utility(s),
            s->discarded, s->stale, s->get_failures, s->local_work, s->remote_failures,
            s->net_blocks, total_mh(s), mc_api_work_utility(s), s->diff_accepted, s->diff_rejected,
            s->diff_stale, s->best_share, hwp, devrejp, poolrejp, poolstalep);
}

size_t mc_api_devs_text(const mc_api_stats *s, char *buf, size_t cap)
{
    double hwp, devrejp;
    dev_pcts(s, &hwp, &devrejp);
    int lsp = latest_share_pool_idx(s);
    int ap  = active_pool_idx(s);
    return (size_t)snprintf(buf, cap,
            "STATUS=S,When=%lu,Code=9,Msg=1 ASC(s),Description=AvalonMiner %s|"
            "ASC=0,Name=AVALON,ID=0,Enabled=Y,Status=%s,Temperature=%.2f,"
            "MHS av=%.2f,MHS 5s=%.2f,MHS 1m=%.2f,MHS 5m=%.2f,MHS 15m=%.2f,"
            "Accepted=%u,Rejected=%u,Hardware Errors=%u,Utility=%.2f,"
            "Last Share Pool=%d,Last Share Time=%u,Total MH=%.4f,Diff1 Work=%.0f,"
            "Difficulty Accepted=%.8f,Difficulty Rejected=%.8f,"
            "Last Share Difficulty=%.8f,Last Valid Work=%u,"
            "Device Hardware%%=%.4f,Device Rejected%%=%.4f,Device Elapsed=%.0f|",
            (unsigned long)s->when, AM_VER, dev_status(s),
            (double)(s->telem.valid ? s->telem.temp_max : 0), dev_mhs_av(s), s->mhs_5s, s->mhs_1m,
            s->mhs_5m, s->mhs_15m, s->accepted, s->rejected, s->hw_errors, dev_utility(s),
            lsp >= 0 ? s->pool[lsp].id : (s->npools > 0 ? ap : -1),
            lsp >= 0 ? s->pool[lsp].last_share_time : 0u, total_mh(s), diff1_work(s),
            s->diff_accepted, s->diff_rejected, lsp >= 0 ? s->pool[lsp].last_share_diff : 0.0,
            s->last_valid_work, hwp, devrejp, s->dev_elapsed);
}

size_t mc_api_coin_text(const mc_api_stats *s, char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap,
            "STATUS=S,When=%lu,Code=78,Msg=AvalonMiner coin,Description=AvalonMiner %s|"
            "COIN,Hash Method=sha256,Current Block Time=%.6f,"
            "Current Block Hash=%s,LP=%s,Network Difficulty=%.8f|",
            (unsigned long)s->when, AM_VER, s->block_time, s->block_hash ? s->block_hash : "",
            coin_lp(s), s->net_difficulty);
}

size_t mc_api_config_text(const mc_api_stats *s, char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap,
            "STATUS=S,When=%lu,Code=33,Msg=AvalonMiner config,Description=AvalonMiner %s|"
            "CONFIG,ASC Count=%d,PGA Count=0,Pool Count=%d,Strategy=Failover,"
            "Log Interval=5,Device Code=AVALON,OS=Linux,Hotplug=None|",
            (unsigned long)s->when, AM_VER, s->asc_count, s->pool_count);
}

size_t mc_api_devdetails_text(const mc_api_stats *s, char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap,
            "STATUS=S,When=%lu,Code=69,Msg=Device Details,Description=AvalonMiner %s|"
            "DEVDETAILS=0,Name=AVALON,ID=0,Driver=avalon,Kernel=,Model=,Device Path=|",
            (unsigned long)s->when, AM_VER);
}
