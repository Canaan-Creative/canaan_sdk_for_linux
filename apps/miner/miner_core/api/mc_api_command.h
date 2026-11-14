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

#ifndef MC_API_COMMAND_H
#define MC_API_COMMAND_H

#include <stddef.h>
#include "mc_api_types.h"

#define MC_API_CMD_CANON_CAP 32 /* canonical command name buffer, bytes */

typedef size_t (*mc_api_cmd_fn)(const char *req, const mc_api_stats *s, int is_json, char *buf,
        size_t cap, void *ctx);

const char   *mc_api_cmd_canon(const char *cmd, char *out, size_t cap);
int           mc_api_command_register(const char *cmd, mc_api_cmd_fn fn, void *ctx);
mc_api_cmd_fn mc_api_command_lookup(const char *cmd, void **ctx_out);
size_t mc_api_command_run(mc_api_cmd_fn fn, const char *cmd, const char *req, const mc_api_stats *s,
        int is_json, char *buf, size_t cap, void *ctx);
void   mc_api_command_reset(void);

#endif /* MC_API_COMMAND_H */
