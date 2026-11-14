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

#include "mc_api_lcd.h"
#include "mc_api_cmds.h"
#include "jwrite.h"
#include <stdio.h>

size_t mc_api_lcd(const mc_api_stats *s, char *buf, size_t cap)
{
    char status[256], sec[1536];
    mc_api_status_elem(status, sizeof status, s->when, 125, "LCD");
    int         ap        = active_pool_idx(s);
    const char *pool_url  = (s->npools > 0 && s->pool[ap].url) ? s->pool[ap].url : "none";
    const char *pool_user = (s->npools > 0 && s->pool[ap].user) ? s->pool[ap].user : "";
    char        esc_url[2 * MC_API_POOL_URL_CAP], esc_user[256];
    json_escape(pool_url, esc_url, sizeof esc_url);
    json_escape(pool_user, esc_user, sizeof esc_user);
    int      lsp = latest_share_pool_idx(s);
    double   lsd = lsp >= 0 ? s->pool[lsp].last_share_diff : 0.0;
    uint32_t lst = lsp >= 0 ? s->pool[lsp].last_share_time : 0;
    snprintf(sec, sizeof sec,
            "[{\"Elapsed\":%u,\"GHS av\":%.2f,\"GHS 5m\":%.2f,\"GHS 5s\":%.2f,"
            "\"Temperature\":%.2f,\"Last Share Difficulty\":%.8f,"
            "\"Last Share Time\":%u,\"Best Share\":%.0f,"
            "\"Last Valid Work\":%u,\"Found Blocks\":0,"
            "\"Current Pool\":\"%s\",\"User\":\"%s\"}]",
            s->elapsed, s->mhs_av / 1000.0, s->mhs_5m / 1000.0, s->mhs_5s / 1000.0,
            (double)(s->telem.valid ? s->telem.temp_max : 0), lsd, lst, s->best_share,
            s->last_valid_work, esc_url, esc_user);
    jwriter w;
    jw_init(&w, buf, cap);
    jw_obj_open(&w);
    jw_kv_raw(&w, "STATUS", status);
    jw_kv_raw(&w, "LCD", sec);
    jw_kv_raw(&w, "id", "1");
    jw_obj_close(&w);
    return jw_len(&w);
}

size_t mc_api_lcd_text(const mc_api_stats *s, char *buf, size_t cap)
{
    int         ap        = active_pool_idx(s);
    const char *pool_url  = (s->npools > 0 && s->pool[ap].url) ? s->pool[ap].url : "none";
    const char *pool_user = (s->npools > 0 && s->pool[ap].user) ? s->pool[ap].user : "";
    char        safe_url[MC_API_POOL_URL_CAP], safe_user[256];
    mc_api_text_field(pool_url, safe_url, sizeof safe_url);
    mc_api_text_field(pool_user, safe_user, sizeof safe_user);
    int      lsp = latest_share_pool_idx(s);
    double   lsd = lsp >= 0 ? s->pool[lsp].last_share_diff : 0.0;
    uint32_t lst = lsp >= 0 ? s->pool[lsp].last_share_time : 0;
    return (size_t)snprintf(buf, cap,
            "STATUS=S,When=%u,Code=125,Msg=LCD,Description=AvalonMiner %s|"
            "LCD,Elapsed=%u,GHS av=%.2f,GHS 5m=%.2f,GHS 5s=%.2f,"
            "Temperature=%.2f,Last Share Difficulty=%.8f,"
            "Last Share Time=%u,Best Share=%.0f,"
            "Last Valid Work=%u,Found Blocks=0,"
            "Current Pool=%s,User=%s|",
            s->when, AM_VER, s->elapsed, s->mhs_av / 1000.0, s->mhs_5m / 1000.0, s->mhs_5s / 1000.0,
            (double)(s->telem.valid ? s->telem.temp_max : 0), lsd, lst, s->best_share,
            s->last_valid_work, safe_url, safe_user);
}
