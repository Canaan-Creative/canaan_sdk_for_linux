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
#include "mm_spawn.h"
#include "tst_board_expect.h"
#include "fan_dev.h"
#include "miner_ipc.h"
#include "mm_telem.h"
#include "osif.h"
#include "wdt_dev.h"
#include "fan_pid.h"
#include "softonoff.h"
#include "cali.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* asic_miner spawn delay, s: none */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_MAX_PASS     64                 /* supervisor passes a scenario may run */
#define TST_WP_CAP       (TST_MAX_PASS * 8) /* waitpid() calls before g_run is forced to 0 */
#define TST_PF_TRIP_PASS VOUT_BELOW_LOW_CNT /* pass the low-Vout power fault trips */
#define TST_VOUT_OK      ((int)POWER_VALID_VOLT_MIN)     /* lowest valid PSU Vout, rail counts */
#define TST_VOUT_LOW     ((int)POWER_VALID_VOLT_MIN - 1) /* PSU Vout one count too low */
/* Pass after the power-fault stop reaches the SIGKILL escalation */
#define TST_PF_KILL_PASS       (TST_PF_TRIP_PASS + (int)(CHILD_TERM_MS / 1000u) + 1)
#define TST_HF_REAP_PASS       3 /* pass on which asic_miner is first reaped */
#define TST_RESTART_PASSES     ((int)(RESTART_MS / 1000u)) /* passes in RESTART_MS */
#define TST_EXEC_FAILED_STATUS (127 << 8)                  /* wait status of a child exiting 127 */
#if IMPL_PSU_IS_DCDC_PD
#define TST_POUT_HIGH    (MAX_POUT_LIMIT_ABNORMAL + 1) /* over the limit, W */
#define TST_PO_TRIP_PASS POUT_OVER_HIGH_CNT            /* pass the fault trips */
#endif                                                 /* IMPL_PSU_IS_DCDC_PD */

#define TST_EV_STR_MAX 512 /* ev_str() result buffer size, bytes */

#define TST_FAN_DUTY_EV /* log each fan duty set as a "duty" event */

/* Calls in the included sources, routed to the tst_* fakes below */
#define cali_resolve(p, c, m, l, o, a) tst_cali_resolve((p), (c), (m), (l), (o), (a))
#define fork()                         tst_fork()
#define system(c)                      tst_system(c)
#define kill(p, s)                     tst_kill((p), (s))
#define waitpid(p, s, o)               tst_waitpid((p), (s), (o))
#define stat(p, b)                     tst_stat((p), (b))
#define fan_pid_set_fixed(s, d)        tst_fan_fixed((s), (d))
#define osif_ipc_send(h, t, d, l)      tst_ipc_send((h), (t), (d), (l))

pid_t                tst_fork(void);
int                  tst_system(const char *cmd);
int                  tst_kill(pid_t pid, int sig);
pid_t                tst_waitpid(pid_t pid, int *status, int options);
int                  tst_stat(const char *path, struct stat *st);
static void          tst_fan_fixed(fan_pid_state_t *st, int duty_pct);
static osif_status_t tst_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len);
static int tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
        cali_param *out, uint8_t *aged);

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
#include "mm_miner_tu.h"
#undef osif_ipc_send
#undef fan_pid_set_fixed
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork
#undef cali_resolve

_Static_assert(CHILD_TERM_MS % 1000u == 0,
        "the SIGKILL escalation must land on a whole supervised pass");
_Static_assert(VOUT_BELOW_LOW_CNT > 1,
        "a one-reading debounce would make the consecutive-run assertions vacuous");
#if IMPL_PSU_IS_DCDC_PD
_Static_assert(POUT_OVER_HIGH_CNT > 1,
        "a one-reading debounce would make the consecutive-run assertions vacuous");
#endif /* IMPL_PSU_IS_DCDC_PD */

typedef void (*tst_script_fn)(int pass); /* per-pass scenario hook, pass from 0 */

static int      tst_aging_failed;   /* one-shot mm_vf_take_aging_failed() result */
static int      tst_persisted_high; /* nonzero: mode 2 resolves aged with max_pout 150 W */
static int      tst_all_aged;       /* nonzero: every resolved mode reports aged */
static int      tst_net_up;         /* nonzero reports the network up with an IP */
static uint64_t tst_now;            /* fake monotonic clock, ms */
/* Passes run and wanted; TST_WP_CAP overrun flag; waitpid() calls */
static int           tst_pass, tst_want, tst_runaway, tst_wp_calls;
static int           tst_pass_ev0[TST_MAX_PASS + 1]; /* first event index of each pass */
static tst_script_fn tst_script;
/* mm_telem_get() poll_fresh flag, asic_live and in_work flag, PSU Vout in rail counts */
static int   tst_fresh, tst_asic_live, tst_vout;
static int   tst_pout, tst_vout12;           /* mm_telem_get() output power, W; PSU 12 V reading */
static pid_t tst_next_pid = 4000;            /* next fake fork() PID */
static pid_t tst_pid_core, tst_pid_asic;     /* last spawned core_miner and asic_miner PIDs */
static int   tst_reap_pend, tst_reap_status; /* asic_miner reaps pending; their wait status */
/* asic_miner exec failures to report; exec-failure reaps done */
static int      tst_exec_fail_n, tst_exec_reaps;
static int      tst_rpm = FAN_RPM_NONE; /* every fan's tach reading, RPM */
static int      tst_fan_duty_pct;       /* duty the fan_health scripts command, % */
static uint16_t tst_pd_watts = 140;     /* PD PSU output power limit, W */
static int      tst_pd_rc;              /* PD PSU pout_limit() result */
static uint8_t  tst_workmode;           /* stored work mode */
static int      tst_mode_write_rc;      /* nonzero fails cali_workmode_set() */
static int      tst_pd_reads;           /* PD PSU pout_limit() call count */

#include "tst_events.h"
#include "tst_fan_dev.h"

int mm_vf_take_aging_failed(void)
{
    int r = tst_aging_failed;

    tst_aging_failed = 0;
    return r;
}

static int tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
        cali_param *out, uint8_t *aged)
{
    int rc = cali_resolve(path, asic_cnt, mode, level, out, aged);
    if (rc == 0 && tst_persisted_high && mode == 2) {
        out->max_pout = 150;
        if (aged)
            *aged = 1;
    }
    if (rc == 0 && tst_all_aged && aged)
        *aged = 1;
    return rc;
}

int net_state_get(void)
{
    return tst_net_up ? 2 : 0;
}
int net_state_ip(char *ip, size_t cap)
{
    if (!ip || !cap)
        return -1;
    snprintf(ip, cap, "10.0.0.1");
    return 0;
}

static void tst_net_raise(void)
{
    tst_net_up = 1;
}

static int ev_last_val(const char *what)
{
    for (int i = tst_ev_n - 1; i >= 0; i--)
        if (!strcmp(tst_ev[i].what, what))
            return tst_ev[i].val;
    return -1;
}
static int p0(int p)
{
    return tst_pass_ev0[p < TST_MAX_PASS ? p : TST_MAX_PASS];
}
static int tst_run_end(void)
{
    int j = ev_first(0, "join");
    return j < 0 ? tst_ev_n : j;
}

uint64_t osif_now_ms(void)
{
    return tst_now;
}
void osif_sleep_ms(uint32_t ms)
{
    tst_now += ms;
}

void mm_telem_ingest(const void *d, int l, unsigned c)
{
    (void)d;
    (void)l;
    (void)c;
}
void mm_telem_ingest_core(const void *d, int l)
{
    (void)d;
    (void)l;
}
void mm_telem_poll(void)
{
    if (tst_pass < TST_MAX_PASS)
        tst_pass_ev0[tst_pass] = tst_ev_n;
    tst_script(tst_pass);
    if (++tst_pass >= tst_want)
        g_run = 0;
    if (tst_pass <= TST_MAX_PASS)
        tst_pass_ev0[tst_pass] = tst_ev_n;
}

void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
    out->poll_fresh = tst_fresh;
    out->asic_live  = tst_asic_live;
    out->in_work    = tst_asic_live;
    out->tmax       = 60;
    out->tavg       = 55;
    out->ps_vout_mv = tst_vout;
    out->pout_w     = tst_pout;
    out->ps_vout12  = tst_vout12;
    out->hb_count   = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++)
        out->hb_outlet[b] = out->hb_inlet[b] = 60;
}

int fan_dev_get_rpm(int fan)
{
    (void)fan;
    return tst_rpm;
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

int tst_children_stop(const pid_t *pids, int n, unsigned t, unsigned k)
{
    (void)pids;
    (void)n;
    (void)t;
    (void)k;
    ev("join", 0);
    return 0;
}

pid_t tst_fork(void)
{
    int   is_asic = (g_enum_hold.phase == ENUM_HOLD_PHASE_ARMED);
    pid_t p       = tst_next_pid++;
    if (is_asic)
        tst_pid_asic = p;
    else
        tst_pid_core = p;
    ev(is_asic ? "spawn-asic" : "spawn-core", (int)p);
    return p;
}
int board_power_rail_on(uint16_t counts)
{
    (void)counts;
    ev("rail-on", 0);
    return 0;
}
int board_power_rail_off(void)
{
    ev("rail-off", 0);
    return 0;
}
const char *hal_psu_rail_strerror(int rc)
{
    (void)rc;
    return "test";
}
int tst_system(const char *cmd)
{
    (void)cmd;
    ev("system?", 0);
    return 0;
}
int tst_kill(pid_t pid, int sig)
{
    ev(pid == tst_pid_core ? "kill-core" : pid == tst_pid_asic ? "kill-asic" : "kill-?", sig);
    return 0;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    if (status)
        *status = 0;
    if (++tst_wp_calls > TST_WP_CAP) {
        tst_runaway = 1;
        g_run       = 0;
        return 0;
    }
    if (tst_reap_pend > 0 && pid > 0 && pid == tst_pid_asic) {
        tst_reap_pend--;
        if (status)
            *status = tst_reap_status;
        return tst_pid_asic;
    }
    return 0;
}
int tst_stat(const char *path, struct stat *st)
{
    (void)path;
    if (st)
        memset(st, 0, sizeof *st);
    return -1;
}

static void tst_fan_fixed(fan_pid_state_t *st, int duty_pct)
{
    ev("fix", duty_pct);
    fan_pid_set_fixed(st, duty_pct);
}

static osif_status_t tst_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    if (type == MSG_MM_PERIPH && data && (size_t)len >= sizeof(mm_periph))
        ev("push", ((const mm_periph *)data)->softoff_rsn);
    else if (type == MSG_MM_STATUS && data && (size_t)len == sizeof(mc_mm_status))
        ev("state", ((const mc_mm_status *)data)->state);
    else
        ev("send?", (int)type);
    return OSIF_OK;
}

uint8_t cali_workmode_get(const cali_wm_store *s)
{
    (void)s;
    return tst_workmode;
}
int cali_workmode_set(const cali_wm_store *s, uint8_t mode)
{
    (void)s;
    if (tst_mode_write_rc)
        return -1;
    tst_workmode = mode;
    return 0;
}
static int tst_pd_limit(uint16_t *watts)
{
    tst_pd_reads++;
    *watts = tst_pd_watts;
    return tst_pd_rc;
}
const hal_psu_ops_t *hal_psu(void)
{
    static const hal_psu_ops_t psu = {
        .pout_limit = tst_pd_limit,
        .vout_fault = tst_psu_vout_fault,
        .flags      = TST_PSU_FLAGS,
    };
    return &psu;
}

static void scenario(int passes, tst_script_fn s)
{
    tst_now         = 0;
    tst_pass        = 0;
    tst_want        = passes;
    tst_script      = s;
    tst_runaway     = 0;
    tst_ev_n        = 0;
    tst_wp_calls    = 0;
    tst_reap_pend   = 0;
    tst_exec_reaps  = 0;
    tst_reap_status = 0;
    tst_pid_core = tst_pid_asic = 0;
    memset(tst_pass_ev0, 0, sizeof tst_pass_ev0);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_api_req           = 0;
    g_fault_off         = 0;
    g_softoff_rsn       = MC_SOFTOFF_NONE;
    g_asic_ever_live    = 0;
    g_asic_exec_fails   = 0;
    tst_fresh           = 1;
    tst_asic_live       = 1;
    tst_vout            = TST_VOUT_OK;
    tst_pout            = 0;
    tst_vout12          = 1200;
    g_run               = 1;
    tst_net_raise();
    mm_miner_main_unused();
}

static void s_ok(int p)
{
    (void)p;
    tst_vout = TST_VOUT_OK;
}
static void s_low(int p)
{
    tst_vout = p == 0 ? TST_VOUT_OK : TST_VOUT_LOW;
}
static void s_zero(int p)
{
    tst_vout = p == 0 ? TST_VOUT_OK : 0;
}
static void s_none(int p)
{
    tst_vout = p == 0 ? TST_VOUT_OK : MM_TELEM_VOUT_NONE;
}
static void s_alt(int p)
{
    tst_vout = (p % 2) ? TST_VOUT_LOW : TST_VOUT_OK;
}
static void s_bad_mixed(int pass)
{
    tst_vout = pass == 0 ? TST_VOUT_OK : pass % 2 ? MM_TELEM_VOUT_NONE : TST_VOUT_LOW;
}
static void s_missing_recovery(int pass)
{
    tst_vout = pass == 0 || pass % VOUT_BELOW_LOW_CNT == 0 ? TST_VOUT_OK : MM_TELEM_VOUT_NONE;
}
static void s_off_unreadable(int pass)
{
    tst_vout = pass < 2 ? TST_VOUT_OK : MM_TELEM_VOUT_NONE;
    if (pass == 1)
        g_api_req = -1;
}
static void s_refund(int p)
{
    tst_vout = (p == 0 || p == VOUT_BELOW_LOW_CNT) ? TST_VOUT_OK : TST_VOUT_LOW;
}
static void s_low_reap(int p)
{
    s_low(p);
    if (p == TST_PF_TRIP_PASS + 1)
        tst_reap_pend = 1;
}
static void s_ok_reap(int p)
{
    s_ok(p);
    if (p == TST_PF_TRIP_PASS + 1)
        tst_reap_pend = 1;
}
static void s_low_kill_reap(int p)
{
    s_low(p);
    if (p == TST_PF_KILL_PASS + 1) {
        tst_reap_pend   = 1;
        tst_reap_status = SIGKILL;
    }
}
static int ev_sig(int from, int to, const char *what, int sig)
{
    int n = 0;
    for (int i = from; i < to && i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, what) && tst_ev[i].val == sig)
            n++;
    return n;
}
static int ev_sig_pass(const char *what, int sig)
{
    for (int i = 0; i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, what) && tst_ev[i].val == sig)
            return tst_ev[i].pass;
    return -1;
}
static void s_never_live(int p)
{
    tst_vout      = TST_VOUT_OK;
    tst_asic_live = 0;
    if (p == TST_HF_REAP_PASS)
        tst_reap_pend = 1;
}
static void s_was_live(int p)
{
    tst_vout      = TST_VOUT_OK;
    tst_asic_live = 1;
    if (p == TST_HF_REAP_PASS)
        tst_reap_pend = 1;
}
static void s_low_dead(int p)
{
    s_low(p);
    tst_asic_live = 0;
}
static void s_low_dead_reap(int p)
{
    s_low_dead(p);
    if (p == TST_PF_TRIP_PASS + 1)
        tst_reap_pend = 1;
}
static void s_exec_failed(int p)
{
    tst_vout      = TST_VOUT_OK;
    tst_asic_live = 0;
    if (p >= TST_HF_REAP_PASS && !tst_reap_pend && tst_exec_reaps < tst_exec_fail_n) {
        tst_exec_reaps++;
        tst_reap_pend   = 1;
        tst_reap_status = TST_EXEC_FAILED_STATUS;
    }
}
static void s_off_low(int p)
{
    tst_vout = p < 2 ? TST_VOUT_OK : TST_VOUT_LOW;
    if (p == 1)
        g_api_req = -1;
}
static void s_low_api(int p)
{
    s_low(p);
    if (p == TST_PF_TRIP_PASS + 1)
        g_api_req = -1;
    if (p == TST_PF_TRIP_PASS + 2)
        g_api_req = 1;
}
#if IMPL_PSU_IS_DCDC_PD
static void s_pout_high(int p)
{
    s_ok(p);
    tst_pout = p == 0 ? 0 : TST_POUT_HIGH;
}
static void s_pout_edge(int p)
{
    s_ok(p);
    tst_pout = MAX_POUT_LIMIT_ABNORMAL;
}
static void s_pout_none(int p)
{
    s_ok(p);
    tst_pout = MM_TELEM_POUT_NONE;
}
static void s_pout_refund(int p)
{
    s_ok(p);
    tst_pout = p % POUT_OVER_HIGH_CNT == 0 ? 0 : TST_POUT_HIGH;
}
static void s_pout_api(int p)
{
    s_ok(p);
    tst_pout = p < 2 ? 0 : TST_POUT_HIGH;
    if (p == 1)
        g_api_req = -1;
}
static void s_pout_hot(int p)
{
    s_pout_high(p);
    if (p == 1)
        g_overheat_req = 1;
}
#endif /* IMPL_PSU_IS_DCDC_PD */

static void s_fan(int p)
{
    s_ok(p);
    mm_fan_pid_fixed(tst_fan_duty_pct);
}

static void fan_scenario(int passes, int rpm, int duty_pct)
{
    tst_rpm          = rpm;
    tst_fan_duty_pct = duty_pct;
    tst_duty_x10     = duty_pct * 10;
    memset(&g_fan_health_st, 0, sizeof g_fan_health_st);
    g_fan_fault_req = 0;
    scenario(passes, s_fan);
    tst_rpm = FAN_RPM_NONE;
}

int main(void)
{
#if defined(BOARD_X3_H)
    mc_ctrl_cmd mode_cmd     = { 0 };
    mode_cmd.v.workmode.mode = 2;
    tst_workmode             = 0;
    tst_all_aged             = 1;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK && mm_workmode_max() == 0,
            "before any admission only mode 0 is permitted (maxmode 0)");
    tst_workmode = 2;
    CHECK(mm_workmode_running() == 2,
            "before any admission the running mode is the selection: the saved "
            "mode shows until the clamp runs");
    tst_workmode = 0;
    tst_pd_watts = 200;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK, "200W admission succeeds");
    int pd_reads             = tst_pd_reads;
    mode_cmd.v.workmode.mode = 2;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "200W adapter rejects mode 2 (300W + 3W reserve)");
    mode_cmd.v.workmode.mode = 0;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "200W adapter permits mode 0 (180W + 3W reserve)");
    tst_pd_rc = -1;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "an unreadable PD does not fail a mode change");
    tst_pd_rc                = 0;
    tst_pd_watts             = 350;
    mode_cmd.v.workmode.mode = 2;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "a mode change uses the last admission, not a fresh PD budget");
    CHECK(tst_pd_reads == pd_reads, "mode changes never read PD (%d read(s))",
            tst_pd_reads - pd_reads);
    tst_pd_rc = -1;
    CHECK(mm_power_prepare() != HAL_PSU_RAIL_OK &&
                    mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "a failed admission keeps the previous mode ceiling");
    tst_pd_rc    = 0;
    tst_workmode = 0;
    tst_pd_watts = 302;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK &&
                    mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "302W adapter rejects mode 2");
    tst_pd_watts = 303;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK &&
                    mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "303W adapter permits exact mode 2 boundary");
    tst_pd_watts       = 200;
    tst_persisted_high = 1;
    tst_workmode       = 2;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && tst_workmode == 2 && mm_workmode_running() == 0,
            "admission budgets the factory row: a 150W persisted mode 2 does not "
            "let a 200W adapter run mode 2");
    tst_persisted_high = 0;
    tst_workmode       = 2;
    tst_mode_write_rc  = -1;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && tst_workmode == 2 &&
                    mm_workmode_running() == 0 && mm_workmode_max() == 0,
            "a 200W adapter runs a persisted mode 2 as mode 0 and never writes the "
            "selection (a failing write cannot refuse startup)");
    tst_mode_write_rc = 0;
    tst_workmode      = 2;
    tst_pd_watts      = 350;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && mm_workmode_running() == 2 &&
                    mm_workmode_max() == 2,
            "a 350W adapter at the next start runs the kept mode 2 selection again");
    CHECK(mm_power_budget() == 350, "the admission's 350W budget is kept for the servo");
    pd_reads = tst_pd_reads;
    CHECK(mm_workmode_button_cycle() == 0 && tst_workmode == 0,
            "the work mode button cycles the running mode 2 within the 350W ceiling: "
            "mode 2 -> mode 0");
    CHECK(tst_pd_reads == pd_reads,
            "the work mode button never reads PD, which would drop the hashboard rail "
            "while mining (%d read(s))",
            tst_pd_reads - pd_reads);
    tst_pd_watts = 40;
    tst_workmode = 2;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && tst_workmode == 2 && mm_workmode_running() == 0,
            "an adapter below the mode 0 budget starts, running mode 0");
    CHECK(mm_workmode_button_cycle() == -1 && tst_workmode == 2,
            "with only mode 0 allowed the work mode button does nothing and keeps the "
            "mode 2 selection (%u)",
            (unsigned)tst_workmode);
    mode_cmd.v.workmode.mode = 0;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "an adapter below the mode 0 budget permits mode 0");
    mode_cmd.v.workmode.mode = 2;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "...and still rejects mode 2");
#elif defined(BOARD_NANO3S_H)
    mc_ctrl_cmd mode_cmd     = { 0 };
    mode_cmd.v.workmode.mode = 1;
    tst_workmode             = 0;
    tst_all_aged             = 1;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK && mm_workmode_max() == 0,
            "before any admission only mode 0 is permitted (maxmode 0)");
    tst_workmode = 2;
    CHECK(mm_workmode_running() == 2,
            "before any admission the running mode is the selection: the saved "
            "mode shows until the clamp runs");
    tst_workmode = 0;
    tst_pd_watts = 100;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK, "100W admission succeeds");
    int pd_reads             = tst_pd_reads;
    mode_cmd.v.workmode.mode = 2;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "100W adapter rejects mode 2 (133W + 3W reserve)");
    mode_cmd.v.workmode.mode = 1;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "100W adapter permits mode 1 (95W + 3W reserve)");
    tst_pd_rc = -1;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "an unreadable PD does not fail a mode change");
    tst_pd_rc                = 0;
    tst_pd_watts             = 140;
    mode_cmd.v.workmode.mode = 2;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "a mode change uses the last admission, not a fresh PD budget");
    CHECK(tst_pd_reads == pd_reads, "mode changes never read PD (%d read(s))",
            tst_pd_reads - pd_reads);
    tst_pd_rc = -1;
    CHECK(mm_power_prepare() != HAL_PSU_RAIL_OK &&
                    mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "a failed admission keeps the previous mode ceiling");
    tst_pd_rc                = 0;
    tst_workmode             = 0;
    mode_cmd.v.workmode.mode = 1;
    tst_pd_watts             = 97;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK &&
                    mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "97W adapter rejects mode 1");
    tst_pd_watts = 98;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK &&
                    mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "98W adapter permits exact mode 1 boundary");
    tst_pd_watts       = 140;
    tst_persisted_high = 1;
    tst_workmode       = 2;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && tst_workmode == 2,
            "admission budgets the factory row: a 150W persisted mode 2 still "
            "fits a 140W adapter");
    tst_persisted_high = 0;
    tst_pd_watts       = 100;
    tst_workmode       = 2;
    tst_mode_write_rc  = -1;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && tst_workmode == 2 &&
                    mm_workmode_running() == 1 && mm_workmode_max() == 1,
            "a 100W adapter runs a persisted mode 2 as mode 1 and never writes the "
            "selection (a failing write cannot refuse startup)");
    tst_mode_write_rc = 0;
    pd_reads          = tst_pd_reads;
    CHECK(mm_workmode_button_cycle() == 0 && tst_workmode == 0 && mm_workmode_button_cycle() == 1 &&
                    tst_workmode == 1,
            "the TP2 button cycles the running mode 1 within the 100W ceiling: "
            "mode 1 -> mode 0 -> mode 1");
    CHECK(tst_pd_reads == pd_reads,
            "the TP2 button never reads PD, which would drop the hashboard rail "
            "while mining (%d read(s))",
            tst_pd_reads - pd_reads);
    tst_workmode = 2;
    tst_pd_watts = 140;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && mm_workmode_running() == 2 &&
                    mm_workmode_max() == 2,
            "a 140W adapter at the next start runs the kept mode 2 selection again");
    CHECK(mm_power_budget() == 140, "the admission's 140W budget is kept for the servo");
    tst_pd_watts = 40;
    tst_workmode = 2;
    CHECK(mm_power_prepare() == HAL_PSU_RAIL_OK && tst_workmode == 2 && mm_workmode_running() == 0,
            "an adapter below the mode 0 budget starts, running mode 0");
    CHECK(mm_workmode_button_cycle() == -1 && tst_workmode == 2,
            "with only mode 0 allowed the TP2 button does nothing and keeps the "
            "mode 2 selection (%u)",
            (unsigned)tst_workmode);
    mode_cmd.v.workmode.mode = 0;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) == MC_SET_OK,
            "an adapter below the mode 0 budget permits mode 0");
    mode_cmd.v.workmode.mode = 1;
    CHECK(mm_apply_workmode(CTRL_OP_WORKMODE, &mode_cmd) != MC_SET_OK,
            "...and still rejects mode 1");
#else
#error "no power-admission goldens for this board"
#endif
    tst_all_aged = 0;
    tst_pd_rc    = -1;
    scenario(3, s_ok);
    CHECK(g_fault_off && ev_count(0, tst_run_end(), "spawn-asic") == 0,
            "unreadable adapter capacity prevents ASIC startup");
    pid_t stopped_asic  = -1;
    pid_t running_core  = tst_pid_core;
    int   spawns_before = ev_count(0, tst_ev_n, "spawn-asic");
    g_asic_retry_at     = 1;
    mm_spawn_retry(RESTART_MS + 1, &running_core, &stopped_asic);
    CHECK(stopped_asic == -1 && g_asic_retry_at == 0 &&
                    ev_count(0, tst_ev_n, "spawn-asic") == spawns_before,
            "pending spawn retry cannot bypass a latched power-admission failure");
    mm_thermal_act(SOFTONOFF_THERMAL_RESUME, &stopped_asic);
    CHECK(g_fault_off && stopped_asic == -1 && g_softoff_rsn == MC_SOFTOFF_POWER_FAULT,
            "thermal resume cannot bypass unreadable adapter capacity");
    mm_api_act(SOFTONOFF_API_ON, 1, &stopped_asic);
    CHECK(g_fault_off && stopped_asic == -1 && g_softonoff.api_softoff,
            "API resume cannot bypass PD admission");
    tst_pd_rc = 0;
    mm_api_act(softonoff_api_step(&g_softonoff, 1), 1, &stopped_asic);
    CHECK(!g_fault_off && stopped_asic > 0,
            "API retry succeeds once adapter capacity is readable, even at 40W");
    tst_pd_watts = 140;
    printf("descriptor %s: %d board(s), POWER_VALID_VOLT_MIN %d, "
           "VOUT_BELOW_LOW_CNT %d, rail clamp %d..%d, VOL_ENUM %d\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, POWER_VALID_VOLT_MIN, VOUT_BELOW_LOW_CNT,
            (int)VOL_MIN, (int)VOL_MAX, (int)VOL_ENUM);
    CHECK(TST_PF_TRIP_PASS == 10, "every board trips on the 10th consecutive low reading (pass %d)",
            TST_PF_TRIP_PASS);

    scenario(TST_PF_TRIP_PASS + 1, s_low);
    CHECK(!tst_runaway, "the supervisor loop terminates (%d waitpid calls, cap %d)", tst_wp_calls,
            TST_WP_CAP);
    CHECK(ev_count(0, p0(TST_PF_TRIP_PASS), "rail-off") == 0,
            "%d consecutive low rail readings do NOT trip: `++pf_cnt >= "
            "VOUT_BELOW_LOW_CNT` needs the full count (%d rail-off(s) in "
            "passes 0..%d)",
            VOUT_BELOW_LOW_CNT - 1, ev_count(0, p0(TST_PF_TRIP_PASS), "rail-off"),
            TST_PF_TRIP_PASS - 1);
    CHECK(ev_count(p0(TST_PF_TRIP_PASS), tst_run_end(), "rail-off") == 1,
            "...and the %dth trips exactly once, on pass %d (saw %d)", VOUT_BELOW_LOW_CNT,
            TST_PF_TRIP_PASS, ev_count(p0(TST_PF_TRIP_PASS), tst_run_end(), "rail-off"));

    {
        static const char *const want[]     = { "kill-asic", "rail-off", "fix", "duty" };
        const int                want_val[] = { SIGTERM, 0, 0, 0 };
        int                      s          = ev_first(p0(TST_PF_TRIP_PASS), "state");
        int                      u          = ev_first(p0(TST_PF_TRIP_PASS), "push");
        int                      tp         = tst_ev[p0(TST_PF_TRIP_PASS)].pass;
        CHECK(ev_is(p0(TST_PF_TRIP_PASS), 4, want, want_val) && s >= 0 && tst_ev[s].pass == tp &&
                        tst_ev[s].val == MC_WORK_IDLE && u > s && tst_ev[u].pass == tp &&
                        tst_ev[u].val == MC_SOFTOFF_POWER_FAULT,
                "the trip SIGTERMs asic_miner, cuts the rail and pins the fan to "
                "0%% before anything else on the pass, and the same pass pushes "
                "MC_SOFTOFF_POWER_FAULT to core (saw %s)",
                ev_str(p0(TST_PF_TRIP_PASS), tst_run_end()));
    }
    CHECK(g_softoff_rsn == MC_SOFTOFF_POWER_FAULT && g_fault_off == 1,
            "...and both latches are set: softoff_rsn %d (want %d) and the "
            "respawn inhibit g_fault_off %d",
            g_softoff_rsn, MC_SOFTOFF_POWER_FAULT, g_fault_off);

    scenario(TST_PF_TRIP_PASS * 3, s_zero);
    CHECK(g_fault_off && ev_count(0, tst_run_end(), "rail-off") == 1,
            "valid zero volts trips once");

    scenario(TST_PF_TRIP_PASS * 3, s_none);
    CHECK(g_fault_off && ev_count(0, tst_run_end(), "rail-off") == 1,
            "sustained unreadable power telemetry trips once");

    scenario(TST_PF_TRIP_PASS * 3, s_alt);
    CHECK(ev_count(0, tst_run_end(), "rail-off") == 0 && !g_fault_off,
            "alternating low/good readings never trip over %d passes, however many "
            "low ones there are in total: the counter is a CONSECUTIVE run (%d "
            "rail-off(s))",
            TST_PF_TRIP_PASS * 3, ev_count(0, tst_run_end(), "rail-off"));
    scenario(TST_PF_TRIP_PASS * 3, s_bad_mixed);
    CHECK(g_fault_off && ev_count(0, tst_run_end(), "rail-off") == 1,
            "mixed missing and low readings cannot mask a continuous power fault");
    scenario(TST_PF_TRIP_PASS * 3, s_missing_recovery);
    CHECK(!g_fault_off && ev_count(0, tst_run_end(), "rail-off") == 0,
            "valid healthy readings reset the missing-telemetry debounce");
    scenario(TST_PF_TRIP_PASS * 3, s_off_unreadable);
    CHECK(!g_fault_off && g_softonoff.api_softoff && ev_count(0, tst_run_end(), "rail-off") == 1,
            "intentional soft-off does not accumulate telemetry faults");

    scenario(2 * TST_PF_TRIP_PASS, s_refund);
    CHECK(ev_count(0, tst_run_end(), "rail-off") == 0 && !g_fault_off,
            "%d lows, ONE good reading, then %d lows again does not trip either — "
            "the single good pass refunded the entire run, exactly as the "
            "watchdog's healthy pass refunds its budget (%d rail-off(s))",
            VOUT_BELOW_LOW_CNT - 1, VOUT_BELOW_LOW_CNT - 1, ev_count(0, tst_run_end(), "rail-off"));

    {
        int passes = 3 * VOUT_BELOW_LOW_CNT + 2;
        scenario(passes, s_low);
        CHECK(ev_count(0, tst_run_end(), "rail-off") == 1,
                "a rail that STAYS low is cut once: the latched fault stops the "
                "check re-arming against a rail it already switched off, so %d "
                "passes give 1 rail-off (saw %d)",
                passes, ev_count(0, tst_run_end(), "rail-off"));
    }

    scenario(TST_PF_TRIP_PASS * 3, s_off_low);
    CHECK(!g_fault_off && g_softonoff.api_softoff && g_softoff_rsn == MC_SOFTOFF_API &&
                    ev_count(0, tst_run_end(), "rail-off") == 1,
            "a low rail during an API soft-off is the rail being off, not a "
            "power fault: latch %d, reason %d, %d rail-off(s) (want the soft-off's "
            "one)",
            g_fault_off, g_softoff_rsn, ev_count(0, tst_run_end(), "rail-off"));

#if IMPL_PSU_IS_DCDC_PD
    scenario(TST_PO_TRIP_PASS, s_pout_high);
    CHECK(ev_count(0, tst_run_end(), "rail-off") == 0 && !g_fault_off,
            "%d consecutive output power readings above %d W do NOT trip (%d "
            "rail-off(s), latch %d)",
            POUT_OVER_HIGH_CNT - 1, MAX_POUT_LIMIT_ABNORMAL, ev_count(0, tst_run_end(), "rail-off"),
            g_fault_off);

    scenario(TST_PO_TRIP_PASS + 1, s_pout_high);
    CHECK(ev_count(0, p0(TST_PO_TRIP_PASS), "rail-off") == 0 &&
                    ev_count(p0(TST_PO_TRIP_PASS), tst_run_end(), "rail-off") == 1,
            "the %dth consecutive output power reading above %d W trips exactly "
            "once, on pass %d (%d rail-off(s) before, %d on it)",
            POUT_OVER_HIGH_CNT, MAX_POUT_LIMIT_ABNORMAL, TST_PO_TRIP_PASS,
            ev_count(0, p0(TST_PO_TRIP_PASS), "rail-off"),
            ev_count(p0(TST_PO_TRIP_PASS), tst_run_end(), "rail-off"));
    {
        static const char *const want[]     = { "kill-asic", "rail-off", "fix", "duty" };
        const int                want_val[] = { SIGTERM, 0, 0, 0 };
        int                      s          = ev_first(p0(TST_PO_TRIP_PASS), "state");
        int                      u          = ev_first(p0(TST_PO_TRIP_PASS), "push");
        int                      tp         = tst_ev[p0(TST_PO_TRIP_PASS)].pass;
        CHECK(ev_is(p0(TST_PO_TRIP_PASS), 4, want, want_val) && s >= 0 && tst_ev[s].pass == tp &&
                        tst_ev[s].val == MC_WORK_IDLE && u > s && tst_ev[u].pass == tp &&
                        tst_ev[u].val == MC_SOFTOFF_POWER_FAULT,
                "the over-power trip SIGTERMs asic_miner, cuts the rail and pins "
                "the fan to 0%% in that order, and the same pass pushes "
                "MC_SOFTOFF_POWER_FAULT to core (saw %s)",
                ev_str(p0(TST_PO_TRIP_PASS), tst_run_end()));
    }
    CHECK(g_softoff_rsn == MC_SOFTOFF_POWER_FAULT && g_fault_off == 1,
            "...and latches the same power fault: softoff_rsn %d (want %d), "
            "g_fault_off %d",
            g_softoff_rsn, MC_SOFTOFF_POWER_FAULT, g_fault_off);

    scenario(TST_PO_TRIP_PASS * 4, s_pout_refund);
    CHECK(ev_count(0, tst_run_end(), "rail-off") == 0 && !g_fault_off,
            "runs of %d readings above %d W, each broken by ONE at or below it, "
            "never trip: the counter is a consecutive run (%d rail-off(s))",
            POUT_OVER_HIGH_CNT - 1, MAX_POUT_LIMIT_ABNORMAL,
            ev_count(0, tst_run_end(), "rail-off"));

    scenario(TST_PO_TRIP_PASS * 4, s_pout_edge);
    CHECK(ev_count(0, tst_run_end(), "rail-off") == 0 && !g_fault_off,
            "an output power of exactly %d W never trips: the limit is strict "
            "(%d rail-off(s))",
            MAX_POUT_LIMIT_ABNORMAL, ev_count(0, tst_run_end(), "rail-off"));

    scenario(TST_PO_TRIP_PASS * 4, s_pout_none);
    CHECK(ev_count(0, tst_run_end(), "rail-off") == 0 && !g_fault_off,
            "an unreadable output power (MM_TELEM_POUT_NONE %d) never trips (%d "
            "rail-off(s))",
            MM_TELEM_POUT_NONE, ev_count(0, tst_run_end(), "rail-off"));

    scenario(TST_PO_TRIP_PASS * 4, s_pout_api);
    CHECK(!g_fault_off && g_softonoff.api_softoff && g_softoff_rsn == MC_SOFTOFF_API &&
                    ev_count(0, tst_run_end(), "rail-off") == 1,
            "output power above %d W during an API soft-off is not counted: "
            "latch %d, reason %d, %d rail-off(s) (want the soft-off's one)",
            MAX_POUT_LIMIT_ABNORMAL, g_fault_off, g_softoff_rsn,
            ev_count(0, tst_run_end(), "rail-off"));

    scenario(TST_PO_TRIP_PASS * 4, s_pout_hot);
    CHECK(!g_fault_off && g_softonoff.thermal_until && g_softoff_rsn == MC_SOFTOFF_ASIC_OVERHOT,
            "output power above %d W during a thermal hold is not counted: "
            "latch %d, reason %d (want %d)",
            MAX_POUT_LIMIT_ABNORMAL, g_fault_off, g_softoff_rsn, MC_SOFTOFF_ASIC_OVERHOT);
#endif /* IMPL_PSU_IS_DCDC_PD */

    {
        scenario(TST_PF_TRIP_PASS + 4, s_low_dead);
        uint64_t t_noreap = tst_now;
        scenario(TST_PF_TRIP_PASS + 4, s_low_dead_reap);
        CHECK(g_fault_off && g_softoff_rsn == MC_SOFTOFF_POWER_FAULT,
                "an asic_miner the power fault SIGTERMed before it ever reported "
                "in is reaped as the deliberate stop it is: the reason stays "
                "POWER_FAULT (%d), not HASH_FAULT",
                g_softoff_rsn);
        CHECK(tst_now == t_noreap && ev_count(0, tst_run_end(), "spawn-asic") == 1,
                "...and the reap neither blocks the loop for RESTART_MS (%llu ms "
                "vs %llu ms without the reap) nor respawns (%d spawn-asic)",
                (unsigned long long)tst_now, (unsigned long long)t_noreap,
                ev_count(0, tst_run_end(), "spawn-asic"));
    }

    {
        scenario(TST_PF_TRIP_PASS + 4, s_ok);
        uint64_t t_noreap = tst_now;
        scenario(TST_PF_TRIP_PASS + 4, s_ok_reap);
        CHECK(tst_now == t_noreap,
                "a crashed asic_miner's %u ms restart delay does not stall the "
                "supervised loop (%llu ms vs %llu ms without the reap)",
                RESTART_MS, (unsigned long long)tst_now, (unsigned long long)t_noreap);
    }

    scenario(TST_PF_TRIP_PASS + 4 + TST_RESTART_PASSES, s_ok_reap);
    CHECK(ev_count(0, tst_run_end(), "spawn-asic") == 2 && !g_fault_off,
            "control: with no fault, a reaped asic_miner IS respawned — %d "
            "spawn-asic (want 2: bring-up plus the crash respawn), latch %d",
            ev_count(0, tst_run_end(), "spawn-asic"), g_fault_off);

    scenario(TST_PF_TRIP_PASS + 4, s_low_reap);
    CHECK(g_fault_off == 1 && ev_count(0, tst_run_end(), "spawn-asic") == 1,
            "after a power fault the SAME reap respawns nothing: %d spawn-asic "
            "(want 1, the bring-up one), latch %d — this is the whole point of "
            "g_fault_off and the only thing that stops mm feeding a dead rail a "
            "new child every %u ms",
            ev_count(0, tst_run_end(), "spawn-asic"), g_fault_off, RESTART_MS);

    scenario(TST_PF_KILL_PASS + 2, s_low);
    CHECK(ev_sig(0, tst_run_end(), "kill-asic", SIGTERM) == 1 &&
                    ev_sig(0, tst_run_end(), "kill-asic", SIGKILL) == 1 &&
                    ev_sig_pass("kill-asic", SIGKILL) == TST_PF_KILL_PASS,
            "an asic_miner still unreaped %u ms after the power fault's SIGTERM "
            "is SIGKILLed once, on pass %d (SIGTERM %d, SIGKILL %d on pass %d)",
            CHILD_TERM_MS, TST_PF_KILL_PASS, ev_sig(0, tst_run_end(), "kill-asic", SIGTERM),
            ev_sig(0, tst_run_end(), "kill-asic", SIGKILL), ev_sig_pass("kill-asic", SIGKILL));

    scenario(TST_PF_KILL_PASS + 4, s_low_reap);
    CHECK(ev_sig(0, tst_run_end(), "kill-asic", SIGKILL) == 0,
            "...and one reaped within %u ms is never SIGKILLed (%d SIGKILL)", CHILD_TERM_MS,
            ev_sig(0, tst_run_end(), "kill-asic", SIGKILL));

    scenario(TST_PF_KILL_PASS + 4, s_low_kill_reap);
    CHECK(g_fault_off == 1 && g_softoff_rsn == MC_SOFTOFF_POWER_FAULT &&
                    ev_count(0, tst_run_end(), "spawn-asic") == 1 &&
                    ev_sig(0, tst_run_end(), "kill-asic", SIGKILL) == 1,
            "...and the SIGKILLed child is reaped as the deliberate stop: reason "
            "%d (want %d), latch %d, %d spawn-asic (want 1), %d SIGKILL",
            g_softoff_rsn, MC_SOFTOFF_POWER_FAULT, g_fault_off,
            ev_count(0, tst_run_end(), "spawn-asic"),
            ev_sig(0, tst_run_end(), "kill-asic", SIGKILL));

    {
        pid_t a         = tst_next_pid++;
        tst_pid_asic    = a;
        tst_ev_n        = 0;
        tst_now         = 0;
        g_fault_off     = 0;
        g_softoff_rsn   = MC_SOFTOFF_NONE;
        g_fan_fault_req = 1;
        mm_fan_fault_act(&a);
        mm_asic_term_tick(CHILD_TERM_MS - 1, a);
        int early = ev_sig(0, tst_ev_n, "kill-asic", SIGKILL);
        mm_asic_term_tick(CHILD_TERM_MS, a);
        int due = ev_sig(0, tst_ev_n, "kill-asic", SIGKILL);
        mm_asic_term_tick(CHILD_TERM_MS + CHILD_KILL_MS, a);
        mm_asic_term_tick(3 * CHILD_TERM_MS, a);
        CHECK(g_softoff_rsn == MC_SOFTOFF_FAN_ERR &&
                        ev_sig(0, tst_ev_n, "kill-asic", SIGTERM) == 1 && early == 0 && due == 1 &&
                        ev_sig(0, tst_ev_n, "kill-asic", SIGKILL) == 1,
                "a fan fault escalates the same way: SIGTERM, then one SIGKILL "
                "at %u ms and none before or after (early %d, due %d, total %d)",
                CHILD_TERM_MS, early, due, ev_sig(0, tst_ev_n, "kill-asic", SIGKILL));

        a               = tst_next_pid++;
        tst_pid_asic    = a;
        tst_ev_n        = 0;
        g_fault_off     = 0;
        g_fan_fault_req = 1;
        mm_fan_fault_act(&a);
        mm_asic_term_tick(CHILD_TERM_MS / 2, -1);
        mm_asic_term_tick(CHILD_TERM_MS, a);
        CHECK(ev_sig(0, tst_ev_n, "kill-asic", SIGKILL) == 0,
                "...and a reap before the deadline cancels it (%d SIGKILL)",
                ev_sig(0, tst_ev_n, "kill-asic", SIGKILL));
        g_fault_off   = 0;
        g_softoff_rsn = MC_SOFTOFF_NONE;
    }

    {
        static const char *const want[]     = { "kill-asic", "rail-off", "fix" };
        static const int         want_val[] = { SIGTERM, 0, FAN_DUTY_MIN_PCT };
        pid_t                    a          = tst_next_pid++;
        tst_pid_asic                        = a;
        tst_ev_n                            = 0;
        g_fault_off                         = 0;
        g_softoff_rsn                       = MC_SOFTOFF_NONE;
        mm_aging_fail_act(&a);
        CHECK(tst_ev_n == 0 && !g_fault_off, "no aging failure reported: no soft-off (%d event(s))",
                tst_ev_n);

        tst_aging_failed = 1;
        mm_aging_fail_act(&a);
        CHECK(g_fault_off && g_softoff_rsn == MC_SOFTOFF_CALI_FAILED && ev_is(0, 3, want, want_val),
                "an aging failure soft-offs: reason %d (want %d), "
                "asic_miner stopped, rail off, fan at FAN_DUTY_MIN_PCT %d%% "
                "(saw %s)",
                g_softoff_rsn, MC_SOFTOFF_CALI_FAILED, FAN_DUTY_MIN_PCT, ev_str(0, 3));
        CHECK(softonoff_children_stopped(&g_softonoff, g_fault_off) & SOFTONOFF_CHILD_ASIC,
                "...and the latched aging failure exempts the ASIC leg of the "
                "watchdog gate, so the stopped board is not reset");

        int n            = tst_ev_n;
        tst_aging_failed = 1;
        mm_aging_fail_act(&a);
        CHECK(tst_ev_n == n && g_softoff_rsn == MC_SOFTOFF_CALI_FAILED,
                "...and a second report while already stopped changes nothing "
                "(%d new event(s))",
                tst_ev_n - n);
        mm_asic_term_tick(3 * CHILD_TERM_MS, -1);
        g_fault_off   = 0;
        g_softoff_rsn = MC_SOFTOFF_NONE;
    }

    scenario(TST_HF_REAP_PASS + 4 + TST_RESTART_PASSES, s_was_live);
    CHECK(g_softoff_rsn == MC_SOFTOFF_NONE && !g_fault_off &&
                    ev_count(0, tst_run_end(), "spawn-asic") == 2,
            "control: an asic_miner that HAD reported in takes the ordinary crash "
            "path when it exits — respawned (%d spawn-asic, want 2), no reason "
            "latched (%d) and no inhibit (%d)",
            ev_count(0, tst_run_end(), "spawn-asic"), g_softoff_rsn, g_fault_off);

    scenario(TST_HF_REAP_PASS + 4, s_never_live);
    CHECK(g_softoff_rsn == MC_SOFTOFF_HASH_FAULT && g_fault_off == 1 &&
                    ev_count(0, tst_run_end(), "spawn-asic") == 1,
            "an asic_miner that exits having NEVER reached asic_live is a HASH "
            "FAULT: reason %d (want %d), inhibit %d, and it is not respawned (%d "
            "spawn-asic, want 1). g_asic_ever_live is what separates this from the "
            "control above",
            g_softoff_rsn, MC_SOFTOFF_HASH_FAULT, g_fault_off,
            ev_count(0, tst_run_end(), "spawn-asic"));

    {
        static const char *const want[] = { "rail-off", "fix", "duty" };
        static const int want_val[] = { 0, FAN_DUTY_HASH_FAULT_PCT, FAN_DUTY_HASH_FAULT_PCT * 10 };
        int              r          = ev_first(0, "rail-off");
        CHECK(ev_count(0, tst_run_end(), "rail-off") == 1 &&
                        ev_count(0, tst_run_end(), "kill-asic") == 0 && r >= 0 &&
                        ev_is(r, 3, want, want_val),
                "...and like the power fault it cuts the rail, then pins the fan "
                "to FAN_DUTY_HASH_FAULT_PCT (%d%%), killing nothing — the child "
                "is already gone (%d rail-off, %d kill-asic, saw %s)",
                FAN_DUTY_HASH_FAULT_PCT, ev_count(0, tst_run_end(), "rail-off"),
                ev_count(0, tst_run_end(), "kill-asic"), ev_str(r < 0 ? 0 : r, r < 0 ? 0 : r + 3));
        CHECK(softonoff_children_stopped(&g_softonoff, g_fault_off) & SOFTONOFF_CHILD_ASIC,
                "...and the latched HASH FAULT still exempts the ASIC leg of the "
                "watchdog gate, so the parked board is not reset");
    }

    {
        int r = ev_first(0, "rail-off");
        int u = r < 0 ? -1 : ev_first(r, "push");
        CHECK(u >= 0 && u < tst_run_end() && tst_ev[u].val == MC_SOFTOFF_HASH_FAULT &&
                        tst_ev[u].now == tst_ev[r].now,
                "...and the reason reaches core on the push right after the "
                "reap, with no loop wait in between (push %d, reason %d)",
                u, u < 0 ? -1 : tst_ev[u].val);
    }

    tst_exec_fail_n = (int)ASIC_EXEC_RETRY_MAX;
    scenario(TST_HF_REAP_PASS + (tst_exec_fail_n + 1) * (TST_RESTART_PASSES + 1), s_exec_failed);
    CHECK(g_softoff_rsn == MC_SOFTOFF_NONE && !g_fault_off &&
                    ev_count(0, tst_run_end(), "spawn-asic") == 1 + tst_exec_fail_n,
            "an asic_miner that exits 127 (chdir/exec failed in the child) is "
            "retried every %u ms, up to %u times in a row, without latching: "
            "reason %d, inhibit %d (%d spawn-asic, want %d)",
            RESTART_MS, ASIC_EXEC_RETRY_MAX, g_softoff_rsn, g_fault_off,
            ev_count(0, tst_run_end(), "spawn-asic"), 1 + tst_exec_fail_n);

    tst_exec_fail_n = (int)ASIC_EXEC_RETRY_MAX + 1;
    scenario(TST_HF_REAP_PASS + (tst_exec_fail_n + 1) * (TST_RESTART_PASSES + 1), s_exec_failed);
    CHECK(g_softoff_rsn == MC_SOFTOFF_HASH_FAULT && g_fault_off == 1 &&
                    ev_count(0, tst_run_end(), "spawn-asic") == tst_exec_fail_n,
            "...and the exec failure after the last retry latches HASH FAULT on "
            "the never-live path: reason %d (want %d), inhibit %d, no further "
            "respawn (%d spawn-asic, want %d)",
            g_softoff_rsn, MC_SOFTOFF_HASH_FAULT, g_fault_off,
            ev_count(0, tst_run_end(), "spawn-asic"), tst_exec_fail_n);
    tst_exec_fail_n = 0;
    {
        pid_t asic = -1;

        g_asic_exec_fails = ASIC_EXEC_RETRY_MAX;
        mm_api_act(SOFTONOFF_API_ON, 1, &asic);
        CHECK(g_asic_exec_fails == 0,
                "a soft-on that clears the latch also resets the exec-failure "
                "count, so the next start gets its retries again (count %u)",
                g_asic_exec_fails);
    }

    scenario(TST_PF_TRIP_PASS + 4, s_low_api);
    CHECK(g_fault_off == 0,
            "an API soft-on clears the respawn inhibit a power fault set (latch "
            "%d) — without this line the phone app can never restart a faulted "
            "miner",
            g_fault_off);
    CHECK(g_softoff_rsn == MC_SOFTOFF_NONE,
            "...and the softoff REASON is cleared with it (%d, want %d). Were it to "
            "survive, :4028 would report SoftOFF[1] from the first fault "
            "until reboot and the web dashboard's card would stay red on a miner "
            "that is mining again",
            g_softoff_rsn, MC_SOFTOFF_NONE);
    CHECK(ev_last_val("push") == MC_SOFTOFF_NONE,
            "...and the cleared reason REACHES core on the next push (last pushed "
            "%d, want %d), so the wire field follows the latch rather than the "
            "static keeping its old value",
            ev_last_val("push"), MC_SOFTOFF_NONE);

#if defined(BOARD_X3_H)
    CHECK(POWER_VALID_VOLT_MIN == 8000,
            "this product's PD input threshold is %d in TRUE millivolts", POWER_VALID_VOLT_MIN);
#elif defined(BOARD_NANO3S_H)
    CHECK(POWER_VALID_VOLT_MIN == 8000,
            "this product's PD input threshold is %d in TRUE millivolts", POWER_VALID_VOLT_MIN);
#else
#error "no PD input threshold golden for this board"
#endif

    if (FAN_BELOW_STALL_CNT > 0) {
        fan_scenario(2 * FAN_BELOW_STALL_CNT - 2, 0, 60);
        CHECK(!g_fault_off && g_softoff_rsn == MC_SOFTOFF_NONE &&
                        g_fan_health_st.bad_run == FAN_BELOW_STALL_CNT - 1,
                "a stalled fan commanded to 60%% is not a fault on the 2 s sample "
                "before the %dth (run %d, latch %d, reason %d)",
                FAN_BELOW_STALL_CNT, g_fan_health_st.bad_run, g_fault_off, g_softoff_rsn);
        fan_scenario(2 * FAN_BELOW_STALL_CNT - 1, 0, 60);
        CHECK(g_fault_off && g_softoff_rsn == MC_SOFTOFF_FAN_ERR,
                "...and the %dth consecutive sample, through ctl_tick, latches "
                "MC_SOFTOFF_FAN_ERR (latch %d, reason %d)",
                FAN_BELOW_STALL_CNT, g_fault_off, g_softoff_rsn);
    }
    fan_scenario(4 * FAN_BELOW_STALL_CNT + 4, FAN_RPM_NONE, 60);
    CHECK(!g_fault_off && g_softoff_rsn == MC_SOFTOFF_NONE,
            "an unreadable tach (FAN_RPM_NONE) never latches a fan fault (latch %d, "
            "reason %d)",
            g_fault_off, g_softoff_rsn);
    fan_scenario(4 * FAN_BELOW_STALL_CNT + 4, 0, 0);
    CHECK(!g_fault_off && g_softoff_rsn == MC_SOFTOFF_NONE,
            "a fan reading 0 RPM while commanded to 0%% is not judged (latch %d, "
            "reason %d)",
            g_fault_off, g_softoff_rsn);

    return tst_done();
}
