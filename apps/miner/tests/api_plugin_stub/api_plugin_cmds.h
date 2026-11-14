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

#ifndef API_PLUGIN_CMDS_H
#define API_PLUGIN_CMDS_H

#include <stddef.h>
#include <stdint.h>

#include "ctrl_cmds.h"
#include "mc_api_setter.h"

mc_set_result plugin_stub_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx);
int           plugin_stub_reads(const char *args);

/*
 * stub ascset settings: one on every board listed before ledmode that only reads
 * without arguments, one only where WP_RELOAD is offered, one secret and hidden
 */
#define API_PLUGIN_SETTERS(X) \
    X("plugin-stub", plugin_stub_setter, CTRL_OP_NONE, 0, "ledmode", plugin_stub_reads) \
    X("plugin-stub-wp", plugin_stub_setter, CTRL_OP_WP_RELOAD, 0, NULL, NULL) \
    X("plugin-stub-hidden", plugin_stub_setter, CTRL_OP_NONE, \
            MC_API_SETTER_SECRET | MC_API_SETTER_HIDDEN, "loop", NULL)

#endif /* API_PLUGIN_CMDS_H */
