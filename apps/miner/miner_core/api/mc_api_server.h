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

#ifndef MC_API_SERVER_H
#define MC_API_SERVER_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "mc_api_types.h"

#define MC_API_CMD_CAP 256
#define MC_API_CHAN_N  2

#define MC_API_TCP_RECV_MS    300  /* port 4028: receive a whole request, ms */
#define MC_API_TCP_SEND_MS    500  /* port 4028: send a whole reply, ms */
#define MC_API_IO_DEADLINE_MS 2000 /* trusted channel: receive a request, or send a reply, ms */

typedef void (*mc_api_snapshot_fn)(mc_api_stats *out, void *ctx);

size_t mc_api_parse_cmd(const char *req, char *cmd, size_t cap);
size_t mc_api_dispatch(const char *req, const char *cmd, const mc_api_stats *s, char *buf,
        size_t cap);
size_t mc_api_dispatch_join(const char *cmd, int is_json, const mc_api_stats *s, char *buf,
        size_t cap);
int    mc_api_server_start(mc_api_snapshot_fn snap, void *ctx);

/* per channel: CLOCK_MONOTONIC ms dispatch of the current request started at, 0 while idle */
extern _Atomic uint64_t g_mc_api_busy_ms[MC_API_CHAN_N];

#endif /* MC_API_SERVER_H */
