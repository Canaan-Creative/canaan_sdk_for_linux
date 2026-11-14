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

#include "api_plugin_cmds.h"
#include "core_ascset_internal.h"

#include <stdio.h>

mc_set_result plugin_stub_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)op;
    (void)s;
    if (!args || !*args) {
        snprintf(msg, msgcap, "plugin-stub ctx %s", ctx ? (const char *)ctx : "(null)");
        return MC_SET_INFO;
    }
    return core_ascset_forward(CTRL_OP_WP_RELOAD, args, msg, msgcap, "plugin-stub");
}

int plugin_stub_reads(const char *args)
{
    return !*args;
}
