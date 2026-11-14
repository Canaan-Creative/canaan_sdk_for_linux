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

#include "mm_ctrl_harness.h"

int main(void)
{
    mc_ctrl_cmd c;

    CHECK(CTRL_OP_PLUGIN_STUB == CTRL_OP_REBOOT + 1 && CTRL_OP_COUNT == CTRL_OP_PLUGIN_STUB + 1,
            "a plugin op is numbered after the built-in ops (%d)", CTRL_OP_PLUGIN_STUB);
    CHECK(!strcmp(ctrl_cmd_name(CTRL_OP_PLUGIN_STUB), "plugin-stub-op"),
            "a plugin op has its command string (%s)", ctrl_cmd_name(CTRL_OP_PLUGIN_STUB));
    CHECK(ctrl_cmd_available(CTRL_OP_PLUGIN_STUB),
            "a plugin op listed in CTRL_PLUGIN_BOARD_OPS is available on [%s]", BOARDCONF_SECTION);
    static const uint16_t own[] = { BOARD_CTRL_OPS };
    size_t                n_own = sizeof own / sizeof own[0];
    int                   kept  = board_ctrl_op_count == n_own + 1;
    for (size_t i = 0; kept && i < n_own; i++)
        kept = board_ctrl_ops[i] == own[i];
    CHECK(kept, "the board's own BOARD_CTRL_OPS stay listed next to the plugin ops");

    CHECK(ctrl_parse_cmd(CTRL_OP_PLUGIN_STUB, "7", &c) == 0 && c.op == CTRL_OP_PLUGIN_STUB &&
                    c.v.plugin_stub == 7,
            "ctrl_parse_cmd parses a plugin op with the plugin parser");
    CHECK(ctrl_parse_cmd(CTRL_OP_PLUGIN_STUB, "x", &c) != 0,
            "ctrl_parse_cmd refuses what the plugin parser refuses");
    CHECK(ctrl_parse_cmd(CTRL_OP_COUNT, "7", &c) != 0, "ctrl_parse_cmd refuses an unknown op");

    memset(&c, 0, sizeof c);
    c.op            = CTRL_OP_PLUGIN_STUB;
    c.seq           = 5;
    c.v.plugin_stub = 7;
    mm_ctrl_cmd(&c, (int)sizeof c);
    CHECK(plugin_stub_routes == 1 && plugin_stub_last == 7,
            "mm_miner routes a plugin op to its CTRL_ROUTE_<op> handler (%d call(s))",
            plugin_stub_routes);
    CHECK(tst_ack_n == 1 && tst_ack.seq == 5 && tst_ack.result == MC_SET_OK,
            "...and acknowledges it with the handler's result (%d ack(s), result %d)", tst_ack_n,
            (int)tst_ack.result);
    CHECK(!strcmp(tst_oplog, "control command plugin-stub-op (stub 7): done"),
            "...and logs it with the plugin's description (%s)", tst_oplog);

    c.v.plugin_stub = 0;
    mm_ctrl_cmd(&c, (int)sizeof c);
    CHECK(tst_ack_n == 2 && tst_ack.result == MC_SET_ERR_ARG &&
                    !strcmp(tst_oplog,
                            "control command plugin-stub-op (stub 0): refused: invalid argument"),
            "a plugin handler's refusal reaches the ack and the log (%s)", tst_oplog);

    memset(&c, 0, sizeof c);
    c.op = CTRL_OP_SETTZ;
    snprintf(c.v.text, sizeof c.v.text, "UTC");
    mm_ctrl_cmd(&c, (int)sizeof c);
    CHECK(!strcmp(tst_oplog,
                  ctrl_cmd_available(CTRL_OP_SETTZ) ?
                          "control command settz (UTC): done" :
                          "control command settz (UTC): refused: not supported"),
            "a built-in text op the plugin does not describe still logs its text (%s)", tst_oplog);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
