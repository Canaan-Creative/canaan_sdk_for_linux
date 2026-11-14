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

#define ROW(e, n, p, c) { CTRL_OP_##e, n, (c) },

typedef struct {
    uint16_t    op; /* CTRL_OP_* */
    const char *name;
    int         cap; /* required board capability, nonzero if present */
} ctrl_row_t;

static const ctrl_row_t g_ascset[]   = { CTRL_CMD_ASCSET(ROW) },
                        g_command[]  = { CTRL_CMD_COMMAND(ROW) },
                        g_internal[] = { CTRL_CMD_INTERNAL(ROW) CTRL_PLUGIN_CMDS(ROW) };

static const struct {
    const ctrl_row_t *rows;
    size_t            n;
} g_grp[] = {
    { g_ascset, sizeof g_ascset / sizeof g_ascset[0] },
    { g_command, sizeof g_command / sizeof g_command[0] },
    { g_internal, sizeof g_internal / sizeof g_internal[0] },
};
static const size_t g_ngrp = sizeof g_grp / sizeof g_grp[0];

static const ctrl_row_t *ctrl_row_find(uint16_t op)
{
    for (size_t g = 0; g < g_ngrp; g++)
        for (size_t i = 0; i < g_grp[g].n; i++)
            if (g_grp[g].rows[i].op == op)
                return &g_grp[g].rows[i];
    return NULL;
}

const char *ctrl_cmd_name(uint16_t op)
{
    const ctrl_row_t *r = ctrl_row_find(op);
    return r ? r->name : "?";
}

int ctrl_cmd_on_board(uint16_t op)
{
    for (size_t i = 0; i < board_ctrl_op_count; i++)
        if (board_ctrl_ops[i] == op)
            return 1;
    return 0;
}

int ctrl_cmd_available(uint16_t op)
{
    const ctrl_row_t *r;

    if (!ctrl_cmd_on_board(op))
        return 0;
    r = ctrl_row_find(op);
    return r && r->cap != 0;
}
