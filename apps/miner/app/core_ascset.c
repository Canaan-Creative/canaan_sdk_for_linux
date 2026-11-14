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

#include "core_ascset.h"
#include "core_ascset_internal.h"
#include "api_plugin.h"
#include "cali.h"
#include "core_activate.h"
#include "core_ident.h"
#include "hex.h"
#include "mc_api_bootby.h"
#include "cfgwr_client.h"
#include "ctrl_args.h"
#include "ctrl_cmds.h"
#include "mc_api_setter.h"
#include "mc_api_qrlogin.h"
#include "mc_api_time.h"
#include "miner_ipc.h"
#include "sha256.h"
#include "webauth.h"
#include "core_board.h"
#include "core_ctrl.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define QR_VERIFY_SIGN        "ff0000ee" /* QR login proof prefix */
#define PASSWORD_PLAIN_MAX    64         /* plain web password max length, bytes */
#define CTRL_FWD_WORKMODE     workmode_setter
#define CTRL_FWD_LEDMODE      led_setter
#define CTRL_FWD_LEDSET       led_setter
#define CTRL_FWD_NIGHTLAMP    led_setter
#define CTRL_FWD_FILTER_CLEAN filterclean_setter
#define CTRL_FWD_LCD          lcd_setter

/* Register an mm_miner ascset setting */
#define REG_FWD(e, name, payload, cap) \
    if (ctrl_cmd_available(CTRL_OP_##e)) { \
        if (mc_api_setter_register(name, CTRL_OP_##e, CTRL_FWD_##e, NULL) == 0) \
            n++; \
        else \
            osif_log(OSIF_LOG_ERR, "API setting %s unavailable", ctrl_cmd_name(CTRL_OP_##e)); \
    }
#define REG_PLUGIN(name, fn, need_op, flags, help_before, reads) \
    mc_api_setter_declare(name, flags, help_before, reads); \
    if ((need_op) == CTRL_OP_NONE || ctrl_cmd_available(need_op)) { \
        if (mc_api_setter_register(name, CTRL_OP_NONE, fn, (void *)g_systemcfg) == 0) \
            n++; \
        else \
            osif_log(OSIF_LOG_ERR, "API setting %s unavailable", name); \
    }

static const char     *g_systemcfg = MC_SYSTEMCFG_DEFAULT;
static char            g_activation_buf[256];
static const char     *g_activation = MC_ACTIVATION_DEFAULT;
static _Atomic uint8_t g_ledmode;
static pthread_mutex_t g_tz_lock = PTHREAD_MUTEX_INITIALIZER;
static char            g_tz[MM_TZ_NAME_CAP]; /* timezone name from the last MSG_MM_PERIPH */

void core_setters_ledmode_set(uint8_t mode)
{
    atomic_store(&g_ledmode, mode);
}

uint8_t core_setters_ledmode(void)
{
    return atomic_load(&g_ledmode);
}

void core_setters_tz_set(const char *name)
{
    pthread_mutex_lock(&g_tz_lock);
    snprintf(g_tz, sizeof g_tz, "%s", name ? name : "");
    pthread_mutex_unlock(&g_tz_lock);
}

static void sha256_hex_n(const char *a, size_t alen, const char *b, char *out, size_t hexchars)
{
    sha256_ctx    c;
    unsigned char d[SHA256_DIGEST_SIZE];
    char          hex[2 * SHA256_DIGEST_SIZE + 1];
    sha256_init(&c);
    sha256_update(&c, a, alen);
    if (b && *b)
        sha256_update(&c, b, strlen(b));
    sha256_final(&c, d);
    if (hexchars > 64)
        hexchars = 64;
    hex_encode(hex, d, sizeof d);
    memcpy(out, hex, hexchars);
    out[hexchars] = 0;
}

static mc_set_result forward_cmd(const char *name, const mc_ctrl_cmd *cmd, char *msg, size_t msgcap,
        const char *ok_reason)
{
    if (osif_ipc_send(g_ipc_core_mm, MSG_CTRL_CMD, cmd, (int)sizeof *cmd) != OSIF_OK) {
        snprintf(msg, msgcap, "control channel down");
        return MC_SET_ERR_HW;
    }
    mc_api_setter_mark_forwarded();
    osif_log(OSIF_LOG_DBG, "ascset %s accepted, %s", name, ok_reason);
    return MC_SET_OK;
}

static mc_set_result forward(uint16_t op, const char *args, char *msg, size_t msgcap,
        const char *ok_reason)
{
    mc_ctrl_cmd cmd;
    if (ctrl_parse_cmd(op, args, &cmd) != 0) {
        snprintf(msg, msgcap, "invalid %s setting", ctrl_cmd_name(op));
        return MC_SET_ERR_ARG;
    }
    return forward_cmd(ctrl_cmd_name(op), &cmd, msg, msgcap, ok_reason);
}

mc_set_result core_ascset_forward(uint16_t op, const char *args, char *msg, size_t msgcap,
        const char *ok_reason)
{
    return forward(op, args, msg, msgcap, ok_reason);
}

static mc_set_result workmode_check(const char *name, const char *args, int bad, uint8_t mode,
        const mc_api_stats *s, const char *aging_fmt, char *msg, size_t msgcap)
{
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing %s setting", name);
        return MC_SET_ERR_ARG;
    }
    if (bad) {
        char shown[MC_API_ARGS_LOG_CAP];
        osif_log(OSIF_LOG_WARN, "ascset %s refused, raw args \"%s\"", name,
                mc_api_args_redact(args, shown, sizeof shown));
        if (bad == -2 && s)
            snprintf(msg, msgcap, "error:1 not support set workmode max_mode[%d]", s->maxmode);
        else
            snprintf(msg, msgcap, "error:2 parameter error");
        return MC_SET_ERR_ARG;
    }
    if (s && mode > s->maxmode) {
        snprintf(msg, msgcap, "error:1 not support set workmode max_mode[%d]", s->maxmode);
        return MC_SET_ERR_ARG;
    }
    if (s && s->adj == 0) {
        snprintf(msg, msgcap, aging_fmt, s->workmode);
        return MC_SET_ERR_ARG;
    }
    return MC_SET_OK;
}

static int is_get(const char *args)
{
    return args && !strcmp(args, "get");
}

static mc_set_result workmode_setter(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)ctx;
    if (is_get(args) && s) {
        snprintf(msg, msgcap, "workmode %d", s->workmode);
        return MC_SET_INFO;
    }
    uint8_t mode = 0;
    int     bad  = ctrl_parse_workmode(args ? args : "", &mode);
    /* Refusal text kept byte for byte for the app. */
    mc_set_result r = workmode_check("workmode", args, bad, mode, s,
            "current mode %d is caling Don't permit switch mode", msg, msgcap);
    if (r != MC_SET_OK)
        return r;

    char reason[64];
    snprintf(reason, sizeof reason, "workmode set to %u", (unsigned)mode);
    return forward(op, args, msg, msgcap, reason);
}

static mc_set_result work_mode_lvl_setter(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)op;
    (void)ctx;
    if (is_get(args) && s) {
        snprintf(msg, msgcap, "workmode %d worklevel 0", s->workmode);
        return MC_SET_INFO;
    }
    uint8_t       mode  = 0;
    int           level = 0;
    int           bad   = ctrl_parse_mode_level(args ? args : "", &mode, &level);
    mc_set_result r     = workmode_check("work_mode_lvl", args, bad, mode, s,
                "current mode %d is caling. Don't permit switch mode", msg, msgcap);
    if (r != MC_SET_OK)
        return r;

    mc_ctrl_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.op               = CTRL_OP_WORKMODE;
    cmd.v.workmode.mode  = mode;
    cmd.v.workmode.level = level;
    char reason[64];
    snprintf(reason, sizeof reason, "work_mode_lvl set to %u", (unsigned)mode);
    return forward_cmd("work_mode_lvl", &cmd, msg, msgcap, reason);
}

static mc_set_result worklevel_setter(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)op;
    (void)s;
    (void)ctx;
    int level = 0;
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing worklevel setting");
        return MC_SET_ERR_ARG;
    }
    if (is_get(args)) {
        snprintf(msg, msgcap, "worklevel 0");
        return MC_SET_INFO;
    }
    if (ctrl_parse_worklevel(args, &level) != 0) {
        char shown[MC_API_ARGS_LOG_CAP];
        osif_log(OSIF_LOG_WARN, "ascset worklevel refused, raw args \"%s\"",
                mc_api_args_redact(args, shown, sizeof shown));
        snprintf(msg, msgcap, "worklevel unknown argument");
        return MC_SET_ERR_ARG;
    }
    osif_log(OSIF_LOG_DBG, "ascset worklevel accepted, worklevel already 0");
    return MC_SET_OK;
}

static mc_set_result led_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)ctx;
    const char *name       = ctrl_cmd_name(op);
    int         is_ledmode = 0, is_nightlamp = 0;
    switch (op) {
    case CTRL_OP_LEDMODE:
        is_ledmode = 1;
        break;
    case CTRL_OP_LEDSET:
        break;
    case CTRL_OP_NIGHTLAMP:
        is_nightlamp = 1;
        break;
    default:
        snprintf(msg, msgcap, "Unknown option: %s", name);
        return MC_SET_ERR_ARG;
    }

    if (!args || !*args) {
        snprintf(msg, msgcap, "missing %s setting", name);
        return MC_SET_ERR_ARG;
    }

    if (!is_ledmode && s) {
        if (is_nightlamp && !s->ledmode) {
            snprintf(msg, msgcap, "not in (dark)mode now");
            return MC_SET_ERR_ARG;
        }
        if (!is_nightlamp && s->ledmode) {
            snprintf(msg, msgcap, "not in (day)mode now");
            return MC_SET_INFO;
        }
    }

    uint8_t  mode = 0, effect = 0, bright = 0;
    uint32_t temper = 0, rgb = 0, dur_s = 0;
    int      bad;
    if (is_ledmode)
        bad = ctrl_parse_ledmode(args, &mode);
    else if (is_nightlamp)
        bad = ctrl_parse_nightlamp(args, &mode, &dur_s);
    else
        bad = ctrl_parse_ledset(args, &effect, &bright, &temper, &rgb);
    if (bad) {
        char shown[MC_API_ARGS_LOG_CAP];
        osif_log(OSIF_LOG_WARN, "ascset %s refused, raw args \"%s\"", name,
                mc_api_args_redact(args, shown, sizeof shown));
        snprintf(msg, msgcap,
                is_ledmode || is_nightlamp ? "Input param error." : "led parameter wrong");
        return MC_SET_ERR_ARG;
    }

    char reason[64];
    if (is_ledmode)
        snprintf(reason, sizeof reason, "ledmode set to %s", mode ? "dark" : "day");
    else if (is_nightlamp)
        snprintf(reason, sizeof reason, mode ? "nightlamp on for %us" : "nightlamp off", dur_s);
    else
        snprintf(reason, sizeof reason, "ledset effect %u", effect);

    mc_set_result r = forward(op, args, msg, msgcap, reason);
    if (r != MC_SET_OK)
        return r;

    if (is_ledmode)
        core_setters_ledmode_set(mode);

    if (is_ledmode)
        snprintf(msg, msgcap, "success set:%u", mode);
    else if (is_nightlamp)
        snprintf(msg, msgcap, "success set:%u %u", mode, dur_s);
    else
        snprintf(msg, msgcap, "led set ok");
    return MC_SET_INFO;
}

static int webpass_field(const char *field, size_t len)
{
    char pass[WEBPASS_HEX_LEN + 1], hex[WEBPASS_HEX_LEN + 1];

    if (len > PASSWORD_PLAIN_MAX || core_webpass_cached_copy(pass, sizeof pass) != 0)
        return 0;
    core_webpass_sha256_hex(field, len, hex);
    return !strcmp(hex, pass);
}

static mc_set_result qrauth_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)op;
    (void)ctx;
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing qr_auth setting");
        return MC_SET_ERR_ARG;
    }
    size_t nonce_len = strcspn(args, ",");
    if (!nonce_len || !args[nonce_len]) {
        mc_api_setter_mark_noop();
        return MC_SET_OK;
    }
    const char *verify_at  = args + nonce_len + 1;
    size_t      verify_len = strcspn(verify_at, ",");
    if (!verify_len) {
        mc_api_setter_mark_noop();
        return MC_SET_OK;
    }
    if (nonce_len >= 128 || verify_len >= 128) {
        snprintf(msg, msgcap, "input length error!");
        return MC_SET_ERR_ARG;
    }
    char nonce_in[129] = { 0 }, verify_in[129] = { 0 };
    memcpy(nonce_in, args, nonce_len);
    memcpy(verify_in, verify_at, verify_len);

    if (!mc_api_qr_login_pending(nonce_in)) {
        snprintf(msg, msgcap, "auth error 01!");
        return MC_SET_ERR_ARG;
    }

    char        pass[WEBPASS_HEX_LEN + 1];
    const char *dna = s && s->dna ? s->dna : "";
    if (core_webpass_cached_copy(pass, sizeof pass) != 0 || !*dna) {
        snprintf(msg, msgcap, "passwd error 02!");
        return MC_SET_ERR_ARG;
    }

    char sha_dna[65], verify[81], bulk[64];
    sha256_hex_n(dna, strlen(dna), "", sha_dna, 24);
    snprintf(bulk, sizeof bulk, "%.24s%.24s", sha_dna, pass);
    char proof[65];
    sha256_hex_n(bulk, strlen(bulk), "", proof, 24);
    snprintf(verify, sizeof verify, "%s%.24s", QR_VERIFY_SIGN, proof);

    if (!core_webpass_proof_matches(verify, verify_in)) {
        snprintf(msg, msgcap, "passwd error 02!");
        return MC_SET_ERR_ARG;
    }
    if (!mc_api_qr_login_approve(nonce_in)) {
        snprintf(msg, msgcap, "auth error 01!");
        return MC_SET_ERR_ARG;
    }
    osif_log(OSIF_LOG_DBG, "QR code login approved");
    return MC_SET_OK;
}

static mc_set_result tz_forward(const char *tzname)
{
    if (!tzname || !*tzname)
        return MC_SET_OK;
    mc_ctrl_cmd cmd;
    mc_ctrl_ack ack;
    if (ctrl_parse_cmd(CTRL_OP_SETTZ, tzname, &cmd) != 0) {
        osif_log(OSIF_LOG_WARN, "settz %s not forwarded (invalid)", tzname);
        return MC_SET_ERR_ARG;
    }
    switch (core_ctrl_send(&cmd, &ack)) {
    case 0:
        break;
    case -1:
        osif_log(OSIF_LOG_WARN, "settz %s not forwarded (control channel down)", tzname);
        return MC_SET_ERR_HW;
    default:
        osif_log(OSIF_LOG_WARN, "settz %s not confirmed: no answer from mm_miner", tzname);
        return MC_SET_ERR_HW;
    }
    if (ack.result != MC_SET_OK) {
        osif_log(OSIF_LOG_WARN, "settz %s refused by mm_miner (result %d)", tzname,
                (int)ack.result);
        return ack.result == MC_SET_ERR_ARG ? MC_SET_ERR_ARG : MC_SET_ERR_HW;
    }
    osif_log(OSIF_LOG_INFO, "settz %s applied by mm_miner", tzname);
    return MC_SET_OK;
}

static int tz_read(char *out, size_t cap)
{
    pthread_mutex_lock(&g_tz_lock);
    snprintf(out, cap, "%s", g_tz);
    pthread_mutex_unlock(&g_tz_lock);
    return 0;
}

static mc_set_result password_setter(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)op;
    (void)s;
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing password setting");
        return MC_SET_ERR_ARG;
    }
    const char *comma = strchr(args, ',');
    int         nargs = comma == args ? 0 : (!comma || !comma[1] || comma[1] == ',') ? 1 : 2;
    if (nargs != 2) {
        snprintf(msg, msgcap, "we need 2 arguments, your input is %d arguments.", nargs);
        return MC_SET_ERR_ARG;
    }
    size_t      oldlen = (size_t)(comma - args);
    const char *newpw  = comma + 1;
    size_t      newlen = strcspn(newpw, ",");
    if (oldlen > PASSWORD_PLAIN_MAX || newlen > PASSWORD_PLAIN_MAX) {
        snprintf(msg, msgcap, "password too long");
        return MC_SET_ERR_ARG;
    }

    const char *cfg = ctx ? (const char *)ctx : MC_SYSTEMCFG_DEFAULT;
    if (!core_webpass_matches(args, oldlen)) {
        snprintf(msg, msgcap, "Auth Failed! password invalid!");
        return MC_SET_ERR_ARG;
    }

    char newhex[WEBPASS_HEX_LEN + 1];
    core_webpass_sha256_hex(newpw, newlen, newhex);
    if (cfgwr_write_webpass(newhex, cfg) != 0) {
        snprintf(msg, msgcap, "config write error");
        return MC_SET_ERR_HW;
    }
    core_webpass_cache_set(newhex);
    osif_log(OSIF_LOG_DBG, "web password changed");
    snprintf(msg, msgcap, "new password success set.");
    return MC_SET_INFO;
}

static int core_platform_reboot_with_reason(uint8_t reason, uint32_t sub, uint16_t delay_s)
{
    mc_ctrl_cmd cmd      = { .op = CTRL_OP_REBOOT };
    cmd.v.reboot.reason  = reason;
    cmd.v.reboot.sub     = sub;
    cmd.v.reboot.delay_s = delay_s;
    if (osif_ipc_send(g_ipc_core_mm, MSG_CTRL_CMD, &cmd, (int)sizeof cmd) != OSIF_OK) {
        osif_log(OSIF_LOG_ERR, "reboot request not sent: mm_miner unreachable");
        return -1;
    }
    mc_api_setter_mark_forwarded();
    return 0;
}

static mc_set_result reboot_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)op;
    (void)s;
    (void)ctx;
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing reboot setting");
        return MC_SET_ERR_ARG;
    }

    unsigned delay = 0;
    for (const char *p = args; *p; p++) {
        if (*p < '0' || *p > '9') {
            snprintf(msg, msgcap, "reboot value not number");
            return MC_SET_ERR_ARG;
        }
        delay = delay * 10 + (unsigned)(*p - '0');
        if (delay > CTRL_REBOOT_DELAY_MAX) {
            snprintf(msg, msgcap, "reboot value should be 0~300 seconds");
            return MC_SET_ERR_ARG;
        }
    }
    if (core_platform_reboot_with_reason(MC_BOOTBY_API, 0, (uint16_t)delay) != 0) {
        snprintf(msg, msgcap, "reboot request not forwarded");
        return MC_SET_ERR_HW;
    }
    return MC_SET_OK;
}

static mc_set_result reboot_web_setter(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)op;
    (void)args;
    (void)s;
    (void)ctx;
    if (!mc_api_peer_trusted()) {
        snprintf(msg, msgcap, "Invalid command");
        return MC_SET_ERR_ARG;
    }
    if (core_platform_reboot_with_reason(MC_BOOTBY_WEB, 0, 0) != 0) {
        snprintf(msg, msgcap, "reboot request not forwarded");
        return MC_SET_ERR_HW;
    }
    return MC_SET_OK;
}

static mc_set_result activate_setter(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)op;
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing activate setting");
        return MC_SET_ERR_ARG;
    }
    const char *dna = s && s->dna ? s->dna : "";
    if (!*dna || !core_activate_verify(dna, args)) {
        snprintf(msg, msgcap, "set activation param error");
        return MC_SET_ERR_ARG;
    }
    const char *cfg = ctx ? (const char *)ctx : MC_ACTIVATION_DEFAULT;
    if (cfgwr_write_activation(cfg) != 0) {
        snprintf(msg, msgcap, "config write error");
        return MC_SET_ERR_HW;
    }
    osif_log(OSIF_LOG_DBG, "device activated");
    return MC_SET_OK;
}

static mc_set_result filterclean_setter(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx)
{
    (void)s;
    (void)ctx;
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing filter-clean setting");
        return MC_SET_ERR_ARG;
    }
    int value;
    if (sscanf(args, "%d", &value) < 1) {
        snprintf(msg, msgcap, "No value passed to avalon-filter-clean");
        return MC_SET_ERR_ARG;
    }
    if (value != 1) {
        snprintf(msg, msgcap, "Input param error.");
        return MC_SET_ERR_ARG;
    }
    return forward(op, args, msg, msgcap, "filter-clean acknowledged");
}

static mc_set_result loop_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)op;
    (void)args;
    (void)ctx;
    int n      = snprintf(msg, msgcap, "LOOP[");
    int boards = s ? s->telem.board_n : 0;
    if (boards > HBOARD_COUNT)
        boards = HBOARD_COUNT;
    for (int i = 0; i < boards && n >= 0 && (size_t)n < msgcap; i++)
        n += snprintf(msg + n, msgcap - (size_t)n, "%d ", s->telem.chip_n[i]);
    if (n >= 0 && (size_t)n < msgcap)
        snprintf(msg + n, msgcap - (size_t)n, "]");
    return MC_SET_INFO;
}

static mc_set_result lcd_setter(uint16_t op, const char *args, const mc_api_stats *s, char *msg,
        size_t msgcap, void *ctx)
{
    (void)s;
    (void)ctx;
    if (!args || !*args) {
        snprintf(msg, msgcap, "missing lcd setting");
        return MC_SET_ERR_ARG;
    }
    uint16_t id, val;
    if (ctrl_parse_lcd(args, &id, &val) != 0) {
        snprintf(msg, msgcap, "No value passed to lcd");
        return MC_SET_ERR_ARG;
    }
    mc_set_result r = forward(op, args, msg, msgcap, "lcd write");
    if (r != MC_SET_OK)
        return r;
    snprintf(msg, msgcap, "success lcd:id[%d] val[%d] len=4", (uint16_t)id, (uint16_t)val);
    return MC_SET_INFO;
}

int core_setters_register(const char *systemcfg_ini)
{
    mc_api_time_set_forwarder(tz_forward);
    mc_api_time_set_tz_reader(tz_read);
    mc_api_setter_set_secret(webpass_field);
    g_systemcfg        = systemcfg_ini && *systemcfg_ini ? systemcfg_ini : MC_SYSTEMCFG_DEFAULT;
    const char *slash  = strrchr(g_systemcfg, '/');
    int         dirlen = slash ? (int)(slash - g_systemcfg + 1) : 0;
    snprintf(g_activation_buf, sizeof g_activation_buf, "%.*sactivation.ini", dirlen, g_systemcfg);
    g_activation = g_activation_buf;

    int n = 0;

    CTRL_CMD_ASCSET(REG_FWD)
    if (ctrl_cmd_available(CTRL_OP_WORKMODE))
        mc_api_setter_declare("workmode", 0, NULL, is_get);

    /* Setter context pointer: none / systemcfg path / activation file path */
    enum { CTX_NONE, CTX_SYSCFG, CTX_ACTIVATION };
    /* Locally handled ascset settings */
    static const struct {
        const char      *name;
        mc_api_setter_fn fn;
        int              ctx;    /* setter context selector (CTX_*) */
        uint16_t        need_op; /* control op the board must offer; CTRL_OP_NONE: on every board */
        mc_api_reads_fn reads;
    } tab[] = {
        { "qr_auth", qrauth_setter, CTX_SYSCFG, CTRL_OP_NONE, NULL },
        { "password", password_setter, CTX_SYSCFG, CTRL_OP_NONE, NULL },
        { "reboot", reboot_setter, CTX_NONE, CTRL_OP_REBOOT, NULL },
        { "reboot_web", reboot_web_setter, CTX_NONE, CTRL_OP_REBOOT, NULL },
        { "worklevel", worklevel_setter, CTX_NONE, CTRL_OP_WORKMODE, is_get },
        { "work_mode_lvl", work_mode_lvl_setter, CTX_NONE, CTRL_OP_WORKMODE, is_get },
        { "activate", activate_setter, CTX_ACTIVATION, CTRL_OP_NONE, NULL },
        { "loop", loop_setter, CTX_NONE, CTRL_OP_NONE, mc_api_setter_always_reads },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (tab[i].need_op != CTRL_OP_NONE && !ctrl_cmd_available(tab[i].need_op))
            continue;
        if (tab[i].reads)
            mc_api_setter_declare(tab[i].name, 0, NULL, tab[i].reads);
#ifndef CAP_ACTIVATION
        if (tab[i].ctx == CTX_ACTIVATION)
            continue;
#endif
        /* clang-format off */
        void *sctx = tab[i].ctx == CTX_SYSCFG ? (void *)g_systemcfg :
                tab[i].ctx == CTX_ACTIVATION ? (void *)g_activation :
                NULL;
        /* clang-format on */
        if (mc_api_setter_register(tab[i].name, CTRL_OP_NONE, tab[i].fn, sctx) == 0)
            n++;
        else
            osif_log(OSIF_LOG_ERR, "API setting %s unavailable", tab[i].name);
    }

    API_PLUGIN_SETTERS(REG_PLUGIN)
    return n;
}
