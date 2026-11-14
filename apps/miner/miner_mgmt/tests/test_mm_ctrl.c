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

#include "mc_api_setter.h"
#include "../../app/ctrl_args.c"
#include "ctrl_cmds.h"
#include "cali.h"
#include <stdio.h>
#include <string.h>

#include "../../tests/include/tst_check.h"

#define T_OK 1755000000LL

#define OP_BY_NAME(e, nm, payload, cap) \
    if (!strcmp(name, nm)) \
        return CTRL_OP_##e;

#define WM_SPAN(lo, hi) (v >= (lo) && v <= (hi)) ||

#define AVAIL_ROW(e, nm, payload, cap) \
    do { \
        int fitted = (cap) != 0 && ctrl_cmd_on_board(CTRL_OP_##e); \
        CHECK(ctrl_cmd_available(CTRL_OP_##e) == fitted, \
                "%s (cap %d, on-board %d) is %s on this descriptor", nm, (cap) != 0, \
                ctrl_cmd_on_board(CTRL_OP_##e), fitted ? "AVAILABLE" : "NOT available"); \
    } while (0);

#define SELFCONSIST_ROW(e, nm, payload, cap) \
    CHECK(!ctrl_cmd_on_board(CTRL_OP_##e) || (cap) != 0, \
            "%s is %s by this descriptor's BOARD_CTRL_OPS and its cap here is %d: a " \
            "command the board's own capability bits refuse must not be listed at " \
            "all, because a descriptor that contradicts itself is not a command " \
            "gated at runtime", \
            nm, ctrl_cmd_on_board(CTRL_OP_##e) ? "listed" : "not listed", (cap) != 0);

static uint16_t tst_op_by_name(const char *name)
{
    CTRL_CMD_LIST(OP_BY_NAME)
    return CTRL_OP_NONE;
}

static int wm_allowed(int v)
{
    return BOARD_WORKMODE_RANGES(WM_SPAN) 0;
}

int main(void)
{
    mc_ctrl_cmd cmd;
    const char *bad_lcd[] = { "0:-1", "0:2", "1:65536", "1:50junk", "1:2:3", "65536:0",
        "1:9999999999999999999" };
    for (unsigned i = 0; i < sizeof bad_lcd / sizeof bad_lcd[0]; i++)
        CHECK(ctrl_parse_cmd(CTRL_OP_LCD, bad_lcd[i], &cmd) != 0, "reject invalid LCD argument %s",
                bad_lcd[i]);
    CHECK(ctrl_parse_cmd(CTRL_OP_LCD, "1:65535", &cmd) == 0,
            "preserve full unsigned 16-bit brightness readback");

    char sub[24], args[72];
    CHECK(mc_api_parse_ascset("{\"command\":\"ascset\",\"parameter\":\"0,ledmode,1\"}", sub,
                  sizeof sub, args, sizeof args, NULL) == 0,
            "core parses the ascset request");
    CHECK(tst_op_by_name(sub) == CTRL_OP_LEDMODE,
            "the wire name resolves to its op through the shared table");
    CHECK(ctrl_parse_cmd(tst_op_by_name(sub), args, &cmd) == 0,
            "core parses the args into the typed payload once, on its own side");

    {
        static const uint16_t text_ops[] = { CTRL_OP_SETTZ, CTRL_OP_FILTER_CLEAN };
        char                  at_cap[CTRL_TEXT_LEN], over_cap[CTRL_TEXT_LEN + 1];
        mc_ctrl_cmd           cmd;

        memset(at_cap, 'x', sizeof at_cap - 1);
        at_cap[sizeof at_cap - 1] = '\0';
        memset(over_cap, 'x', sizeof over_cap - 1);
        over_cap[sizeof over_cap - 1] = '\0';
        for (unsigned i = 0; i < sizeof text_ops / sizeof text_ops[0]; i++) {
            CHECK(ctrl_parse_cmd(text_ops[i], at_cap, &cmd) == 0 && !strcmp(cmd.v.text, at_cap),
                    "%s accepts a %d-byte text argument whole", ctrl_cmd_name(text_ops[i]),
                    CTRL_TEXT_LEN - 1);
            CHECK(ctrl_parse_cmd(text_ops[i], over_cap, &cmd) != 0,
                    "%s rejects a %d-byte text argument instead of cutting it",
                    ctrl_cmd_name(text_ops[i]), CTRL_TEXT_LEN);
        }
    }

    CHECK(ctrl_parse_cmd(CTRL_OP_NONE, "80", &cmd) != 0,
            "an op outside the shared table has no payload shape and is refused "
            "on the SEND side, before anything reaches the wire");

    CTRL_CMD_LIST(AVAIL_ROW)

    CTRL_CMD_LIST(SELFCONSIST_ROW)

    uint8_t   mode        = 0xEE;
    int       lvl         = -99;
    const int mid_high_ok = wm_allowed(1) && wm_allowed(2);
    CHECK(ctrl_parse_workmode("0", &mode) == 0 && mode == 0, "workmode 0 -> mode 0");
    if (mid_high_ok) {
        CHECK(ctrl_parse_workmode("1", &mode) == 0 && mode == 1, "workmode 1 -> mode 1");
        CHECK(ctrl_parse_workmode("2", &mode) == 0 && mode == 2, "workmode 2 -> mode 2");
    }
    CHECK(ctrl_parse_workmode("low", &mode) == -1, "workmode low rejected as malformed");
    CHECK(ctrl_parse_workmode("mid", &mode) == -1, "workmode mid rejected as malformed");
    CHECK(ctrl_parse_workmode("high", &mode) == -1, "workmode high rejected as malformed");
    CHECK(ctrl_parse_workmode("HIGH", &mode) == -1, "workmode HIGH rejected as malformed");
    CHECK(ctrl_parse_workmode("medium", &mode) == -1, "workmode medium rejected as malformed");
    CHECK(ctrl_parse_workmode("3", &mode) != 0, "workmode 3 rejected (>= CALI_MODE_MAX)");
    CHECK(ctrl_parse_workmode("-1", &mode) != 0, "negative workmode rejected");
    CHECK(ctrl_parse_workmode("", &mode) != 0, "empty workmode rejected");
    CHECK(ctrl_parse_workmode("2x", &mode) != 0, "trailing junk in workmode rejected");
    CHECK(ctrl_parse_workmode("turbo", &mode) != 0, "unknown workmode name rejected");
    CHECK(ctrl_parse_workmode("999999999999", &mode) != 0,
            "overlong workmode rejected (no overflow)");
    CHECK(ctrl_parse_workmode(NULL, &mode) != 0, "NULL workmode args rejected");

    CHECK(ctrl_parse_workmode("set,0", &mode) == 0 && mode == 0,
            "captured: workmode set,0 -> mode 0");
    if (mid_high_ok) {
        CHECK(ctrl_parse_workmode("set,1", &mode) == 0 && mode == 1,
                "captured: workmode set,1 -> mode 1");
        CHECK(ctrl_parse_workmode("set,2", &mode) == 0 && mode == 2,
                "captured: workmode set,2 -> mode 2");
        CHECK(ctrl_parse_workmode("SET,2", &mode) == 0 && mode == 2,
                "the set keyword is case-insensitive like the rest of the grammar");
    } else {
        CHECK(ctrl_parse_workmode("SET,0", &mode) == 0 && mode == 0,
                "the set keyword is case-insensitive like the rest of the grammar");
    }
    CHECK(ctrl_parse_workmode("set,high", &mode) == -1, "set keyword takes no name form");
    CHECK(ctrl_parse_workmode("set,low", &mode) == -1, "set keyword takes no name form");
    CHECK(ctrl_parse_workmode("set,3", &mode) != 0, "set,3 still range-checked");
    CHECK(ctrl_parse_workmode("set", &mode) != 0, "the keyword alone is not a mode");
    CHECK(ctrl_parse_workmode("set,", &mode) != 0, "set with no value rejected");
    CHECK(ctrl_parse_workmode("get,1", &mode) != 0, "only the captured keyword is skipped");

    CHECK(ctrl_parse_worklevel("0", &lvl) == 0 && lvl == 0, "worklevel 0 accepted");
    CHECK(ctrl_parse_worklevel("+0", &lvl) == 0 && lvl == 0, "worklevel +0 accepted");
    CHECK(ctrl_parse_worklevel("-0", &lvl) == 0 && lvl == 0, "worklevel -0 accepted");
    CHECK(ctrl_parse_worklevel("1", &lvl) != 0, "worklevel 1 rejected (one level per mode)");
    CHECK(ctrl_parse_worklevel("-3", &lvl) != 0, "worklevel -3 rejected (one level per mode)");
    CHECK(ctrl_parse_worklevel("", &lvl) != 0, "empty worklevel rejected");
    CHECK(ctrl_parse_worklevel("x", &lvl) != 0, "non-numeric worklevel rejected");
    CHECK(ctrl_parse_worklevel("set,0", &lvl) == 0 && lvl == 0,
            "worklevel takes the same set keyword as workmode");
    CHECK(ctrl_parse_worklevel("set,1", &lvl) != 0, "worklevel set,1 still rejected");

    mode = 0xEE;
    lvl  = -99;
    if (mid_high_ok) {
        CHECK(ctrl_parse_mode_level("0,2", &mode, &lvl) == 0 && mode == 2 && lvl == 0,
                "work_mode_lvl 0,2 -> mode 2 level 0 (level-first reading)");
        CHECK(ctrl_parse_mode_level("2,0", &mode, &lvl) == 0 && mode == 2 && lvl == 0,
                "work_mode_lvl 2,0 -> mode 2 level 0 (mode-first reading)");
    }
    CHECK(ctrl_parse_mode_level("0,0", &mode, &lvl) == 0 && mode == 0 && lvl == 0,
            "work_mode_lvl 0,0 -> mode 0 level 0 (both readings agree)");
    CHECK(ctrl_parse_mode_level("1,2", &mode, &lvl) != 0,
            "work_mode_lvl 1,2 rejected (no non-zero level exists)");
    CHECK(ctrl_parse_mode_level("0,3", &mode, &lvl) != 0, "work_mode_lvl mode 3 rejected");
    CHECK(ctrl_parse_mode_level("2", &mode, &lvl) != 0, "work_mode_lvl needs both fields");
    CHECK(ctrl_parse_mode_level("2,", &mode, &lvl) != 0,
            "work_mode_lvl empty second field rejected");
    CHECK(ctrl_parse_mode_level("2,0,0", &mode, &lvl) != 0,
            "work_mode_lvl trailing field rejected");
    CHECK(ctrl_parse_mode_level("", &mode, &lvl) != 0, "empty work_mode_lvl rejected");
    CHECK(ctrl_parse_mode_level(NULL, &mode, &lvl) != 0, "NULL work_mode_lvl args rejected");
    if (mid_high_ok) {
        CHECK(ctrl_parse_mode_level("set,2,0", &mode, &lvl) == 0 && mode == 2 && lvl == 0,
                "work_mode_lvl takes the same set keyword as workmode");
        CHECK(ctrl_parse_mode_level("2-0", &mode, &lvl) == 0 && mode == 2 && lvl == 0,
                "work_mode_lvl accepts the dash separator the app uses for ledset");
        CHECK(ctrl_parse_mode_level("set-2-0", &mode, &lvl) == 0 && mode == 2 && lvl == 0,
                "set keyword also dash-separated");
    } else {
        CHECK(ctrl_parse_mode_level("set,0,0", &mode, &lvl) == 0 && mode == 0 && lvl == 0,
                "work_mode_lvl takes the same set keyword as workmode");
        CHECK(ctrl_parse_mode_level("set-0-0", &mode, &lvl) == 0 && mode == 0 && lvl == 0,
                "set keyword also dash-separated");
    }

    for (int m = -2; m <= 12; m++) {
        int  want = wm_allowed(m);
        char arg[16];
        int  rc;
        snprintf(arg, sizeof arg, "%d", m);
        mode = 0xEE;
        rc   = ctrl_parse_workmode(arg, &mode);
        CHECK(want ? rc == 0 && mode == m : rc == -2, "[%s] workmode %d %s (rc %d)",
                BOARDCONF_SECTION, m, want ? "accepted" : "refused as out of range", rc);
        snprintf(arg, sizeof arg, "set,%d", m);
        mode = 0xEE;
        rc   = ctrl_parse_workmode(arg, &mode);
        CHECK(want ? rc == 0 && mode == m : rc == -2, "[%s] workmode set,%d %s (rc %d)",
                BOARDCONF_SECTION, m, want ? "accepted" : "refused as out of range", rc);
        snprintf(arg, sizeof arg, "0,%d", m);
        mode = 0xEE;
        rc   = ctrl_parse_mode_level(arg, &mode, &lvl);
        CHECK(want ? rc == 0 && mode == m && lvl == 0 : rc == -1,
                "[%s] work_mode_lvl 0,%d %s (rc %d)", BOARDCONF_SECTION, m,
                want ? "accepted" : "refused", rc);
        snprintf(arg, sizeof arg, "set-%d-0", m);
        mode = 0xEE;
        rc   = ctrl_parse_mode_level(arg, &mode, &lvl);
        CHECK(want ? rc == 0 && mode == m && lvl == 0 : rc == -1,
                "[%s] work_mode_lvl set-%d-0 %s (rc %d)", BOARDCONF_SECTION, m,
                want ? "accepted" : "refused", rc);
    }
    for (int m = 0; m < CALI_MODE_MAX; m++)
        CHECK(!!(CALI_MODE_BOARD_MASK & (1u << m)) == wm_allowed(m),
                "[%s] CALI_MODE_BOARD_MASK bit %d agrees with BOARD_WORKMODE_RANGES",
                BOARDCONF_SECTION, m);

    uint8_t  fx = 0xEE, bright = 0xEE;
    uint32_t temper = 0xEEEE, rgb = 0xEEEEEE, dur = 0xEEEE;
    mode = 0xEE;
    CHECK(ctrl_parse_ledmode("0", &mode) == 0 && mode == 0, "ledmode 0 -> day");
    CHECK(ctrl_parse_ledmode("1", &mode) == 0 && mode == 1, "ledmode 1 -> dark");
    CHECK(ctrl_parse_ledmode("day", &mode) != 0, "ledmode name day rejected (the API is numeric)");
    CHECK(ctrl_parse_ledmode("DARK", &mode) != 0, "ledmode name dark rejected");
    CHECK(ctrl_parse_ledmode("0x1", &mode) != 0, "ledmode hex rejected");
    CHECK(ctrl_parse_ledmode("2", &mode) != 0, "ledmode 2 rejected");
    CHECK(ctrl_parse_ledmode("256", &mode) != 0, "ledmode 256 rejected, not wrapped to 0");
    CHECK(ctrl_parse_ledmode("1x", &mode) != 0, "ledmode trailing text rejected");
    CHECK(ctrl_parse_ledmode("", &mode) != 0, "empty ledmode rejected");
    CHECK(ctrl_parse_ledmode(NULL, &mode) != 0, "NULL ledmode args rejected");

    CHECK(ctrl_parse_cmd(CTRL_OP_LEDSET, "2", &cmd) != 0,
            "wire ledset rejects incomplete settings");
    CHECK(ctrl_parse_ledset("2", &fx, &bright, &temper, &rgb) != 0 && fx == 0xEE &&
                    bright == 0xEE && temper == 0xEEEE && rgb == 0xEEEEEE,
            "ledset <effect> alone rejected, outputs untouched");
    CHECK(ctrl_parse_ledset("3,80,50,16711680", &fx, &bright, &temper, &rgb) != 0,
            "ledset four-field packed-rgb form rejected");
    CHECK(ctrl_parse_ledset("1,100,100,0,0,255", &fx, &bright, &temper, &rgb) != 0,
            "ledset comma separator rejected");
    CHECK(ctrl_parse_ledset("1-100-100-0-0-0xff", &fx, &bright, &temper, &rgb) != 0,
            "ledset hex channel rejected");
    CHECK(ctrl_parse_ledset("", &fx, &bright, &temper, &rgb) != 0, "empty ledset rejected");
    CHECK(ctrl_parse_ledset("blue", &fx, &bright, &temper, &rgb) != 0,
            "non-numeric ledset rejected");
    CHECK(ctrl_parse_ledset("1-100-100-0-0-99999999999", &fx, &bright, &temper, &rgb) != 0,
            "ledset overlong channel rejected (no overflow)");
    CHECK(ctrl_parse_ledset(NULL, &fx, &bright, &temper, &rgb) != 0, "NULL ledset args rejected");

    CHECK(ctrl_parse_ledset("2-100-100-0-0-255", &fx, &bright, &temper, &rgb) == 0 && fx == 2 &&
                    bright == 100 && temper == 100 && rgb == 0x0000FFu,
            "captured: ledset 2-100-100-0-0-255 -> BLINK, full blue");
    CHECK(ctrl_parse_ledset("4-100-100-0-0-255", &fx, &bright, &temper, &rgb) == 0 && fx == 4,
            "captured: effect 4 (COLORCYCLE) is the top of the app's sweep");
    CHECK(ctrl_parse_ledset("0-100-100-0-0-255", &fx, &bright, &temper, &rgb) == 0 && fx == 0,
            "captured: effect 0 (DARK) accepted");
    CHECK(ctrl_parse_ledset("1-49-100-0-0-125", &fx, &bright, &temper, &rgb) == 0 && bright == 49 &&
                    rgb == 0x00007Du,
            "captured: bright 49%% with blue 125");
    CHECK(ctrl_parse_ledset("1-6-100-0-0-15", &fx, &bright, &temper, &rgb) == 0 && bright == 6 &&
                    rgb == 0x00000Fu,
            "captured: bright 6%% with blue 15");
    CHECK(ctrl_parse_ledset("1-100-35-15-0-95", &fx, &bright, &temper, &rgb) == 0 && temper == 35 &&
                    rgb == 0x0F005Fu,
            "captured: temper 35 with r=15 b=95");
    CHECK(ctrl_parse_ledset("1-100-100-35-110-0", &fx, &bright, &temper, &rgb) == 0 &&
                    rgb == 0x236E00u,
            "captured: r=35 g=110 b=0 packs in wire order");
    CHECK(ctrl_parse_ledset("1-100-100-0-107-110", &fx, &bright, &temper, &rgb) == 0 &&
                    rgb == 0x006B6Eu,
            "captured: g=107 b=110");
    CHECK(ctrl_parse_ledset("1-100-100-0-0-256", &fx, &bright, &temper, &rgb) != 0,
            "six-field ledset range-checks each channel at 255");
    CHECK(ctrl_parse_ledset("5-100-100-0-0-255", &fx, &bright, &temper, &rgb) != 0,
            "ledset SHRINK rejected (night-lamp only, API effect <= COLORCYCLE)");
    CHECK(ctrl_parse_ledset("6-100-100-0-0-255", &fx, &bright, &temper, &rgb) != 0,
            "ledset EXTEND rejected");
    CHECK(ctrl_parse_ledset("1-101-100-0-0-255", &fx, &bright, &temper, &rgb) != 0,
            "ledset bright 101 rejected");
    CHECK(ctrl_parse_ledset("1-100-70000-0-0-255", &fx, &bright, &temper, &rgb) != 0,
            "ledset temper 70000 rejected");
    CHECK(ctrl_parse_ledset("1-100-100-0-0-255-0", &fx, &bright, &temper, &rgb) != 0,
            "seventh field rejected");
    CHECK(ctrl_parse_ledset("1-100-100-0-0", &fx, &bright, &temper, &rgb) != 0,
            "five fields rejected (exactly six)");
    CHECK(ctrl_parse_ledset("1-100-100-0-0-", &fx, &bright, &temper, &rgb) != 0,
            "ledset trailing dash rejected");

    mode = 0xEE;
    CHECK(ctrl_parse_nightlamp("1-180", &mode, &dur) == 0 && mode == 1 && dur == 180,
            "nightlamp 1-180 -> the API dash spelling");
    CHECK(ctrl_parse_nightlamp("0-180", &mode, &dur) == 0 && mode == 0,
            "nightlamp 0-180 -> off, dash spelling");
    CHECK(ctrl_parse_nightlamp("1-600", &mode, &dur) == 0 && mode == 1 && dur == 600,
            "nightlamp dash form carries the duration");
    CHECK(ctrl_parse_nightlamp("1-0", &mode, &dur) == 0 && mode == 1 && dur == 0,
            "nightlamp duration 0 stays 0 (mm applies the default)");
    CHECK(ctrl_parse_nightlamp("1-70000", &mode, &dur) != 0,
            "nightlamp duration 70000 rejected (uint16 seconds, no wrap)");
    CHECK(ctrl_parse_nightlamp("1-60-1", &mode, &dur) != 0, "nightlamp third field rejected");
    CHECK(ctrl_parse_nightlamp("1-", &mode, &dur) != 0, "nightlamp empty duration rejected");
    CHECK(ctrl_parse_nightlamp("1,600", &mode, &dur) != 0, "nightlamp comma separator rejected");
    CHECK(ctrl_parse_nightlamp("1", &mode, &dur) != 0,
            "nightlamp without a duration rejected (the API requires both)");
    CHECK(ctrl_parse_nightlamp("2-60", &mode, &dur) != 0, "nightlamp mode 2 rejected");
    CHECK(ctrl_parse_nightlamp("1-0x3c", &mode, &dur) != 0, "nightlamp hex duration rejected");
    CHECK(ctrl_parse_nightlamp("", &mode, &dur) != 0, "empty nightlamp rejected");
    CHECK(ctrl_parse_nightlamp("on-60", &mode, &dur) != 0, "non-numeric nightlamp mode rejected");
    CHECK(ctrl_parse_nightlamp(NULL, &mode, &dur) != 0, "NULL nightlamp args rejected");

    int     imm;
    uint8_t val;
    int64_t ts;

    imm = 0;
    CHECK(ctrl_parse_softsched("", &imm, &val, &ts) == 0 && imm == 1,
            "an empty payload is the IMMEDIATE form");
    imm = 0;
    CHECK(ctrl_parse_softsched("now", &imm, &val, &ts) == 0 && imm == 1,
            "\"now\" is the immediate form too");
    imm = 0;
    CHECK(ctrl_parse_softsched("NOW", &imm, &val, &ts) == 0 && imm == 1, "...case-insensitively");

    imm = 1;
    val = 0;
    ts  = 0;
    CHECK(ctrl_parse_softsched("1755000000", &imm, &val, &ts) == 0 && imm == 0 && val == 1 &&
                    ts == T_OK,
            "a lone epoch ARMS at that second");
    imm = 1;
    val = 0;
    ts  = 0;
    CHECK(ctrl_parse_softsched("t:1755000000", &imm, &val, &ts) == 0 && val == 1 && ts == T_OK,
            "...and the `time` command's t: prefix is accepted");
    val = 0;
    ts  = 0;
    CHECK(ctrl_parse_softsched("T:1755000000", &imm, &val, &ts) == 0 && imm == 0 && val == 1 &&
                    ts == T_OK,
            "T: prefix accepted, case-insensitive");
    CHECK(ctrl_parse_softsched("set,T:0,1755000000", &imm, &val, &ts) == 0 && imm == 0 &&
                    val == 0 && ts == T_OK,
            "set,T:<valid>,<ts> accepted");
    CHECK(ctrl_parse_softsched("set,1755000000", &imm, &val, &ts) == 0 && val == 1 && ts == T_OK,
            "...as is the app's `set` keyword");

    val = 9;
    ts  = 0;
    CHECK(ctrl_parse_softsched("1,1755000000", &imm, &val, &ts) == 0 && val == 1 && ts == T_OK,
            "two fields: valid=1 arms");
    val = 9;
    ts  = 0;
    CHECK(ctrl_parse_softsched("0,1755000000", &imm, &val, &ts) == 0 && val == 0 && ts == T_OK,
            "valid=0 DISARMS, and still carries its timestamp");
    val = 9;
    ts  = 0;
    CHECK(ctrl_parse_softsched("0-1755000000", &imm, &val, &ts) == 0 && val == 0 && ts == T_OK,
            "'-' separates as well as ','");
    val = 9;
    ts  = 0;
    CHECK(ctrl_parse_softsched("set,0,1755000000", &imm, &val, &ts) == 0 && val == 0 && ts == T_OK,
            "`set` keyword before the two-field form");

    val = 9;
    ts  = -1;
    CHECK(ctrl_parse_softsched("1", &imm, &val, &ts) == 0 && val == 1 && ts == 1,
            "a lone 1 is an epoch, not a bare flag");
    val = 9;
    ts  = -1;
    CHECK(ctrl_parse_softsched("0", &imm, &val, &ts) == 0 && val == 1 && ts == 0,
            "a lone 0 is epoch 0, accepted");
    val = 9;
    ts  = 0;
    CHECK(ctrl_parse_softsched("4133981573", &imm, &val, &ts) == 0 && val == 1 &&
                    ts == 4133981573LL,
            "an epoch after 2100 is accepted");
    val = 9;
    ts  = 0;
    CHECK(ctrl_parse_softsched("1,9223372036854775807", &imm, &val, &ts) == 0 && val == 1 &&
                    ts == INT64_MAX,
            "INT64_MAX is accepted");
    CHECK(ctrl_parse_softsched("9223372036854775808", &imm, &val, &ts) != 0,
            "INT64_MAX + 1 is refused, not wrapped");
    CHECK(ctrl_parse_softsched("99999999999999999999", &imm, &val, &ts) != 0,
            "an over-long digit run is refused, not wrapped");
    CHECK(ctrl_parse_softsched("2,1755000000", &imm, &val, &ts) != 0,
            "a first field above 1 is not a valid flag");
    CHECK(ctrl_parse_softsched("1755000000,1755000100", &imm, &val, &ts) != 0,
            "two epochs are refused — field 1 is the flag, so this is ambiguous");
    CHECK(ctrl_parse_softsched("1,1755000000,1", &imm, &val, &ts) != 0, "a third field is refused");
    CHECK(ctrl_parse_softsched("1,", &imm, &val, &ts) != 0, "an empty epoch is refused");
    CHECK(ctrl_parse_softsched("1755000000x", &imm, &val, &ts) != 0, "trailing junk is refused");
    CHECK(ctrl_parse_softsched("-1755000000", &imm, &val, &ts) != 0, "a negative epoch is refused");
    CHECK(ctrl_parse_softsched("later", &imm, &val, &ts) != 0, "a non-numeric payload is refused");
    CHECK(ctrl_parse_softsched("t:", &imm, &val, &ts) != 0, "a bare t: prefix is refused");
    CHECK(ctrl_parse_softsched(NULL, &imm, &val, &ts) != 0, "NULL args refused");
    CHECK(ctrl_parse_softsched("now", NULL, &val, &ts) != 0, "NULL out-pointer refused");

    ctrl_ip_t ipc;
    CHECK(ctrl_parse_ip("dhcp", &ipc) == 0 && ipc.is_static == 0, "ip dhcp -> dhcp");
    CHECK(ctrl_parse_ip("DHCP", &ipc) == 0 && ipc.is_static == 0, "ip dhcp is case-insensitive");
    CHECK(ctrl_parse_ip("static,192.168.1.66,255.255.255.0,192.168.1.1", &ipc) == 0 &&
                    ipc.is_static == 1 && !strcmp(ipc.ip, "192.168.1.66") &&
                    !strcmp(ipc.mask, "255.255.255.0") && !strcmp(ipc.gw, "192.168.1.1") &&
                    ipc.dns[0] == 0,
            "ip static without dns parses, dns empty");
    CHECK(ctrl_parse_ip("static,10.0.0.2,255.0.0.0,10.0.0.1,8.8.8.8", &ipc) == 0 &&
                    ipc.is_static == 1 && !strcmp(ipc.dns, "8.8.8.8"),
            "ip static with dns parses");
    CHECK(ctrl_parse_ip("STATIC,10.0.0.2,255.0.0.0,10.0.0.1", &ipc) == 0 && ipc.is_static == 1,
            "the static keyword is case-insensitive");
    CHECK(ctrl_parse_ip("static,0.0.0.0,0.0.0.0,0.0.0.0", &ipc) == 0,
            "all-zero quads parse (range check is the actuator's job, not the grammar)");
    CHECK(ctrl_parse_ip("static,255.255.255.255,255.255.255.255,255.255.255.255", &ipc) == 0,
            "255 in every octet accepted");

    CHECK(ctrl_parse_ip("", &ipc) != 0, "empty ip rejected");
    CHECK(ctrl_parse_ip(NULL, &ipc) != 0, "NULL ip args rejected");
    CHECK(ctrl_parse_ip("dhcp,192.168.1.1", &ipc) != 0, "dhcp with trailing junk rejected");
    CHECK(ctrl_parse_ip("static", &ipc) != 0, "static with no fields rejected");
    CHECK(ctrl_parse_ip("static,192.168.1.66", &ipc) != 0, "static missing mask+gw rejected");
    CHECK(ctrl_parse_ip("static,192.168.1.66,255.255.255.0", &ipc) != 0,
            "static missing gw rejected");
    CHECK(ctrl_parse_ip("static,192.168.1.66,255.255.255.0,192.168.1.1,", &ipc) != 0,
            "trailing comma (empty dns) rejected");
    CHECK(ctrl_parse_ip("static,192.168.1.66,255.255.255.0,192.168.1.1,8.8.8.8,1.1.1.1", &ipc) != 0,
            "a sixth field rejected");
    CHECK(ctrl_parse_ip("static,256.1.1.1,255.255.255.0,192.168.1.1", &ipc) != 0,
            "octet over 255 rejected");
    CHECK(ctrl_parse_ip("static,192.168.1,255.255.255.0,192.168.1.1", &ipc) != 0,
            "three-octet address rejected");
    CHECK(ctrl_parse_ip("static,192.168.1.1.1,255.255.255.0,192.168.1.1", &ipc) != 0,
            "five-octet address rejected");
    CHECK(ctrl_parse_ip("static,192.168.1.,255.255.255.0,192.168.1.1", &ipc) != 0,
            "empty trailing octet rejected");
    CHECK(ctrl_parse_ip("static,192.168.01.1,255.255.255.0,192.168.1.1", &ipc) == 0,
            "leading-zero octet accepted (<= 3 digits, <= 255)");
    CHECK(ctrl_parse_ip("static,192.168.1111.1,255.255.255.0,192.168.1.1", &ipc) != 0,
            "four-digit octet rejected (no overflow)");
    CHECK(ctrl_parse_ip("static,19 2.168.1.1,255.255.255.0,192.168.1.1", &ipc) != 0,
            "embedded space rejected (shell-injection guard)");
    CHECK(ctrl_parse_ip("static,192.168.1.1;reboot,255.255.255.0,192.168.1.1", &ipc) != 0,
            "shell metacharacters rejected");

    CHECK(ctrl_parse_cmd(CTRL_OP_REBOOT, "3,7,30", &cmd) == 0 && cmd.v.reboot.reason == 3 &&
                    cmd.v.reboot.sub == 7 && cmd.v.reboot.delay_s == 30,
            "reboot reason,sub,delay carries the delay to mm");
    CHECK(ctrl_parse_cmd(CTRL_OP_REBOOT, "3,0", &cmd) == 0 && cmd.v.reboot.delay_s == 0,
            "reboot without a delay field means delay 0");
    CHECK(ctrl_parse_cmd(CTRL_OP_REBOOT, "3,0,300", &cmd) == 0 &&
                    cmd.v.reboot.delay_s == CTRL_REBOOT_DELAY_MAX,
            "reboot delay 300 is the top of the range");
    CHECK(ctrl_parse_cmd(CTRL_OP_REBOOT, "3,0,301", &cmd) != 0, "reboot delay 301 rejected");
    CHECK(ctrl_parse_cmd(CTRL_OP_REBOOT, "3,0,30x", &cmd) != 0,
            "reboot delay with trailing junk rejected");

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
