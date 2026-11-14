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

#ifndef CTRL_CMDS_H
#define CTRL_CMDS_H

#include <stdint.h>
#include <stddef.h>
#include "boardconf.h"
#include "ctrl_plugin.h"

/* ascset commands: X(op name, command string, mc_ctrl_cmd.v member, required capability) */
#define CTRL_CMD_ASCSET(X) CTRL_CMD_ASCSET_LO(X) CTRL_CMD_ASCSET_HI(X)
#define CTRL_CMD_ASCSET_LO(X) \
    X(WORKMODE, "workmode", workmode, 1) \
    X(LEDMODE, "ledmode", ledmode, CAP_LED_STRIP) \
    X(LEDSET, "ledset", ledset, CAP_LED_STRIP) \
    X(NIGHTLAMP, "nightlamp", nightlamp, CAP_LED_STRIP)
#define CTRL_CMD_ASCSET_HI(X) \
    X(FILTER_CLEAN, "filter-clean", text, 1) \
    X(LCD, "lcd", text, CAP_LCD)

#define CTRL_CMD_COMMAND(X) \
    X(SOFTON, "softon", soft, 1) \
    X(SOFTOFF, "softoff", soft, 1)

#define CTRL_CMD_INTERNAL(X) \
    X(SETTZ, "settz", text, 1) \
    X(WP_RELOAD, "wp-reload", text, CAP_LCD) \
    X(REBOOT, "reboot", reboot, 1)

#define CTRL_CMD_LIST(X) \
    CTRL_CMD_ASCSET(X) CTRL_CMD_COMMAND(X) CTRL_CMD_INTERNAL(X) CTRL_PLUGIN_CMDS(X)

#define CTRL_OP_ENUM(e, n, p, c) CTRL_OP_##e,

#define CTRL_TEXT_LEN 40 /* text argument size, bytes */

#define CTRL_REBOOT_DELAY_MAX 300 /* longest reboot delay accepted, s */

enum {
    CTRL_OP_NONE = 0,
    /* clang-format off */
    CTRL_CMD_ASCSET_LO(CTRL_OP_ENUM)
    CTRL_OP_RESERVED_5,
    CTRL_CMD_ASCSET_HI(CTRL_OP_ENUM)
    CTRL_CMD_COMMAND(CTRL_OP_ENUM) CTRL_CMD_INTERNAL(CTRL_OP_ENUM) CTRL_PLUGIN_CMDS(CTRL_OP_ENUM)
    CTRL_OP_COUNT /* number of op codes, CTRL_OP_NONE included */
    /* clang-format on */
};
#undef CTRL_OP_ENUM
_Static_assert(CTRL_OP_RESERVED_5 == 5 && CTRL_OP_FILTER_CLEAN == 6 && CTRL_OP_LCD == 7 &&
                CTRL_OP_SOFTON == 8 && CTRL_OP_SOFTOFF == 9 && CTRL_OP_SETTZ == 10 &&
                CTRL_OP_WP_RELOAD == 11 && CTRL_OP_REBOOT == 12,
        "CTRL_OP_* values are the core-mm wire numbering");

/* control command message, 48 bytes */
typedef struct mc_ctrl_cmd {
    uint16_t op;  /* CTRL_OP_* */
    uint16_t rsv; /* hashboard index of an op that names one, otherwise 0 */
    uint32_t seq; /* echoed in mc_ctrl_ack.seq; 0 when no answer is awaited */
    union {
        struct {
            uint8_t mode;  /* work mode number within BOARD_WORKMODE_RANGES */
            int32_t level; /* cali level index, below CALI_LEVEL_COUNT; the API sends only 0 */
        } workmode;
        struct {
            uint8_t mode; /* LED mode, 0 day, 1 dark */
        } ledmode;
        struct {
            uint8_t  effect;
            uint8_t  bright; /* brightness, percent */
            uint16_t temper; /* colour temperature setting, no unit; stored and reported only */
            uint8_t  r;      /* red, 0-255 */
            uint8_t  g;      /* green, 0-255 */
            uint8_t  b;      /* blue, 0-255 */
        } ledset;
        struct {
            uint8_t  mode;       /* 1 on, 0 off */
            uint32_t duration_s; /* on time, s; 0 selects the default */
        } nightlamp;
        struct {
            int32_t immediate; /* nonzero: now; zero: at ts */
            uint8_t valid;     /* 1 sets the schedule, 0 clears it */
            int64_t ts;        /* scheduled time, Unix epoch s */
        } soft;
        struct {
            uint8_t  reason; /* boot reason, MC_BOOTBY_*; 0 records none */
            uint32_t sub;
            uint16_t delay_s; /* delay before reboot, s; 0 selects the default */
        } reboot;
        char text[CTRL_TEXT_LEN]; /* argument string, NUL-terminated */
        CTRL_PLUGIN_PAYLOAD
    } v; /* argument, member selected by op */
} mc_ctrl_cmd;

_Static_assert(sizeof(mc_ctrl_cmd) == 48,
        "mc_ctrl_cmd is the wire struct; "
        "its size is the version axis and cannot change silently");

extern const uint16_t board_ctrl_ops[];
extern const size_t   board_ctrl_op_count;

const char *ctrl_cmd_name(uint16_t op);
int         ctrl_cmd_on_board(uint16_t op);
int         ctrl_cmd_available(uint16_t op);

#endif /* CTRL_CMDS_H */
