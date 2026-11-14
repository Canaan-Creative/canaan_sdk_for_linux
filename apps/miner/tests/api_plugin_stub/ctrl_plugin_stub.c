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

#include "ctrl_cmds.h"

#include <stdio.h>

int     plugin_stub_routes;
uint8_t plugin_stub_last;

int plugin_stub_parse(uint16_t op, const char *args, mc_ctrl_cmd *out)
{
    if (op != CTRL_OP_PLUGIN_STUB || !args || args[0] < '0' || args[0] > '9' || args[1])
        return -1;
    out->v.plugin_stub = (uint8_t)(args[0] - '0');
    return 0;
}

mc_set_result plugin_stub_route(uint16_t op, const mc_ctrl_cmd *c)
{
    (void)op;
    plugin_stub_routes++;
    plugin_stub_last = c->v.plugin_stub;
    return c->v.plugin_stub ? MC_SET_OK : MC_SET_ERR_ARG;
}

int plugin_stub_describe(const mc_ctrl_cmd *c, char *buf, size_t cap)
{
    if (c->op != CTRL_OP_PLUGIN_STUB)
        return 0;
    snprintf(buf, cap, "stub %u", c->v.plugin_stub);
    return 1;
}
