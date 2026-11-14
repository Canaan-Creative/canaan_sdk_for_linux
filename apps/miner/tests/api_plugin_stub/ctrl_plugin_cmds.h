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

#ifndef CTRL_PLUGIN_CMDS_H
#define CTRL_PLUGIN_CMDS_H

#include <stddef.h>
#include <stdint.h>

#include "mc_set_result.h"

#define CTRL_PLUGIN_CMDS(X)               X(PLUGIN_STUB, "plugin-stub-op", plugin_stub, 1)
#define CTRL_PLUGIN_PAYLOAD               uint8_t plugin_stub;
#define CTRL_PLUGIN_BOARD_OPS             CTRL_OP_PLUGIN_STUB
#define CTRL_PLUGIN_PARSE(op, args, out)  plugin_stub_parse((op), (args), (out))
#define CTRL_PLUGIN_DESCRIBE(c, buf, cap) plugin_stub_describe((c), (buf), (cap))
#define CTRL_ROUTE_PLUGIN_STUB            plugin_stub_route

struct mc_ctrl_cmd;

extern int     plugin_stub_routes;
extern uint8_t plugin_stub_last;

int           plugin_stub_parse(uint16_t op, const char *args, struct mc_ctrl_cmd *out);
mc_set_result plugin_stub_route(uint16_t op, const struct mc_ctrl_cmd *c);
int           plugin_stub_describe(const struct mc_ctrl_cmd *c, char *buf, size_t cap);

#endif /* CTRL_PLUGIN_CMDS_H */
