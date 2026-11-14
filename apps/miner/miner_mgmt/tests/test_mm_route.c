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

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "ctrl_cmds.h"
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "boardconf.h"
#include "cali.h"
#include "ctrl_args.h"
#include "fan_dev.h"
#include "fan_pid.h"
#include "mc_api_bootby.h"
#include "miner_ipc.h"
#include "mm_led.h"
#include "mm_cfgstore.h"
#include "mm_display.h"
#include "mm_ui_glue.h"
#include "mm_telem.h"
#include "osif.h"
#include "wdt_dev.h"
#include "mm_tz.h"
#include "net_ops.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"

#define TST_TZ_DIR  MM_ROUTE_TZ_DIR  /* Per-board scratch zoneinfo directory in the build tree */
#define TST_TZ_LINK MM_ROUTE_TZ_LINK /* Per-board scratch localtime link in the build tree */
#undef MM_TZ_ZONEINFO_DIR
#define MM_TZ_ZONEINFO_DIR TST_TZ_DIR /* Overridden to the scratch zoneinfo directory */
#undef MM_TZ_LOCALTIME_PATH
#define MM_TZ_LOCALTIME_PATH TST_TZ_LINK /* Overridden to the scratch localtime link */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_ACT_MAX 32 /* Capacity of tst_act[], actions */
/* cali_resolve() calls, less one per MSG_MM_PERIPH push */
#define ROUTE_RESOLVES (tst_resolves - tst_periph_n)

#define OP_BY_NAME(e, nm, payload, cap) \
    if (!strcmp(name, nm)) \
        return CTRL_OP_##e;

#define WM_SPAN(lo, hi) (v >= (lo) && v <= (hi)) ||

#define PAYLOAD_ROW(e, nm, payload, cap) \
    do { \
        mc_ctrl_cmd u; \
        memset(&u, 0x01, sizeof u); \
        u.op     = CTRL_OP_##e; \
        int took = ctrl_take_cmd(&u, (int)sizeof u, &got); \
        if (!strcmp(#payload, "text")) \
            CHECK(took == 1 && got.v.text[0] == '?' && got.v.text[CTRL_TEXT_LEN - 2] == '?' && \
                            got.v.text[CTRL_TEXT_LEN - 1] == 0, \
                    "%s carries `text`, so every 0x01 byte scrubs to '?' and the " \
                    "field NUL-clamps", \
                    nm); \
        else \
            CHECK(took == 1 && got.v.text[0] == 0x01 && got.v.text[CTRL_TEXT_LEN - 1] == 0x01, \
                    "%s carries `%s`, a NUMERIC payload, so its bytes cross " \
                    "untouched even at 0x01", \
                    nm, #payload); \
    } while (0);

#define AVAIL_ROW(e, nm, payload, cap) \
    do { \
        if ((cap) != 0 && ctrl_cmd_on_board(CTRL_OP_##e)) \
            break; \
        mc_ctrl_cmd u; \
        memset(&u, 0, sizeof u); \
        u.op        = CTRL_OP_##e; \
        int applied = ctrl_take_cmd(&u, (int)sizeof u, &got) && ctrl_cmd_available(got.op); \
        CHECK(!applied, \
                "...so a well-formed %s cannot come out of the mm take+gate path " \
                "looking like success: the framing is valid, the cap is what " \
                "refuses, and mm answers UNSUP rather than a stub OK for absent " \
                "hardware", \
                nm); \
    } while (0);

#define ASCSET_NAME(e, nm, payload, cap) nm,

osif_status_t  tst_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len);
void           tst_ipc_deinit(osif_ipc_t *h);
static void    tst_fan_fixed(fan_pid_state_t *st, int duty_pct);
static int     tst_wm_set(const cali_wm_store *s, uint8_t mode);
static uint8_t tst_wm_get(const cali_wm_store *s);
static int     tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
            cali_param *out, uint8_t *aged);
static mc_set_result tst_led_apply(uint16_t op, const char *args);
static int           tst_bootby_stage(uint8_t reason, uint32_t sub);
static int           tst_poweroff_requested(void);
static int           tst_display_lcd(int id, int value);
static void          tst_wp_reload(void);
pid_t                tst_fork(void);
int                  tst_system(const char *cmd);
int                  tst_kill(pid_t pid, int sig);
pid_t                tst_waitpid(pid_t pid, int *status, int options);
int                  tst_stat(const char *path, struct stat *st);
static int           tst_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *),
                  void *arg);
static int           tst_clock_nanosleep(clockid_t clk, int flags, const struct timespec *req,
                  struct timespec *rem);

#define osif_ipc_send(h, t, d, l) tst_ipc_send((h), (t), (d), (l)) /* Records ACKs and pushes */
#define osif_ipc_deinit(h)        tst_ipc_deinit(h)                /* Records the freed handle */
#define fan_pid_set_fixed(s, d)   tst_fan_fixed((s), (d))          /* Logs "fan=" */
#define cali_workmode_set(p, m)   tst_wm_set((p), (m))             /* Logs "wm=" */
#define cali_workmode_get(p)      tst_wm_get(p)                    /* Returns tst_wm_mode */
/* tst_cali_resolve(): records its arguments */
#define cali_resolve(p, c, m, l, o, a) tst_cali_resolve((p), (c), (m), (l), (o), (a))
#define mm_led_apply                   tst_led_apply              /* Logs "led=" */
#define mm_bootby_stage(r, s)          tst_bootby_stage((r), (s)) /* Logs "bootby=" */
#define mm_ui_poweroff_requested       tst_poweroff_requested     /* Returns tst_poweroff */
#define mm_display_lcd(i, v)           tst_display_lcd((i), (v))  /* Logs "lcd=" */
#define mm_ui_wallpaper_reload()       tst_wp_reload()            /* Logs "wp-reload" */
#define fork()                         tst_fork()                 /* Logs "FORK!" and exits */
#define system(c)                      tst_system(c)              /* Logs the command kind */
#define kill(p, s)                     tst_kill((p), (s))         /* Logs "KILL!" */
#define waitpid(p, s, o)               tst_waitpid((p), (s), (o)) /* Reaps nothing */
#define stat(p, b)                     tst_stat((p), (b))         /* TZ fixture files only */

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
/* Runs the reboot thread inline */
#define pthread_create(t, a, f, x)  tst_pthread_create((t), (a), (f), (x))
#define clock_nanosleep(c, f, r, m) tst_clock_nanosleep((c), (f), (r), (m)) /* Logs the sleep, s */
#include "mm_miner_tu.h"
#undef clock_nanosleep
#undef pthread_create
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork
#undef mm_ui_poweroff_requested
#undef mm_ui_wallpaper_reload
#undef mm_display_lcd
#undef mm_bootby_stage
#undef mm_led_apply
#undef cali_resolve
#undef cali_workmode_get
#undef cali_workmode_set
#undef fan_pid_set_fixed
#undef osif_ipc_deinit
#undef osif_ipc_send

static char tst_act[TST_ACT_MAX][48]; /* Action log, one formatted tag per stub call */
static int  tst_act_n;                /* Actions logged; may exceed TST_ACT_MAX */
/* ACKs sent, MSG_MM_PERIPH pushes, 1 once an ACK followed a push */
static int           tst_ack_n, tst_periph_n, tst_ack_after_periph;
static mc_set_result tst_ack;          /* Result of the last ACK */
static mc_ctrl_ack   tst_ack_msg;      /* Last ACK, whole */
static int           tst_eintr_n;      /* clock_nanosleep() calls still to fail with EINTR */
static int           tst_reboot_rc;    /* Value mm_system_reboot() returns */
static int           tst_pthread_fail; /* 1: tst_pthread_create() fails */
static uint8_t       tst_wm_mode = 2;  /* Work mode cali_workmode_get() reports */
/* Value cali_workmode_set() returns; 0 also stores the mode */
static int         tst_wm_set_rc;
static const char *tst_seen_ini;       /* Calibration path the last cali_resolve() saw */
static uint16_t    tst_seen_cnt;       /* ASIC count the last cali_resolve() saw */
static uint8_t     tst_seen_mode;      /* Mode the last cali_resolve() saw */
static int8_t      tst_seen_level;     /* Level the last cali_resolve() saw */
static uint16_t    tst_cali_temp = 90; /* Target temperature cali_resolve() returns, degC */
static int         tst_resolves;       /* cali_resolve() calls */
static uint8_t     tst_aged = 1;       /* Aged flag cali_resolve() returns */
static uint16_t    tst_led_op;         /* Op of the last mm_led_apply() */
static int         tst_poweroff;       /* Value mm_ui_poweroff_requested() returns */
static int         tst_duty_x10;       /* Last fan duty set, 0.1 % units */
static int         tst_trace_stop;     /* record the shutdown stop sequence */
static osif_ipc_t *tst_freed[2];       /* IPC handles passed to osif_ipc_deinit() */
static int         tst_freed_n;
static int         tst_send_freed; /* Sends made on a freed handle */
/* Commands the nano3s routes */
static const char *const FITTED_NANO3S[] = {
    "workmode",
    "ledmode",
    "ledset",
    "nightlamp",
    "filter-clean",
    "lcd",
    "softon",
    "softoff",
    "settz",
    "wp-reload",
};
/* Commands the x3 routes */
static const char *const FITTED_X3[] = {
    "workmode",
    "settz",
};
static const char *const *g_fitted; /* Command list expected on this board; NULL = none */
static int                g_fitted_n;

static int mock_net_wait(unsigned t);
static int mock_net_dhcp(void);
static int mock_net_static(const char *ip, const char *m, const char *g, const char *d);
static int mock_net_addr(char *b, unsigned c);
/* Network ops hal_net() returns; every op is a stub */
static const hal_net_ops_t mock_net_ops = {
    .iface      = MM_NET_IFACE,
    .wait_ready = mock_net_wait,
    .dhcp       = mock_net_dhcp,
    .set_static = mock_net_static,
    .get_addr   = mock_net_addr,
};

static void act(const char *fmt, ...)
{
    va_list ap;
    if (tst_act_n >= TST_ACT_MAX) {
        tst_act_n++;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(tst_act[tst_act_n++], sizeof tst_act[0], fmt, ap);
    va_end(ap);
}
static const char *acts(void)
{
    static char b[512];
    size_t      n = 0;
    b[0]          = '\0';
    for (int i = 0; i < tst_act_n && i < TST_ACT_MAX && n + 1 < sizeof b; i++)
        n += (size_t)snprintf(b + n, sizeof b - n, "%s%s", n ? "," : "", tst_act[i]);
    return b[0] ? b : "(nothing)";
}
static int has_act(const char *tag)
{
    for (int i = 0; i < tst_act_n && i < TST_ACT_MAX; i++)
        if (!strcmp(tst_act[i], tag))
            return 1;
    return 0;
}

void tst_ipc_deinit(osif_ipc_t *h)
{
    if (h && tst_freed_n < (int)(sizeof tst_freed / sizeof tst_freed[0]))
        tst_freed[tst_freed_n++] = h;
}

osif_status_t tst_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)len;
    for (int i = 0; h && i < tst_freed_n; i++)
        if (tst_freed[i] == h)
            tst_send_freed++;
    if (type == MSG_CTRL_ACK) {
        memcpy(&tst_ack_msg, data, sizeof tst_ack_msg);
        tst_ack = (mc_set_result)tst_ack_msg.result;
        tst_ack_n++;
        if (tst_periph_n > 0)
            tst_ack_after_periph = 1;
        act("ack=%d", (int)tst_ack);
    } else if (type == MSG_MM_PERIPH) {
        tst_periph_n++;
        act("periph");
    }
    return OSIF_OK;
}

static int tst_clock_nanosleep(clockid_t clk, int flags, const struct timespec *req,
        struct timespec *rem)
{
    uint64_t at = (uint64_t)req->tv_sec * 1000u + (uint64_t)req->tv_nsec / 1000000u;

    (void)rem;
    if (clk != CLOCK_MONOTONIC || flags != TIMER_ABSTIME) {
        act("sleep-relative");
        return EINVAL;
    }
    if (tst_eintr_n > 0) {
        tst_eintr_n--;
        act("eintr");
        return EINTR;
    }
    act("sleep=%u", (unsigned)((at - osif_now_ms()) / 1000u));
    return 0;
}

int mm_cfgstore_flush(void)
{
    act("cfgflush");
    return 0;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    char    b[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (lvl == OSIF_LOG_WARN && strstr(b, "superseded"))
        act("warn-superseded");
}

static int act_index(const char *tag)
{
    for (int i = tst_act_n - 1; i >= 0; i--)
        if (i < TST_ACT_MAX && !strcmp(tst_act[i], tag))
            return i;
    return -1;
}

int mm_system_reboot(uint8_t reason, uint32_t sub)
{
    mm_cfg_flush();
    act("reboot=%u,%u", (unsigned)reason, (unsigned)sub);
    return tst_reboot_rc;
}

static void *tst_idle_thread(void *arg)
{
    return arg;
}

static int tst_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    if (tst_pthread_fail)
        return -1;
    if (fn == mm_reboot_thread) {
        fn(arg);
        return pthread_create(t, a, tst_idle_thread, NULL);
    }
    return pthread_create(t, a, fn, arg);
}

static void tst_fan_fixed(fan_pid_state_t *st, int duty_pct)
{
    act("fan=%d", duty_pct);
    fan_pid_set_fixed(st, duty_pct);
}
static int tst_wm_set(const cali_wm_store *s, uint8_t mode)
{
    (void)s;
    act("wm=%d", (int)mode);
    if (tst_wm_set_rc == 0)
        tst_wm_mode = mode;
    return tst_wm_set_rc;
}
static uint8_t tst_wm_get(const cali_wm_store *s)
{
    (void)s;
    return tst_wm_mode;
}

static int tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
        cali_param *out, uint8_t *aged)
{
    tst_resolves++;
    tst_seen_ini   = path;
    tst_seen_cnt   = asic_cnt;
    tst_seen_mode  = mode;
    tst_seen_level = level;
    if (out) {
        memset(out, 0, sizeof *out);
        out->temp     = tst_cali_temp;
        out->max_pout = 62;
    }
    if (aged)
        *aged = tst_aged;
    act("cali=%d", (int)mode);
    return 0;
}
static mc_set_result tst_led_apply(uint16_t op, const char *args)
{
    (void)args;
    tst_led_op = op;
    act("led=%s", ctrl_cmd_name(op));
    return MC_SET_INFO;
}
static int tst_bootby_stage(uint8_t reason, uint32_t sub)
{
    (void)sub;
    act("bootby=%u", (unsigned)reason);
    return 0;
}
static int tst_poweroff_requested(void)
{
    return tst_poweroff;
}
static int tst_display_lcd(int id, int value)
{
    act("lcd=%d:%d", id, value);
    return 0;
}
static void tst_wp_reload(void)
{
    act("wp-reload");
}

int eeprom_init(void)
{
    return 0;
}
int eeprom_read(uint8_t addr, uint8_t *pbuf, uint8_t data_len, uint8_t miner_id)
{
    (void)addr;
    (void)miner_id;
    memset(pbuf, 0, data_len);
    return 0;
}

static void tz_fixture_clean(void)
{
    unlink(TST_TZ_LINK);
    unlink(TST_TZ_LINK ".new");
}

pid_t tst_fork(void)
{
    act("FORK!");
    tz_fixture_clean();
    _exit(2);
}
static int mock_net_wait(unsigned t)
{
    (void)t;
    return 0;
}
static int mock_net_dhcp(void)
{
    return tst_system("udhcpc -i " MM_NET_IFACE " -b");
}
static int mock_net_static(const char *ip, const char *m, const char *g, const char *d)
{
    (void)ip;
    (void)m;
    (void)g;
    (void)d;
    return tst_system("ifconfig " MM_NET_IFACE);
}
static int mock_net_addr(char *b, unsigned c)
{
    if (b && c)
        b[0] = 0;
    return -1;
}
const hal_net_ops_t *hal_net(void)
{
    return &mock_net_ops;
}

int tst_system(const char *cmd)
{
    if (strstr(cmd, "udhcpc -i"))
        act("dhcp-up");
    else if (strstr(cmd, "pidof udhcpc"))
        act("dhcp-stop");
    else if (strstr(cmd, "ifconfig"))
        act("ifconfig");
    else if (strstr(cmd, "route"))
        act("route");
    else if (strstr(cmd, "ping"))
        act("ping");
    else
        act("sh?");
    return 0;
}
int tst_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    act("KILL!");
    return 0;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)pid;
    (void)options;
    if (status)
        *status = 0;
    return 0;
}
int tst_stat(const char *path, struct stat *st)
{
    if (st)
        memset(st, 0, sizeof *st);
    const char *z = strstr(path, TST_TZ_DIR "/");
    if (!z)
        return -1;
    act("tz=%s", z + sizeof(TST_TZ_DIR));
    if (st)
        st->st_mode = S_IFREG;
    return 0;
}

int fan_dev_init(void)
{
    return 0;
}
void fan_dev_set_duty_x10(int d)
{
    tst_duty_x10 = d;
    act("duty=%d", d);
}
int fan_dev_last_duty_x10(void)
{
    return tst_duty_x10;
}
int fan_dev_get_duty_pct(void)
{
    return tst_duty_x10 / 10;
}
int wdt_dev_init(void)
{
    return 0;
}
int wdt_dev_bootstatus(void)
{
    return 0;
}
int wdt_dev_timeout_s(void)
{
    return 2 * WDT_TIMEOUT_S;
}
int wdt_dev_deadline_s(void)
{
    return WDT_TIMEOUT_S;
}
int wdt_dev_feed_period_ms(void)
{
    return WDT_FEED_PERIOD_MS_MAX;
}
void wdt_dev_disarm(void)
{
}
void wdt_dev_feed(void)
{
}
void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
}

int tst_children_stop(const pid_t *pids, int n, unsigned term_ms, unsigned kill_ms)
{
    (void)pids;
    (void)n;
    (void)term_ms;
    (void)kill_ms;
    act("children-stop");
    return 0;
}
void mm_vf_stop_request(void)
{
    if (tst_trace_stop)
        act("vf-stop-request");
}
void mm_vf_stop(void)
{
    if (tst_trace_stop)
        act("vf-stop");
}
int board_power_rail_off(void)
{
    if (tst_trace_stop)
        act("rail-off");
    return HAL_PSU_RAIL_OK;
}

static int act_first(const char *tag)
{
    for (int i = 0; i < tst_act_n && i < TST_ACT_MAX; i++)
        if (!strcmp(tst_act[i], tag))
            return i;
    return -1;
}

static int save_before_children_stop(void)
{
    int save = act_first("cfgflush");
    return save >= 0 && save < act_first("children-stop");
}

static void route_reset(void);

static int shutdown_writes_sys(int ota, int poweroff)
{
    char tag[16];
    snprintf(tag, sizeof tag, "bootby=%u", (unsigned)MC_BOOTBY_SYS);
    route_reset();
    g_sig = SIGTERM;
    atomic_store(&g_ota_reboot, ota);
    tst_poweroff   = poweroff;
    tst_trace_stop = 1;
    mm_shutdown(-1, -1);
    tst_trace_stop = 0;
    g_sig          = 0;
    atomic_store(&g_ota_reboot, 0);
    tst_poweroff = 0;
    return has_act(tag);
}

static void route_reset(void)
{
    tst_act_n            = 0;
    tst_ack_n            = 0;
    tst_periph_n         = 0;
    tst_ack_after_periph = 0;
    tst_ack              = (mc_set_result)-1;
    tst_resolves         = 0;
    tst_led_op           = CTRL_OP_NONE;
}

static uint16_t tst_op_by_name(const char *name)
{
    CTRL_CMD_LIST(OP_BY_NAME)
    return CTRL_OP_NONE;
}

static int route(const char *subcmd, const char *args)
{
    mc_ctrl_cmd cmd;
    uint16_t    op = tst_op_by_name(subcmd);
    route_reset();
    if (ctrl_parse_cmd(op, args, &cmd) != 0)
        return -2;
    mm_ctrl_cb(MSG_CTRL_CMD, &cmd, (int)sizeof cmd);
    return tst_ack_n ? (int)tst_ack : -1;
}

static int route_op(uint16_t op)
{
    mc_ctrl_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.op = op;
    route_reset();
    mm_ctrl_cb(MSG_CTRL_CMD, &cmd, (int)sizeof cmd);
    return tst_ack_n ? (int)tst_ack : -1;
}

static void reset_state(void)
{
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    memset(&g_soft, 0, sizeof g_soft);
    g_api_req           = 0;
    g_fan_target_temp   = FAN_TARGET_TEMP;
    g_fan_target_manual = 0;
    tst_wm_set_rc       = 0;
    tst_wm_mode         = 2;
    g_power_modes       = (1u << CALI_MODE_MAX) - 1u;
    tst_cali_temp       = 90;
    tst_aged            = 1;
    tst_duty_x10        = 0;
    tst_pthread_fail    = 0;
}

static int fitted_here(const char *nm)
{
    for (int i = 0; i < g_fitted_n; i++)
        if (!strcmp(nm, g_fitted[i]))
            return 1;
    return 0;
}

static int wm_allowed(int v)
{
    return BOARD_WORKMODE_RANGES(WM_SPAN) 0;
}

static int cb_sent(uint32_t type, const mc_ctrl_cmd *cmd, int len)
{
    route_reset();
    mm_ctrl_cb(type, cmd, len);
    return tst_ack_n + tst_periph_n;
}

static void test_take_cmd(void)
{
    mc_ctrl_cmd cmd, got;

    memset(&got, 0, sizeof got);
    CHECK(ctrl_parse_cmd(CTRL_OP_REBOOT, "3,0,80", &cmd) == 0 &&
                    ctrl_take_cmd(&cmd, (int)sizeof cmd, &got) == 1,
            "mm accepts a well-formed MSG_CTRL_CMD");
    CHECK(got.op == CTRL_OP_REBOOT && got.v.reboot.delay_s == 80,
            "op and numeric payload survive the round trip (%u)", got.v.reboot.delay_s);
    {
        static uint8_t big[sizeof(mc_ctrl_cmd) + 4];
        memcpy(big, &cmd, sizeof cmd);
        CHECK(ctrl_take_cmd(big, (int)sizeof big, &got) == 0 &&
                        ctrl_take_cmd(&cmd, (int)sizeof cmd - 1, &got) == 0,
                "mm drops a MSG_CTRL_CMD longer or shorter than mc_ctrl_cmd");
    }

    CHECK(ctrl_parse_cmd(CTRL_OP_REBOOT, "10,0", &cmd) == 0 &&
                    ctrl_take_cmd(&cmd, (int)sizeof cmd, &got) == 1 && got.v.reboot.reason == 10,
            "a numeric payload is not scrubbed even when its bytes are < 0x20");

    CHECK(ctrl_parse_cmd(CTRL_OP_SETTZ, "Asia/Shanghai", &cmd) == 0 &&
                    ctrl_take_cmd(&cmd, (int)sizeof cmd, &got) == 1 &&
                    !strcmp(got.v.text, "Asia/Shanghai"),
            "a text payload survives intact");

    cmd.v.text[4] = '\n';
    cmd.v.text[5] = 'F';
    CHECK(ctrl_take_cmd(&cmd, (int)sizeof cmd, &got) == 1 && got.v.text[4] == '?' &&
                    got.v.text[5] == 'F',
            "control chars in text are scrubbed, not passed to a log line raw");

    memset(&cmd, 'A', sizeof cmd);
    cmd.op = CTRL_OP_SETTZ;
    CHECK(ctrl_take_cmd(&cmd, (int)sizeof cmd, &got) == 1 &&
                    strlen(got.v.text) == sizeof got.v.text - 1,
            "an unterminated text field is NUL-clamped");

    CHECK(ctrl_parse_cmd(CTRL_OP_WP_RELOAD, "", &cmd) == 0 &&
                    ctrl_take_cmd(&cmd, (int)sizeof cmd, &got) == 1 && got.op == CTRL_OP_WP_RELOAD,
            "wp-reload round-trips core->mm intact");

    CHECK(cb_sent(MSG_CTRL_ACK, &cmd, (int)sizeof cmd) == 0, "wrong message type dropped");
    CHECK(cb_sent(MSG_ASIC_JOB, &cmd, (int)sizeof cmd) == 0,
            "datapath type on the control channel dropped");
    CHECK(cb_sent(5u, &cmd, (int)sizeof cmd) == 0,
            "retired id 5 (a subcmd+args text form) is refused, so a stale "
            "core cannot be read through the current struct");
    CHECK(cb_sent(11u, &cmd, (int)sizeof cmd) == 0,
            "id 11 (MSG_MM_STATUS, mm -> core only) is refused on the control channel");
    CHECK(ctrl_take_cmd(NULL, (int)sizeof cmd, &got) == 0, "NULL payload dropped");
    CHECK(ctrl_take_cmd(&cmd, (int)sizeof cmd - 1, &got) == 0,
            "short packet dropped (no partial apply)");
    CHECK(ctrl_take_cmd(&cmd, 0, &got) == 0, "zero-length dropped");

    CTRL_CMD_LIST(PAYLOAD_ROW)

    CTRL_CMD_LIST(AVAIL_ROW)
}

static int soft_cmd(uint16_t op, const char *args, uint32_t seq)
{
    mc_ctrl_cmd cmd;
    if (ctrl_parse_cmd(op, args, &cmd) != 0)
        return -2;
    cmd.seq = seq;
    route_reset();
    memset(&tst_ack_msg, 0, sizeof tst_ack_msg);
    mm_ctrl_cb(MSG_CTRL_CMD, &cmd, (int)sizeof cmd);
    return tst_ack_n ? (int)tst_ack : -1;
}

static void soft_state_reset(void)
{
    reset_state();
    memset(&g_softonoff, 0, sizeof g_softonoff);
    g_fault_off   = 0;
    g_softoff_rsn = MC_SOFTOFF_NONE;
}

static void test_soft_refusals(void)
{
    char    past[32], soon[32], edge[32];
    int64_t now = (int64_t)time(NULL);

    if (!ctrl_cmd_available(CTRL_OP_SOFTON) || !ctrl_cmd_available(CTRL_OP_SOFTOFF))
        return;

    snprintf(past, sizeof past, "1,%lld", (long long)(now - 3600));
    snprintf(edge, sizeof edge, "%lld", (long long)(now - SOFT_SCHED_WINDOW_S + 1));
    snprintf(soon, sizeof soon, "%lld", (long long)(now + 3600));

    soft_state_reset();
    CHECK(soft_cmd(CTRL_OP_SOFTOFF, past, 41) == MC_SET_ERR_ARG &&
                    tst_ack_msg.why == MC_CTRL_WHY_PAST && tst_ack_msg.seq == 41,
            "a scheduled softoff an hour in the past is refused with the reason, "
            "not stored to never fire (why %u seq %u)",
            tst_ack_msg.why, tst_ack_msg.seq);
    CHECK(g_soft.off_valid == 0, "...and nothing is armed");
    CHECK(tst_ack_msg.arg >= now && tst_ack_msg.arg <= now + 5,
            "...the ACK carries mm's clock, so the caller sees what \"past\" was measured against");

    soft_state_reset();
    CHECK(soft_cmd(CTRL_OP_SOFTON, edge, 42) == MC_SET_OK && g_soft.on_valid == 1,
            "a time inside the %d s firing window is still armed: it fires next tick",
            SOFT_SCHED_WINDOW_S);
    CHECK(tst_ack_msg.why == MC_CTRL_WHY_NONE && tst_ack_msg.seq == 42,
            "...the ACK echoes the command's seq with no reason");

    soft_state_reset();
    CHECK(soft_cmd(CTRL_OP_SOFTOFF, soon, 43) == MC_SET_OK && g_soft.off_valid == 1,
            "a future softoff is armed");
    CHECK(soft_cmd(CTRL_OP_SOFTOFF, "0,0", 44) == MC_SET_OK && g_soft.off_valid == 0,
            "a cancel carries no time and is never \"in the past\"");

    soft_state_reset();
    g_softonoff.thermal_until = 180000u;
    CHECK(soft_cmd(CTRL_OP_SOFTON, "now", 45) == MC_SET_ERR_STATE &&
                    tst_ack_msg.why == MC_CTRL_WHY_COOLDOWN && tst_ack_msg.arg == 180,
            "softon during the over-temperature cool-down is refused with the seconds "
            "left (why %u arg %lld)",
            tst_ack_msg.why, (long long)tst_ack_msg.arg);
    CHECK(g_api_req == 0, "...and no request is queued for the supervisor");
    CHECK(soft_cmd(CTRL_OP_SOFTOFF, "now", 46) == MC_SET_OK &&
                    tst_ack_msg.why == MC_CTRL_WHY_NONE && g_api_req == -1,
            "softoff during the cool-down is accepted: the supervisor latches it");
    g_api_req = 0;
    CHECK(soft_cmd(CTRL_OP_SOFTOFF, soon, 47) == MC_SET_OK && g_soft.off_valid == 1,
            "a scheduled softoff during the cool-down is still armed: it is due later");

    soft_state_reset();
    g_fault_off   = 1;
    g_softoff_rsn = MC_SOFTOFF_FAN_ERR;
    CHECK(soft_cmd(CTRL_OP_SOFTON, "now", 48) == MC_SET_ERR_STATE &&
                    tst_ack_msg.why == MC_CTRL_WHY_FAULT && tst_ack_msg.arg == MC_SOFTOFF_FAN_ERR,
            "a lone softon during a fault stop is refused with the fault reason "
            "(why %u arg %lld)",
            tst_ack_msg.why, (long long)tst_ack_msg.arg);
    CHECK(g_api_req == 0, "...and the fault stays latched, unchanged");
    CHECK(soft_cmd(CTRL_OP_SOFTOFF, "now", 49) == MC_SET_OK && g_api_req == -1,
            "softoff during a fault stop is accepted: the first half of the release");
    g_api_req               = 0;
    g_softonoff.api_softoff = 1;
    CHECK(soft_cmd(CTRL_OP_SOFTON, "now", 50) == MC_SET_OK && g_api_req == 1,
            "softon after that softoff is accepted: it releases the fault");

    soft_state_reset();
    CHECK(soft_cmd(CTRL_OP_SOFTON, "now", 51) == MC_SET_OK && g_api_req == 1,
            "softon while mining stays OK, as before");
    soft_state_reset();
}

int main(void)
{
    atexit(tz_fixture_clean);

    printf("descriptor %s: %d chips per hashboard, %d board(s), fan target band "
           "[%d..%d] degC, boot target %d degC\n",
            BOARDCONF_SECTION, MAX_ASIC_ON_SINGLE_HASH, (int)HBOARD_COUNT, FAN_PID_TEMP_MIN,
            FAN_PID_TEMP_MAX, FAN_TARGET_TEMP);

    if (!strcmp(BOARDCONF_SECTION, "NANO3S")) {
        g_fitted   = FITTED_NANO3S;
        g_fitted_n = 10;
    } else if (!strcmp(BOARDCONF_SECTION, "X3")) {
        g_fitted   = FITTED_X3;
        g_fitted_n = 2;
    } else {
        printf("FAIL: no expected command set for descriptor [%s] — write it by "
               "hand off that board's own BOARD_CTRL_OPS and CAP_*\n",
                BOARDCONF_SECTION);
        return 1;
    }

    test_take_cmd();
    test_soft_refusals();

    {
        mc_ctrl_cmd c;
        memset(&c, 0, sizeof c);
        c.op              = CTRL_OP_REBOOT;
        c.v.reboot.reason = MC_BOOTBY_API;
        reset_state();
        route_reset();
        tst_pthread_fail = 1;
        int rc           = route_reboot(CTRL_OP_REBOOT, &c);
        tst_pthread_fail = 0;
        CHECK(rc == MC_SET_ERR_HW && !has_act("reboot=3,0"),
                "a reboot whose thread cannot be spawned answers MC_SET_ERR_HW "
                "(%d), not OK: no reboot follows it (acts: %s)",
                rc, acts());

        char slept[16];
        snprintf(slept, sizeof slept, "sleep=%d", CTRL_REBOOT_DELAY_SEC);
        route_reset();
        c.v.reboot.sub = 7;
        tst_reboot_rc  = -1;
        rc             = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && has_act(slept) && has_act("reboot=3,7"),
                "the reboot thread waits %d s and hands the requested reason and "
                "subreason to mm_system_reboot (rc %d, acts: %s)",
                CTRL_REBOOT_DELAY_SEC, rc, acts());
        route_reset();
        rc            = route_reboot(CTRL_OP_REBOOT, &c);
        tst_reboot_rc = 0;
        CHECK(rc == MC_SET_OK && has_act("reboot=3,7"),
                "...and a reboot that fails clears the pending latch, so the next "
                "request reboots again instead of being swallowed (rc %d, acts: %s)",
                rc, acts());
    }

    {
        mc_ctrl_cmd c;
        char        grace[16];
        uint16_t    old_gen;
        int         rc;

        memset(&c, 0, sizeof c);
        c.op              = CTRL_OP_REBOOT;
        c.v.reboot.reason = MC_BOOTBY_API;
        snprintf(grace, sizeof grace, "sleep=%d", CTRL_REBOOT_DELAY_SEC);
        reset_state();

        route_reset();
        g_reboot_at        = 0;
        c.v.reboot.delay_s = 30;
        rc                 = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && has_act("sleep=30") && !has_act(grace) && has_act("reboot=3,0"),
                "a reboot with delay 30 waits 30 s, not the ack grace, then "
                "reboots with reason API (rc %d, acts: %s)",
                rc, acts());

        route_reset();
        g_reboot_at        = 0;
        c.v.reboot.delay_s = 0;
        rc                 = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && has_act(grace) && has_act("reboot=3,0"),
                "a reboot with delay 0 waits only the %d s ack grace (rc %d, "
                "acts: %s)",
                CTRL_REBOOT_DELAY_SEC, rc, acts());

        route_reset();
        g_reboot_at        = 0;
        c.v.reboot.delay_s = CTRL_REBOOT_DELAY_MAX + 1;
        rc                 = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_ERR_ARG && !has_act("reboot=3,0"),
                "mm refuses a delay over %d s itself (rc %d, acts: %s)", CTRL_REBOOT_DELAY_MAX, rc,
                acts());

        route_reset();
        g_reboot_at        = osif_now_ms() + 60000u;
        old_gen            = g_reboot_gen;
        c.v.reboot.delay_s = 100;
        rc                 = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && !has_act("sleep=100") && !has_act("reboot=3,0"),
                "a request due after the pending reboot arms nothing: the earlier "
                "deadline stands (rc %d, acts: %s)",
                rc, acts());

        route_reset();
        c.v.reboot.delay_s = 5;
        rc                 = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && has_act("sleep=5") && has_act("reboot=3,0"),
                "a request due before the pending reboot takes over and fires "
                "first (rc %d, acts: %s)",
                rc, acts());

        route_reset();
        (void)mm_reboot_thread((void *)(uintptr_t)old_gen);
        CHECK(!has_act("reboot=3,0"),
                "the superseded request wakes to a newer arm and does not reboot "
                "(acts: %s)",
                acts());
        g_reboot_at = 0;

        route_reset();
        c.v.reboot.delay_s = 0;
        tst_eintr_n        = 2;
        rc                 = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && tst_eintr_n == 0 && act_index("eintr") >= 0 &&
                        act_index(grace) > act_index("eintr") &&
                        act_index("reboot=3,0") > act_index(grace),
                "a signal that interrupts the delay does not bring the reboot "
                "forward: the thread sleeps again to the same absolute deadline "
                "and reboots only after it (acts: %s)",
                acts());
        g_reboot_at = 0;

        route_reset();
        rc = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && act_index("cfgflush") > act_index(grace) &&
                        act_index("reboot=3,0") > act_index("cfgflush"),
                "settings changed during the delay are saved after it, right "
                "before mm_system_reboot (acts: %s)",
                acts());
        g_reboot_at = 0;

        route_reset();
        tst_reboot_rc = -1;
        rc            = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && has_act("reboot=3,0") && !has_act("warn-superseded") &&
                        g_reboot_at == 0,
                "a failed reboot that replaced nothing drops only itself and "
                "warns about no superseded request (acts: %s)",
                acts());

        route_reset();
        g_reboot_at        = osif_now_ms() + 60000u;
        c.v.reboot.delay_s = 5;
        rc                 = route_reboot(CTRL_OP_REBOOT, &c);
        tst_reboot_rc      = 0;
        CHECK(rc == MC_SET_OK && has_act("reboot=3,0") && has_act("warn-superseded") &&
                        g_reboot_at == 0,
                "a failed reboot that took over a later pending one says that "
                "request is dropped too (acts: %s)",
                acts());
        g_reboot_at = 0;
    }

    {
        static const struct {
            const char *sub, *args, *tag;
            int         want;
        } tbl[] = {
            { "workmode", "0", "wm=0", MC_SET_OK },
            { "ledmode", "1", "led=ledmode", MC_SET_INFO },
            { "ledset", "2-100-100-0-0-255", "led=ledset", MC_SET_INFO },
            { "nightlamp", "1-180", "led=nightlamp", MC_SET_INFO },
            { "settz", "Asia/Shanghai", "tz=Asia/Shanghai", MC_SET_OK },
            { "softon", "now", "", MC_SET_OK },
            { "softoff", "now", "", MC_SET_OK },
            { "filter-clean", "1", "", MC_SET_OK },
            { "lcd", "1:1", "lcd=1:1", MC_SET_OK },
            { "wp-reload", "png", "wp-reload", MC_SET_OK },
            { "reboot", "3,0", "reboot=3,0", MC_SET_OK },
        };
        for (unsigned i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
            if (!ctrl_cmd_available(tst_op_by_name(tbl[i].sub)))
                continue;
            reset_state();
            if (!strcmp(tbl[i].sub, "filter-clean"))
                g_filtermesh_time = 777;
            int rc     = route(tbl[i].sub, tbl[i].args);
            int tagged = !tbl[i].tag[0] ? 1 : has_act(tbl[i].tag);
            if (!strcmp(tbl[i].sub, "softon"))
                tagged = (g_api_req == 1);
            if (!strcmp(tbl[i].sub, "softoff"))
                tagged = (g_api_req == -1);
            if (!strcmp(tbl[i].sub, "filter-clean"))
                tagged = (g_filtermesh_time == 0);
            CHECK(rc == tbl[i].want,
                    "`%s %s` answers %d — the reply code its own handler returns "
                    "(got %d)",
                    tbl[i].sub, tbl[i].args, tbl[i].want, rc);
            CHECK(tagged,
                    "`%s %s` leaves behind the side effect only ITS handler "
                    "produces, so it reached that handler rather than a sibling "
                    "arm or the fall-through (acts: %s)",
                    tbl[i].sub, tbl[i].args, acts());
        }

        const unsigned nrow   = sizeof tbl / sizeof tbl[0];
        unsigned       nroute = 0;
        CHECK(g_ctrl_route[CTRL_OP_NONE] == NULL, "CTRL_OP_NONE has no route");
        CHECK(g_ctrl_route[CTRL_OP_RESERVED_5] == NULL, "CTRL_OP_RESERVED_5 has no route");
        for (unsigned op = 1; op < CTRL_OP_COUNT; op++) {
            if (op == CTRL_OP_RESERVED_5)
                continue;
            CHECK(g_ctrl_route[op] != NULL, "%s has a route in the op-indexed table",
                    ctrl_cmd_name(op));
            if (g_ctrl_route[op])
                nroute++;
        }
        char missing[192] = "";
        for (unsigned op = 1; op < CTRL_OP_COUNT; op++) {
            if (!g_ctrl_route[op])
                continue;
            const char *nm   = ctrl_cmd_name(op);
            int         seen = 0;
            for (unsigned i = 0; i < nrow && !seen; i++)
                if (!strcmp(tbl[i].sub, nm))
                    seen = 1;
            if (!seen) {
                strncat(missing, " ", sizeof missing - strlen(missing) - 1);
                strncat(missing, nm, sizeof missing - strlen(missing) - 1);
            }
        }
        char stray[192] = "";
        for (unsigned i = 0; i < nrow; i++) {
            int real = 0;
            for (unsigned op = 1; op < CTRL_OP_COUNT && !real; op++)
                if (g_ctrl_route[op] && !strcmp(tbl[i].sub, ctrl_cmd_name(op)))
                    real = 1;
            if (!real) {
                strncat(stray, " ", sizeof stray - strlen(stray) - 1);
                strncat(stray, tbl[i].sub, sizeof stray - strlen(stray) - 1);
            }
        }
        CHECK(nrow == nroute,
                "the rows above ACCOUNT FOR the whole shared command table by "
                "count: %u route(s) vs %u row(s)",
                nroute, nrow);
        CHECK(!missing[0],
                "every route in the shared command table has a row above \u2014 an "
                "untested route is one the app can send that nothing here "
                "exercises; untested routes:%s",
                missing[0] ? missing : " none");
        CHECK(!stray[0],
                "every row above names a route that really exists \u2014 a stray row "
                "is one testing a command the table no longer dispatches; rows "
                "naming no route:%s",
                stray[0] ? stray : " none");

        for (unsigned op = 1; op < CTRL_OP_COUNT; op++)
            if (g_ctrl_route[op])
                CHECK(ctrl_cmd_name(op)[0] != '?',
                        "route %u's op resolves to a name in the shared table", op);

        {
            static const char *const ascset[] = { CTRL_CMD_ASCSET(ASCSET_NAME) };
            static const uint16_t    hidden[] = { CTRL_OP_SETTZ, CTRL_OP_WP_RELOAD };
            for (unsigned h = 0; h < sizeof hidden / sizeof hidden[0]; h++) {
                const char *nm     = ctrl_cmd_name(hidden[h]);
                int         listed = 0;
                for (unsigned i = 0; i < sizeof ascset / sizeof ascset[0]; i++)
                    listed |= !strcmp(ascset[i], nm);
                CHECK(!listed,
                        "%s is not a CTRL_CMD_ASCSET row, so core's ascset loop "
                        "cannot see it and `ascset 0,%s` stays an unknown option "
                        "on the unauthenticated :4028 port",
                        nm, nm);
            }
        }

        for (uint16_t op = 1; op < CTRL_OP_COUNT; op++) {
            if (!g_ctrl_route[op] || ctrl_cmd_available(op))
                continue;
            reset_state();
            int rc   = route_op(op);
            int side = 0;
            for (int k = 0; k < tst_act_n && k < TST_ACT_MAX; k++)
                if (strncmp(tst_act[k], "ack", 3) && strcmp(tst_act[k], "periph") &&
                        strncmp(tst_act[k], "cali", 4))
                    side = 1;
            CHECK(rc == MC_SET_ERR_UNSUP,
                    "%s is not available on this board, so it answers UNSUP "
                    "(rc %d)",
                    ctrl_cmd_name(op), rc);
            CHECK(!side,
                    "%s is not available on this board, so its handler is never "
                    "entered — no actuation of hardware the board does not have "
                    "(%d act(s): %s)",
                    ctrl_cmd_name(op), tst_act_n, acts());
        }
    }

    {
        mc_ctrl_cmd c;
        memset(&c, 0, sizeof c);
        c.op              = CTRL_OP_REBOOT;
        c.v.reboot.reason = MC_BOOTBY_API;
        reset_state();
        (void)route_reboot(CTRL_OP_REBOOT, &c);
        route_reset();
        int rc = route_reboot(CTRL_OP_REBOOT, &c);
        CHECK(rc == MC_SET_OK && !has_act("reboot=3,0"),
                "a reboot requested while one is already pending answers OK and "
                "issues no second reboot (rc %d, acts: %s)",
                rc, acts());
        char sys[16];
        snprintf(sys, sizeof sys, "bootby=%u", (unsigned)MC_BOOTBY_SYS);
        CHECK(shutdown_writes_sys(0, 0) && act_index(sys) < act_first("cfgflush") &&
                        save_before_children_stop(),
                "a SIGTERM shutdown stages MC_BOOTBY_SYS and saves before it stops the "
                "children (acts: %s)",
                acts());
        CHECK(act_index("cfgflush") > act_index("children-stop"),
                "a SIGTERM shutdown saves again after the children are stopped (acts: %s)", acts());
        CHECK(act_first("vf-stop-request") >= 0 &&
                        act_first("vf-stop-request") < act_first("children-stop"),
                "a SIGTERM shutdown asks the V/F thread to stop before it stops the "
                "children (acts: %s)",
                acts());
        CHECK(act_first("vf-stop") > act_first("children-stop") &&
                        act_first("vf-stop") < act_first("rail-off"),
                "...and joins the V/F thread after the children stop and before the "
                "rail is cut (acts: %s)",
                acts());
        CHECK(!shutdown_writes_sys(1, 0) && save_before_children_stop(),
                "a SIGTERM shutdown during an OTA reboot keeps the upgrade reason "
                "(acts: %s)",
                acts());
        CHECK(!shutdown_writes_sys(0, 1) && save_before_children_stop(),
                "a SIGTERM shutdown after a power-button power-off stages no reason "
                "(acts: %s)",
                acts());
    }

    {
        static int tag_core, tag_asic;
        g_ipc_core_mm  = (osif_ipc_t *)&tag_core;
        g_ipc_asic_mm  = (osif_ipc_t *)&tag_asic;
        tst_freed_n    = 0;
        tst_send_freed = 0;
        mm_shutdown(-1, -1);
        mm_status_push();
        mm_periph_push();
        CHECK(tst_freed_n == 2 && tst_freed[0] == (osif_ipc_t *)&tag_core &&
                        tst_freed[1] == (osif_ipc_t *)&tag_asic,
                "shutdown frees the ctrl channel, then the asic-ctrl channel (freed %d)",
                tst_freed_n);
        CHECK(!g_ipc_core_mm && !g_ipc_asic_mm && tst_send_freed == 0,
                "after shutdown no send reaches a freed channel handle (core %p, asic %p, "
                "sends on a freed handle %d)",
                (void *)g_ipc_core_mm, (void *)g_ipc_asic_mm, tst_send_freed);
        tst_freed_n = 0;
    }

    if (fitted_here("workmode")) {
        reset_state();
        route("workmode", "0");
        CHECK(ROUTE_RESOLVES == 2,
                "the workmode route resolves twice, the aging gate and its fan "
                "target (%d call(s)); the nano3s mode ceiling is the cached "
                "admission, not a resolve",
                ROUTE_RESOLVES);
        CHECK(tst_seen_cnt == MAX_ASIC_ON_SINGLE_HASH,
                "the workmode route asks cali_resolve for THIS descriptor's chip "
                "population, %d and not the other board's (got %u) — a wrong "
                "count falls through to this product's own row-0 mode 0",
                MAX_ASIC_ON_SINGLE_HASH, tst_seen_cnt);
        CHECK(tst_seen_mode == 0,
                "the workmode route resolves at the mode JUST PERSISTED, 0 (got "
                "%u) — not the boot-time one",
                tst_seen_mode);
        CHECK(tst_seen_level == 0,
                "the workmode route resolves at level 0 (got %d), the only level "
                "this product has",
                (int)tst_seen_level);
        CHECK(tst_seen_ini && !strcmp(tst_seen_ini, CALI_INI_PATH),
                "the workmode route resolves out of %s (got %s) — the persisted "
                "cali store, not some other path",
                CALI_INI_PATH, tst_seen_ini ? tst_seen_ini : "(never called)");
    }

    if (fitted_here("workmode")) {
        reset_state();
        mm_fan_target_set_manual(85);
        tst_cali_temp = 70;
        int rc        = route("workmode", "0");
        CHECK(rc == MC_SET_OK && has_act("wm=0"),
                "with the fan target hand-set to 85 degC, a workmode change still succeeds and "
                "is persisted (rc %d, acts %s)",
                rc, acts());
        CHECK(ROUTE_RESOLVES == 2,
                "...and the aging gate and the fan retarget each resolve once (%d call(s))",
                ROUTE_RESOLVES);
        CHECK(g_fan_target_temp == 85,
                "...and the fan target stays at the hand-set %u degC, not the new mode's %u",
                g_fan_target_temp, 70);
    }

    if (fitted_here("workmode")) {
        reset_state();
        tst_aged         = 0;
        unsigned running = mm_workmode_running();
        int      rc      = route("workmode", "0");
        CHECK(rc == MC_SET_ERR_ARG && !has_act("wm=0") && g_fan_target_temp == FAN_TARGET_TEMP,
                "a running mode without a calibration record (aging, or aging "
                "failed) refuses workmode MC_SET_ERR_ARG (%d) and persists "
                "nothing (acts: %s)",
                rc, acts());
        CHECK(tst_seen_mode == running, "...the gate asks about the RUNNING mode, %u (got %u)",
                running, tst_seen_mode);
        reset_state();
        tst_aged = 0;
        rc       = mm_workmode_button_cycle();
        CHECK(rc == -1 && tst_wm_mode == 2,
                "...and so is the work mode button (%d), through the same gate", rc);
        reset_state();
        rc = mm_workmode_button_cycle();
        if (mm_workmode_max() > 0)
            CHECK(rc == 0 && tst_wm_mode == 0,
                    "an aged running mode lets the button cycle to mode 0 (%d)", rc);
        else
            CHECK(rc == -1 && tst_wm_mode == 2,
                    "a board with one work mode ignores the button and keeps "
                    "the selection (%d)",
                    rc);
    }

    reset_state();
    {
        int rc     = route_op(0u);
        int pinned = g_fan_st.fixed_duty;
        int rc_hi  = route_op((uint16_t)0xffffu);
        CHECK(rc == MC_SET_ERR_UNSUP,
                "opcode 0 — not an opcode at all — is refused MC_SET_ERR_UNSUP "
                "(%d)",
                rc);
        CHECK(rc_hi == MC_SET_ERR_UNSUP,
                "opcode 0xffff — past the table — is refused MC_SET_ERR_UNSUP "
                "(%d)",
                rc_hi);
        static const uint16_t unrouted[] = { CTRL_OP_NONE, CTRL_OP_RESERVED_5, CTRL_OP_COUNT,
            (uint16_t)0xffffu };
        for (unsigned i = 0; i < sizeof unrouted / sizeof unrouted[0]; i++) {
            mc_ctrl_cmd c;
            memset(&c, 0, sizeof c);
            c.op          = unrouted[i];
            int rc_direct = (int)mm_ctrl_apply(&c);
            CHECK(rc_direct == MC_SET_ERR_UNSUP,
                    "mm_ctrl_apply refuses opcode %u MC_SET_ERR_UNSUP on "
                    "its own, without the availability gate in front (%d)",
                    (unsigned)unrouted[i], rc_direct);
        }
        CHECK(pinned == -1,
                "opcode 0 actuates NOTHING: the fan pin is still released (%d). "
                "The switch has no default that falls into an arm",
                pinned);
        CHECK(g_fan_st.fixed_duty == -1,
                "opcode 0xffff actuates NOTHING either (%d) — a core built with "
                "an opcode this mm does not have cannot land on the wrong "
                "actuator",
                g_fan_st.fixed_duty);
    }

    if (fitted_here("softon") && fitted_here("softoff")) {
        reset_state();
        route("softoff", "now");
        int off_req = g_api_req;
        reset_state();
        route("softon", "now");
        CHECK(off_req == -1,
                "`softoff` reaches mm_apply_soft as OFF: g_api_req is -1 (got "
                "%d). The soft arm serves both names off one handler, so losing "
                "the name here would stop a miner the user asked to start",
                off_req);
        CHECK(g_api_req == 1,
                "`softon` reaches the same handler as ON: g_api_req is 1 (got "
                "%d) — the name, not the arm, is what picks the direction",
                g_api_req);
    }

    if (fitted_here("ledmode") && fitted_here("nightlamp")) {
        reset_state();
        route("ledmode", "1");
        int led_named = has_act("led=ledmode");
        reset_state();
        route("nightlamp", "1-180");
        CHECK(led_named,
                "`ledmode` reaches mm_led_apply carrying ledmode's own op — the "
                "light arm is one function for three commands, so the op is what "
                "picks which of the three the strip does");
        CHECK(has_act("led=nightlamp"),
                "`nightlamp` reaches the same function carrying nightlamp's own "
                "op, not a sibling's (acts: %s)",
                acts());
    }

    {
        static const struct {
            uint16_t    op;
            const char *args;
        } lit[] = {
            { CTRL_OP_LEDMODE, "1" },
            { CTRL_OP_LEDSET, "2-100-100-0-0-255" },
            { CTRL_OP_NIGHTLAMP, "1-180" },
        };
        for (unsigned i = 0; i < sizeof lit / sizeof lit[0]; i++) {
            if (!ctrl_cmd_available(lit[i].op))
                continue;
            reset_state();
            int rc = route(ctrl_cmd_name(lit[i].op), lit[i].args);
            CHECK(rc == MC_SET_INFO,
                    "`%s` is answered MC_SET_INFO (%d) by the light arm — the "
                    "code the real mm_led_apply returns, distinct from the "
                    "fall-through's UNSUP",
                    ctrl_cmd_name(lit[i].op), rc);
            CHECK(tst_led_op == lit[i].op,
                    "`%s` reaches mm_led_apply carrying its OWN opcode %u, not a "
                    "sibling's (got %u) — the light arm serves three commands off "
                    "one function, so the op is the only thing that picks which "
                    "of the three the strip actually does",
                    ctrl_cmd_name(lit[i].op), lit[i].op, tst_led_op);
        }
    }

    CHECK(!strcmp(ctrl_cmd_name(CTRL_OP_LEDMODE), "ledmode"),
            "CTRL_OP_LEDMODE still spells itself `ledmode` on the wire (got %s) "
            "— the name is a published contract with shipped handsets, so "
            "renaming this row breaks phones in the field without breaking the "
            "build",
            ctrl_cmd_name(CTRL_OP_LEDMODE));
    CHECK(!strcmp(ctrl_cmd_name(CTRL_OP_LEDSET), "ledset"),
            "CTRL_OP_LEDSET still spells itself `ledset` on the wire (got %s)",
            ctrl_cmd_name(CTRL_OP_LEDSET));
    CHECK(!strcmp(ctrl_cmd_name(CTRL_OP_NIGHTLAMP), "nightlamp"),
            "CTRL_OP_NIGHTLAMP still spells itself `nightlamp` on the wire (got "
            "%s)",
            ctrl_cmd_name(CTRL_OP_NIGHTLAMP));

    {
        reset_state();
        int rc   = route_op(CTRL_OP_NONE);
        int side = 0;
        for (int k = 0; k < tst_act_n && k < TST_ACT_MAX; k++)
            if (strncmp(tst_act[k], "ack", 3) && strcmp(tst_act[k], "periph") &&
                    strncmp(tst_act[k], "cali", 4))
                side = 1;
        CHECK(rc == MC_SET_ERR_UNSUP,
                "an opcode with no row answers MC_SET_ERR_UNSUP (%d) — the app "
                "reporting a failure is the only symptom there is",
                rc);
        CHECK(!side,
                "an opcode with no row actuates NOTHING — no partial apply on the "
                "way to the fall-through (acts: %s)",
                acts());
    }

    if (fitted_here("ledmode")) {
        reset_state();
        int rc = route("ledmode", "1");
        CHECK(tst_ack_n == 1,
                "a recognised message gets EXACTLY ONE ACK (%d) — core waits on "
                "one reply, so a second would be read as the answer to the next "
                "command",
                tst_ack_n);
        CHECK(tst_periph_n == 1, "a recognised message pushes exactly one mm_periph snapshot (%d)",
                tst_periph_n);
        CHECK(tst_ack_after_periph,
                "the mm_periph snapshot is pushed BEFORE the ACK (ordered %d) — "
                "the app re-reads estats right after its reply, and LED[..] is "
                "the only readback ledmode has",
                tst_ack_after_periph);
        CHECK(rc == MC_SET_INFO,
                "this is the RECOGNISED arm, pinned by the handler's own "
                "MC_SET_INFO (%d) rather than by an ACK count the UNSUP arm "
                "produces too",
                rc);
        CHECK(has_act("led=ledmode"),
                "...and by its side effect: the light handler really ran (acts: "
                "%s)",
                acts());
    }

    reset_state();
    route_op(CTRL_OP_NONE);
    CHECK(tst_ack_n == 1,
            "...and an UNSUPPORTED command is ACKed exactly once too (%d): core "
            "is never left waiting on a command mm refused",
            tst_ack_n);
    CHECK(tst_periph_n == 1,
            "...and it still pushes one mm_periph snapshot (%d): the push is "
            "unconditional, even though nothing changed",
            tst_periph_n);
    CHECK(tst_ack_after_periph,
            "...and the push still comes before the ACK on the refused arm "
            "(ordered %d)",
            tst_ack_after_periph);

    {
        mc_ctrl_cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.op              = CTRL_OP_WORKMODE;
        cmd.v.workmode.mode = 1;

        reset_state();
        tst_act_n = tst_ack_n = tst_periph_n = 0;
        mm_ctrl_cb(MSG_MM_PERIPH, &cmd, (int)sizeof cmd);
        int a_type = tst_ack_n + tst_periph_n;

        reset_state();
        tst_act_n = tst_ack_n = tst_periph_n = 0;
        mm_ctrl_cb(5u, &cmd, (int)sizeof cmd);
        int a_ret5 = tst_ack_n + tst_periph_n;

        reset_state();
        tst_act_n = tst_ack_n = tst_periph_n = 0;
        mm_ctrl_cb(11u, &cmd, (int)sizeof cmd);
        int a_ret11 = tst_ack_n + tst_periph_n;

        reset_state();
        tst_act_n = tst_ack_n = tst_periph_n = 0;
        mm_ctrl_cb(MSG_CTRL_CMD, &cmd, (int)sizeof cmd - 1);
        int a_short = tst_ack_n + tst_periph_n;

        reset_state();
        tst_act_n = tst_ack_n = tst_periph_n = 0;
        mm_ctrl_cb(MSG_CTRL_CMD, NULL, (int)sizeof cmd);
        int a_null = tst_ack_n + tst_periph_n;

        CHECK(a_type == 0,
                "a message on the WRONG type (MSG_MM_PERIPH) is dropped in "
                "silence — no ACK, no periph push (%d sent)",
                a_type);
        CHECK(a_ret5 == 0,
                "a message on RETIRED id 5 (MSG_CTRL_REQ) is dropped in "
                "silence (%d sent) — a stale core cannot be read through the current "
                "struct",
                a_ret5);
        CHECK(a_ret11 == 0,
                "a message on id 11 — not a valid ctrl command here (the id is "
                "reused as MSG_MM_STATUS, the mm->core direction) — is dropped in "
                "silence (%d sent), a separate row in the guard",
                a_ret11);
        CHECK(a_short == 0,
                "a message one byte SHORT of mc_ctrl_cmd is dropped in silence "
                "(%d sent) — no field is read out of a truncated payload",
                a_short);
        CHECK(a_null == 0, "a message with a NULL payload is dropped in silence (%d sent)", a_null);
        CHECK(!has_act("wm=1"),
                "...and none of those five refusals APPLIED anything: the early "
                "return sits BEFORE the routing table, so a malformed `workmode "
                "1` never reaches the mode store");
    }

    reset_state();
    if (fitted_here("workmode")) {
        mc_ctrl_cmd raw;
        memset(&raw, 0, sizeof raw);
        raw.op = CTRL_OP_WORKMODE;

        raw.v.workmode.level = WORK_LEVEL_COUNT - 1;
        route_reset();
        mm_ctrl_cb(MSG_CTRL_CMD, &raw, (int)sizeof raw);
        int top = tst_ack_n ? (int)tst_ack : -1;

        reset_state();
        raw.v.workmode.level = WORK_LEVEL_COUNT;
        route_reset();
        mm_ctrl_cb(MSG_CTRL_CMD, &raw, (int)sizeof raw);
        int over           = tst_ack_n ? (int)tst_ack : -1;
        int over_persisted = has_act("wm=0");

        raw.v.workmode.level = -1;
        route_reset();
        mm_ctrl_cb(MSG_CTRL_CMD, &raw, (int)sizeof raw);
        int under           = tst_ack_n ? (int)tst_ack : -1;
        int under_persisted = has_act("wm=0");

        CHECK(top == MC_SET_OK,
                "level %d (WORK_LEVEL_COUNT-1) is the highest mm ACCEPTS (%d) — "
                "the band is this descriptor's, so a board declaring more levels "
                "moves it; a literal back in mm_apply_workmode does not",
                WORK_LEVEL_COUNT - 1, top);
        CHECK(over == MC_SET_ERR_ARG,
                "level %d (WORK_LEVEL_COUNT) is the lowest mm REFUSES (%d) — one "
                "past the descriptor's own top",
                WORK_LEVEL_COUNT, over);
        CHECK(under == MC_SET_ERR_ARG,
                "level -1 is refused too (%d): the field is signed, so the floor "
                "needs its own guard",
                under);
        CHECK(!over_persisted && !under_persisted,
                "...and those refusals persist NOTHING (%d / %d): the bound is "
                "tested before the mode is written, so a rejected command "
                "cannot leave the store on a mode the user never got",
                over_persisted, under_persisted);
    }

    {
        uint8_t top = 0;
        for (uint8_t m = 0; m < CALI_MODE_MAX; m++)
            if (wm_allowed(m))
                top = m;
        reset_state();
        CHECK(mm_workmode_max() == top,
                "[%s] mm reports maxmode %u, the highest mode BOARD_WORKMODE_RANGES "
                "admits (got %u)",
                BOARDCONF_SECTION, top, mm_workmode_max());
        for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
            uint8_t want = wm_allowed(m) ? m : (uint8_t)CALI_MODE_DEFAULT;
            reset_state();
            tst_wm_mode = m;
            CHECK(mm_workmode_running() == want,
                    "[%s] a persisted mode %u runs as %u at start-up (got %u)", BOARDCONF_SECTION,
                    m, want, mm_workmode_running());
            if (!fitted_here("workmode"))
                continue;
            mc_ctrl_cmd raw;
            memset(&raw, 0, sizeof raw);
            raw.op              = CTRL_OP_WORKMODE;
            raw.v.workmode.mode = m;
            route_reset();
            mm_ctrl_cb(MSG_CTRL_CMD, &raw, (int)sizeof raw);
            int  rc = tst_ack_n ? (int)tst_ack : -1;
            char tag[8];
            snprintf(tag, sizeof tag, "wm=%u", m);
            if (wm_allowed(m))
                CHECK(rc == MC_SET_OK && has_act(tag),
                        "[%s] mm applies and persists mode %u (rc %d, acts %s)", BOARDCONF_SECTION,
                        m, rc, acts());
            else
                CHECK(rc == MC_SET_ERR_ARG && !has_act(tag),
                        "[%s] mm refuses mode %u outside BOARD_WORKMODE_RANGES and "
                        "persists nothing (rc %d, acts %s)",
                        BOARDCONF_SECTION, m, rc, acts());
        }
    }

    reset_state();
    tst_wm_set_rc = -1;
    if (fitted_here("workmode")) {
        int rc = route("workmode", "0");
        CHECK(rc == MC_SET_ERR_HW,
                "a workmode that cannot be PERSISTED answers MC_SET_ERR_HW (%d), "
                "distinct from both MC_SET_ERR_ARG and the table's "
                "MC_SET_ERR_UNSUP — the app can tell a storage failure from a bad "
                "argument",
                rc);
        CHECK(ROUTE_RESOLVES == 1,
                "...and only the aging gate resolves calibration (%d call(s)) "
                "when the persist fails",
                ROUTE_RESOLVES);
        CHECK(g_fan_target_temp == FAN_TARGET_TEMP,
                "...so the fan target stays at %u degC rather than following a "
                "mode the board will not remember across a reboot",
                g_fan_target_temp);
    }
    tst_wm_set_rc = 0;

    for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
        reset_state();
        tst_cali_temp = (uint16_t)(70 + m);
        tst_resolves  = 0;
        tst_act_n     = 0;
        mm_fan_target_apply(m);
        CHECK(tst_resolves == 1, "mm_fan_target_apply(%u) resolves EXACTLY ONCE (%d call(s))", m,
                tst_resolves);
        CHECK(tst_seen_mode == m,
                "mm_fan_target_apply(%u) resolves THAT mode's cali record and not "
                "a neighbour's (got %u)",
                m, tst_seen_mode);
        CHECK(tst_seen_cnt == MAX_ASIC_ON_SINGLE_HASH,
                "mm_fan_target_apply(%u) asks for THIS descriptor's chip "
                "population, %d (got %u)",
                m, MAX_ASIC_ON_SINGLE_HASH, tst_seen_cnt);
        CHECK(tst_seen_level == 0, "mm_fan_target_apply(%u) resolves at level 0 (got %d)", m,
                (int)tst_seen_level);
        CHECK(g_fan_target_temp == 70 + m,
                "mm_fan_target_apply(%u) takes the record's temp as the fan "
                "target: %u degC (got %u). Nothing else moves — the rail and the "
                "V/F stay with asic_miner",
                m, (unsigned)(70 + m), g_fan_target_temp);
        CHECK(g_fan_target_manual == 0,
                "mm_fan_target_apply(%u) does NOT set the manual latch (%d): "
                "reading a mode's target is not the user pinning one",
                m, g_fan_target_manual);
    }

    reset_state();
    g_fan_target_temp   = 77;
    g_fan_target_manual = 1;
    tst_resolves        = 0;
    for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
        tst_cali_temp = 99;
        mm_fan_target_apply(m);
    }
    CHECK(tst_resolves == (int)CALI_MODE_MAX,
            "with the manual latch set, each mode retarget still resolves its "
            "cali record exactly once (%d call(s) across all %d modes) and "
            "discards the %u degC it answers",
            tst_resolves, (int)CALI_MODE_MAX, 99);
    CHECK(g_fan_target_temp == 77,
            "...and the hand-set target still stands at %u degC after all %d mode "
            "retargets",
            g_fan_target_temp, (int)CALI_MODE_MAX);

    CHECK(g_fan_target_manual == 1,
            "...and nothing in this path clears the latch: there is NO release. "
            "No writer of g_fan_target_manual clears it — so a hand-set target survives "
            "every later workmode change and is only undone by a restart (latch "
            "%d). Pinned as shipped",
            g_fan_target_manual);

    return tst_done();
}
