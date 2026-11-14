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

#ifndef MC_API_CMDS_H
#define MC_API_CMDS_H

#include <stddef.h>

#include "mc_api_types.h"

#define AM_VER                CORE_MINER_SEMVER
#define MC_API_MSG_CAP        (2u * MC_API_POOL_URL_CAP + 64u) /* Msg buffer, bytes */
#define MC_API_LEGACY_MSG_CAP 1280                             /* legacy Msg buffer, bytes */

size_t mc_api_version(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_summary(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_devs(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_pools(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_coin(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_config(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_devdetails(const mc_api_stats *s, char *buf, size_t cap);
void   mc_api_status_elem(char *out, size_t cap, uint32_t when, int code, const char *msg);
double mc_api_work_utility(const mc_api_stats *s);
double diff1_work(const mc_api_stats *s);
int    active_pool_idx(const mc_api_stats *s);
int    latest_share_pool_idx(const mc_api_stats *s);
void   json_escape(const char *in, char *out, size_t cap);
void   mc_api_text_field(const char *in, char *out, size_t cap);
int    mc_api_cmd_writes(const char *req, const char *cmd);
size_t mc_api_cmd_check(const char *req, const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_cmd_check_text(const char *req, const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_version_text(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_summary_text(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_devs_text(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_pools_text(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_coin_text(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_config_text(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_devdetails_text(const mc_api_stats *s, char *buf, size_t cap);
size_t mc_api_dispatch_text(const char *req, const char *cmd, const mc_api_stats *s, char *buf,
        size_t cap);
size_t mc_api_text_status(char *buf, size_t cap, uint32_t when, char st, int code, const char *msg);
size_t mc_api_status_reply(int is_json, char *buf, size_t cap, uint32_t when, char st, int code,
        const char *msg);
size_t mc_api_status_reply_legacy(int is_json, char *buf, size_t cap, uint32_t when, char st,
        int code, const char *msg);
size_t mc_api_lockstats(int is_json, const mc_api_stats *s, char *buf, size_t cap);

#endif /* MC_API_CMDS_H */
