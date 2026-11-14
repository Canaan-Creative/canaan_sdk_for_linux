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
#include "mm_spawn.h"
#include "tst_board_expect.h"
#include "framework.h"
#include "fan_dev.h"
#include "mm_telem.h"
#include "osif.h"
#include "psu_ops.h"
#include "wdt_dev.h"
#include "fan_pid.h"
#include "thermal.h"
#include "softonoff.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* asic_miner spawn delay, s: none */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_MAX_PASS  64                 /* supervisor passes a scenario may run */
#define TST_SLEEP_CAP (TST_MAX_PASS * 8) /* osif_sleep_ms() calls before g_run is forced to 0 */
#define TST_TICK_MS   1000u              /* fake clock per ctl_tick() event, ms */
#define TST_OFF_PASS  2                  /* pass that requests the API soft-off */
#define TST_ON_PASS   5                  /* pass that requests the API soft-on */
/* Pass on which the soft-off reaches the SIGKILL escalation */
#define TST_KILL_PASS    (TST_OFF_PASS + (int)(CHILD_TERM_MS / TST_TICK_MS))
#define TST_KILL_ON_PASS (TST_KILL_PASS + 2) /* soft-on pass after the SIGKILL */
#define TST_RBF_PASS     2                   /* pass that marks a reboot as started and failed */
/* Passes covering WDT_CORE_WEDGE_MS plus three */
#define TST_WEDGE_PASSES ((int)(WDT_CORE_WEDGE_MS / TST_TICK_MS) + 3)

#define TST_FAN_DUTY_EV /* log each fan duty set as a "duty" event */

/* Calls in the included sources, routed to the tst_* fakes below */
#define fork()                             tst_fork()
#define system(c)                          tst_system(c)
#define kill(p, s)                         tst_kill((p), (s))
#define waitpid(p, s, o)                   tst_waitpid((p), (s), (o))
#define stat(p, b)                         tst_stat((p), (b))
#define fan_pid_set_fixed(s, d)            tst_fan_fixed((s), (d))
#define ctl_tick(n)                        tst_ctl_tick(n)
#define osif_log(...)                      tst_log(__VA_ARGS__)
#define thermal_telem_check(a, b, c, d, e) tst_telem_check((a), (b), (c), (d), (e))

pid_t                        tst_fork(void);
int                          tst_system(const char *cmd);
int                          tst_kill(pid_t pid, int sig);
pid_t                        tst_waitpid(pid_t pid, int *status, int options);
int                          tst_stat(const char *path, struct stat *st);
static void                  tst_fan_fixed(fan_pid_state_t *st, int duty_pct);
static int                   tst_ctl_tick(uint64_t now_ms);
static void                  tst_log(osif_loglvl_t lvl, const char *fmt, ...);
static thermal_telem_verdict tst_telem_check(thermal_telem_state_t *st, int asic_live,
        int temp_valid, uint64_t now_ms, uint32_t grace_ms);

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
#include "mm_miner_tu.h"
#undef thermal_telem_check
#undef osif_log
#undef ctl_tick
#undef fan_pid_set_fixed
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork

_Static_assert(FAN_DUTY_API_SOFTOFF_PCT > 0,
        "FAN_DUTY_API_SOFTOFF_PCT must be a real duty: 0 or negative would be "
        "indistinguishable from the fan-released sentinel (-1) that the "
        "soft-ON pins, and the soft-off pin and soft-on release checks would pass "
        "vacuously");
_Static_assert(FAN_DUTY_API_SOFTOFF_PCT != FAN_DUTY_SOFTOFF_PCT,
        "the API soft-off pin and the thermal soft-off overheat pin are the same duty "
        "on this descriptor — the assertion that the API soft-off pin is not the "
        "overheat pin would be vacuous");
_Static_assert(CHILD_TERM_MS % TST_TICK_MS == 0,
        "the SIGKILL escalation must land on a whole supervised pass");
_Static_assert(TST_WEDGE_PASSES <= TST_MAX_PASS,
        "the wedge scenario must fit the pass budget, or the wedge check would end "
        "before WDT_CORE_WEDGE_MS and pass vacuously");

typedef void (*tst_script_fn)(int pass); /* per-pass scenario hook, pass from 0 */

static int      tst_net_up; /* nonzero reports the network up with an IP */
static uint64_t tst_now;    /* fake monotonic clock, ms */
/* Passes run and wanted; osif_sleep_ms() calls; TST_SLEEP_CAP overrun flag */
static int           tst_pass, tst_want, tst_sleeps, tst_runaway;
static int           tst_pass_ev0[TST_MAX_PASS + 2]; /* first event index of each pass */
static int           tst_wp_calls;                   /* waitpid() call count */
static tst_script_fn tst_script;
/* mm_telem_get() poll_fresh, asic_live and in_work flags; tmax and tavg, degC */
static int   tst_fresh, tst_asic_live, tst_in_work, tst_tmax;
static int   tst_join_rc;                  /* mm_children_stop() result */
static pid_t tst_next_pid = 4000;          /* next fake fork() PID */
static pid_t tst_pid_core, tst_pid_asic;   /* last spawned core_miner and asic_miner PIDs */
static int   tst_off_rc, tst_on_rc;        /* nonzero leaves rail off / rail on unconfirmed */
static int   tst_reap_asic, tst_reap_core; /* one-shot: next waitpid() reaps that child */
static char  tst_last_log[512];
/*
 * Log lines; counts of the over-temperature-ignored, soft-off-recorded,
 * reboot-failed-stopped, reboot-in-progress-refused and API soft-on lines.
 */
static int tst_log_lines, tst_ignored_reports, tst_held_reports, tst_rbf_stopped_logs;
static int tst_rbf_refused_logs, tst_soft_on_logs;
static int tst_telem_reset; /* one-shot: next thermal_telem_check() clears its state */

#include "tst_events.h"
#include "tst_fan_dev.h"

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

static int p0(int p)
{
    return tst_pass_ev0[p < TST_MAX_PASS ? p : TST_MAX_PASS - 1];
}
static int p1(int p)
{
    return p0(p + 1);
}

static int tst_run_end(void)
{
    int j = ev_first(0, "join");
    return j < 0 ? tst_ev_n : j;
}

static int api0(int p)
{
    for (int i = p0(p); i < p1(p) && i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, "tick"))
            return i + 1;
    return -1;
}

uint64_t osif_now_ms(void)
{
    return tst_now;
}
void osif_sleep_ms(uint32_t ms)
{
    tst_now += ms;
    if (++tst_sleeps <= TST_SLEEP_CAP)
        return;
    tst_runaway = 1;
    g_run       = 0;
    if (tst_sleeps > 2 * TST_SLEEP_CAP) {
        fprintf(stderr,
                "  FAIL: the supervisor loop does not exit on a cleared "
                "g_run — still running after %d sleeps; aborting\n",
                tst_sleeps);
        _exit(1);
    }
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
    out->ps_vout_mv = POWER_VALID_VOLT_MIN;
    out->asic_live  = tst_asic_live;
    out->in_work    = tst_in_work;
    out->tmax       = tst_tmax;
    out->tavg       = tst_tmax;
    out->hb_count   = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++)
        out->hb_outlet[b] = out->hb_inlet[b] = 60;
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

int tst_children_stop(const pid_t *pids, int n, unsigned term_ms, unsigned kill_ms)
{
    (void)pids;
    (void)n;
    (void)term_ms;
    (void)kill_ms;
    ev("join", tst_join_rc);
    return tst_join_rc;
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
    ev("rail-on", tst_on_rc);
    return tst_on_rc ? HAL_PSU_RAIL_E_UNCONFIRMED : HAL_PSU_RAIL_OK;
}
int board_power_rail_off(void)
{
    ev("rail-off", tst_off_rc);
    return tst_off_rc ? HAL_PSU_RAIL_E_UNCONFIRMED : HAL_PSU_RAIL_OK;
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
    const char *who = pid == tst_pid_core ? "kill-core" :
            pid == tst_pid_asic           ? "kill-asic" :
                                            "kill-?";
    ev(who, sig);
    return 0;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    tst_wp_calls++;
    if (status)
        *status = 0;
    if (pid > 0 && pid == tst_pid_asic && tst_reap_asic) {
        tst_reap_asic = 0;
        ev("reap-asic", 0);
        return pid;
    }
    if (pid > 0 && pid == tst_pid_core && tst_reap_core) {
        tst_reap_core = 0;
        ev("reap-core", 0);
        return pid;
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
static int tst_ctl_tick(uint64_t now_ms)
{
    int r = ctl_tick(now_ms);
    ev("tick", (int)(now_ms / TST_TICK_MS));
    return r;
}
static void tst_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(tst_last_log, sizeof tst_last_log, fmt, ap);
    va_end(ap);
    tst_log_lines++;
    if (strstr(tst_last_log, "ignored — over-temperature"))
        tst_ignored_reports++;
    if (!strcmp(tst_last_log,
                "soft-off by API recorded — mining stays stopped "
                "after the over-temperature cool-down"))
        tst_held_reports++;
    if (!strcmp(tst_last_log, "reboot failed — mining stays stopped"))
        tst_rbf_stopped_logs++;
    if (strstr(tst_last_log, "not started — reboot in progress"))
        tst_rbf_refused_logs++;
    if (!strcmp(tst_last_log, "soft-on by API — mining restarts"))
        tst_soft_on_logs++;
}
static thermal_telem_verdict tst_telem_check(thermal_telem_state_t *st, int asic_live,
        int temp_valid, uint64_t now_ms, uint32_t grace_ms)
{
    if (tst_telem_reset) {
        memset(st, 0, sizeof *st);
        tst_telem_reset = 0;
    }
    return thermal_telem_check(st, asic_live, temp_valid, now_ms, grace_ms);
}

static void scenario(int passes, tst_script_fn s)
{
    tst_now      = 0;
    tst_pass     = 0;
    tst_want     = passes;
    tst_script   = s;
    tst_sleeps   = 0;
    tst_runaway  = 0;
    tst_ev_n     = 0;
    tst_wp_calls = 0;
    tst_pid_core = tst_pid_asic = 0;
    tst_duty_x10                = 0;
    tst_reap_asic = tst_reap_core = 0;
    tst_log_lines = tst_ignored_reports = tst_held_reports = tst_rbf_stopped_logs = 0;
    tst_rbf_refused_logs = tst_soft_on_logs = 0;
    tst_telem_reset                         = 1;
    memset(tst_pass_ev0, 0, sizeof tst_pass_ev0);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty    = -1;
    g_overheat_req         = 0;
    g_api_req              = 0;
    g_fault_off            = 0;
    g_rail_off_unconfirmed = 0;
    tst_fresh              = 1;
    tst_asic_live          = 1;
    tst_in_work            = 1;
    tst_tmax               = 60;
    g_run                  = 1;
    tst_net_raise();
    mm_miner_main_unused();
}

static void s_off(int p)
{
    if (p == TST_OFF_PASS)
        g_api_req = -1;
}
static void s_off_on(int p)
{
    if (p == TST_OFF_PASS)
        g_api_req = -1;
    if (p == TST_OFF_PASS)
        tst_reap_asic = 1;
    if (p == TST_ON_PASS)
        g_api_req = 1;
}
static void s_off_on_noreap(int p)
{
    if (p == TST_OFF_PASS)
        g_api_req = -1;
    if (p == TST_ON_PASS)
        g_api_req = 1;
    if (p == TST_ON_PASS + 1)
        tst_reap_asic = 1;
}
static void s_off_twice(int p)
{
    if (p == TST_OFF_PASS) {
        g_api_req     = -1;
        tst_reap_asic = 1;
    }
    if (p == TST_ON_PASS)
        g_api_req = -1;
}
static void s_off_stuck_on(int p)
{
    if (p == TST_OFF_PASS)
        g_api_req = -1;
    if (p == TST_KILL_PASS)
        tst_reap_asic = 1;
    if (p == TST_KILL_ON_PASS)
        g_api_req = 1;
}
static int ev_sig(int from, int to, const char *what, int sig)
{
    int n = 0;
    for (int i = from; i < to && i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, what) && tst_ev[i].val == sig)
            n++;
    return n;
}
static int ev_sig_first(const char *what, int sig)
{
    for (int i = 0; i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, what) && tst_ev[i].val == sig)
            return i;
    return -1;
}
static void s_on_while_mining(int p)
{
    if (p == TST_OFF_PASS)
        g_api_req = 1;
}
static void s_hold_then_press(int p)
{
    if (p == 1)
        g_overheat_req = 1;
    if (p == TST_OFF_PASS)
        g_api_req = -1;
    if (p == TST_ON_PASS)
        g_api_req = 1;
}

static void s_reboot_failed(int p)
{
    if (p == 0)
        tst_asic_live = 0;
    if (p == TST_RBF_PASS) {
        atomic_store(&g_rebooting, 1);
        atomic_store(&g_reboot_failed, 1);
    }
    if (p == TST_RBF_PASS + 1)
        tst_reap_asic = 1;
}
static void s_reboot_failed_exit(int p)
{
    if (p == TST_RBF_PASS) {
        g_asic_ever_live = 1;
        atomic_store(&g_rebooting, 1);
        atomic_store(&g_reboot_failed, 1);
    }
    if (p == TST_RBF_PASS + 1)
        tst_reap_asic = 1;
}

int main(void)
{
    printf("descriptor %s: %d board(s), API soft-off pin %d%%, enum pin %d%%, "
           "softoff pin %d%%, hold %u ms, restart backoff %u ms\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, FAN_DUTY_API_SOFTOFF_PCT, FAN_DUTY_ENUM_PCT,
            FAN_DUTY_SOFTOFF_PCT, OVERHOT_HOLD_MS, RESTART_MS);

    scenario(TST_OFF_PASS + 2, s_off);
    CHECK(!tst_runaway, "the loop exits on a cleared g_run (%d sleeps, cap %d)", tst_sleeps,
            TST_SLEEP_CAP);
    {
        static const char *const up[] = { "rail-on", "spawn-core", "spawn-asic" };
        CHECK(ev_is(0, 3, up, NULL) && p0(0) >= 3 && ev_count(0, tst_run_end(), "spawn-asic") == 1,
                "bring-up: the rail is energized before either child is spawned, "
                "and with no start gate both children are spawned there, before "
                "the first supervised pass (pass 0 starts at act %d; saw %s)",
                p0(0), ev_str(0, 5));
    }

    int a0 = api0(TST_OFF_PASS), a1 = p1(TST_OFF_PASS);
    {
        static const char *const off[] = { "kill-asic", "rail-off", "fix", "duty" };
        const int offv[] = { SIGTERM, 0, FAN_DUTY_API_SOFTOFF_PCT, FAN_DUTY_API_SOFTOFF_PCT * 10 };
        CHECK(a0 >= 0 && ev_is(a0, 4, off, offv) && a0 + 4 == a1,
                "SOFTONOFF_API_OFF actuates exactly these four acts, in this order, and "
                "NOTHING else in the pass: SIGTERM asic_miner, rail OFF "
                "(in-process), then the fan pinned at FAN_DUTY_API_SOFTOFF_PCT (%d%%) through "
                "BOTH the PID override and the HAL (saw %s)",
                FAN_DUTY_API_SOFTOFF_PCT, ev_str(a0 < 0 ? p0(TST_OFF_PASS) : a0, a1));
    }

    CHECK(ev_count(a0, a1, "kill-core") == 0 &&
                    ev_count(p0(TST_OFF_PASS), tst_run_end(), "spawn-core") == 0,
            "...and core_miner is NEITHER signalled NOR restarted — :4028 stays up "
            "for the whole soft-off, because the soft-ON has to arrive over it and "
            "this board has no RTC to fall back on (saw %d kills, %d respawns)",
            ev_count(a0, a1, "kill-core"), ev_count(p0(TST_OFF_PASS), tst_run_end(), "spawn-core"));

    CHECK(ev_count(a0, a1, "join") == 0,
            "...and the rail is cut with NO join: the SIGTERM is fire-and-forget, "
            "so the rail-off can run while asic_miner is still driving the ASIC UART. "
            "Same shape as the thermal soft-off and unlike the shutdown tail (:1448), "
            "which joins deliberately and says why. Reported, not changed");

    {
        int r = ev_first(a0, "rail-off"), f = ev_first(a0, "fix");
        CHECK(r >= 0 && f > r,
                "...and the fan pin is the LAST act, behind the blocking "
                "rail-off — the fork is gone but the call is synchronous, so the "
                "soft-off duty still waits for it (rail-off at act %d, fan pin at act %d "
                "of the block)",
                r - a0, f - a0);
    }

    CHECK(g_api_req == 0,
            "g_api_req is CONSUMED before the verdict is even computed (:1359), so "
            "one press can only ever act once (flag %d after the run)",
            g_api_req);

    CHECK(g_softonoff.api_softoff == 1,
            "...and the soft-off LATCHES: api_softoff stays set for the rest of the run, "
            "which is what keeps the ASIC child from being respawned by the reap "
            "path and the dog fed through the soft-off (latch %d)",
            g_softonoff.api_softoff);

    CHECK(g_softoff_rsn == MC_SOFTOFF_API,
            "...and records the API soft-off reason SoftOFF[%d] (saw %d)", MC_SOFTOFF_API,
            g_softoff_rsn);

    CHECK(ev_count(a1, tst_run_end(), "rail-off") == 0 &&
                    ev_count(a1, tst_run_end(), "kill-asic") == 0 &&
                    ev_count(a1, tst_run_end(), "fix") == 0,
            "...and nothing is re-cut, re-signalled or re-pinned on later passes: "
            "the soft-off is a latch, not a per-iteration action (%d further rail "
            "cuts, %d kills, %d pins)",
            ev_count(a1, tst_run_end(), "rail-off"), ev_count(a1, tst_run_end(), "kill-asic"),
            ev_count(a1, tst_run_end(), "fix"));

    scenario(TST_ON_PASS + 2, s_off_on);
    int b0 = api0(TST_ON_PASS), b1 = p1(TST_ON_PASS);
    {
        static const char *const on[] = { "fix", "rail-on", "spawn-asic" };
        CHECK(b0 >= 0 && ev_is(b0, 3, on, NULL) && tst_ev[b0].val == -1 && b0 + 3 == b1,
                "SOFTONOFF_API_ON actuates exactly these three acts, in this order, and "
                "nothing else: release the fan to the PID (fix=-1), re-enable the "
                "rail, THEN respawn asic_miner — so the rail is hot before the "
                "child that drives the ASIC UART exists. Note core_miner is NOT "
                "respawned here, unlike the thermal RESUME, because it was never "
                "stopped (saw %s)",
                ev_str(b0 < 0 ? p0(TST_ON_PASS) : b0, b1));
        CHECK(tst_soft_on_logs == 1, "...and logs that mining restarts (%d log(s))",
                tst_soft_on_logs);
    }
    CHECK(g_softonoff.api_softoff == 0,
            "...and the latch is released, so the reap path treats the next ASIC "
            "exit as a crash again (latch %d)",
            g_softonoff.api_softoff);
    CHECK(g_softoff_rsn == MC_SOFTOFF_NONE, "...and the soft-on clears the reason (saw %d)",
            g_softoff_rsn);

    scenario(TST_ON_PASS + 6, s_off_on_noreap);
    {
        int                      c0 = api0(TST_ON_PASS), c1 = p1(TST_ON_PASS);
        static const char *const on[] = { "fix", "rail-on" };
        CHECK(c0 >= 0 && ev_is(c0, 2, on, NULL) && tst_ev[c0].val == -1 && c0 + 2 == c1,
                "a soft-ON that arrives BEFORE the SIGTERMed child has been reaped "
                "respawns NOTHING: `if (asic <= 0)` reads the pid variable, which "
                "the reap branch clears, not the child's liveness — so the fan is "
                "released and the rail comes up over a chain with no driver (saw "
                "%s)",
                ev_str(c0 < 0 ? p0(TST_ON_PASS) : c0, c1));

        int      rp    = ev_first(c1, "reap-asic");
        int      sp    = ev_first(rp < 0 ? c1 : rp, "spawn-asic");
        uint64_t press = tst_ev[c0 >= 0 ? c0 : c1].now;
        CHECK(rp >= 0 && sp > rp && tst_ev[sp].now == tst_ev[rp].now + RESTART_MS,
                "...and it recovers through the CRASH path instead: the reap sees "
                "api_softoff already clear, so softonoff_intentional_exit answers 0, the "
                "loop logs an unexpected exit and respawns %u ms later, after the "
                "RESTART_MS backoff. The miner does come back — %llu ms after the "
                "user's press rather than on it, and by a route this block did not "
                "intend. Reported, not changed",
                RESTART_MS, (unsigned long long)(sp >= 0 ? tst_ev[sp].now - press : 0));
    }

    scenario(TST_OFF_PASS + 2, s_off);
    int ok_lines = tst_log_lines;
    tst_off_rc   = 1;
    tst_join_rc  = 1;
    scenario(TST_OFF_PASS + 2, s_off);
    tst_off_rc  = 0;
    tst_join_rc = 0;
    CHECK(ok_lines > 0 && tst_log_lines == ok_lines,
            "a failed API soft-off rail-off and a failed shutdown child join add "
            "no supervisor log line — their owners report them (%d line(s) with "
            "both failing, %d with both succeeding)",
            tst_log_lines, ok_lines);

    tst_on_rc = 1;
    scenario(TST_ON_PASS + 2, s_off_on);
    tst_on_rc = 0;
    {
        int d0 = api0(TST_ON_PASS), d1 = p1(TST_ON_PASS);
        CHECK(g_fault_off && g_softonoff.api_softoff &&
                        ev_first(d0 < 0 ? p0(TST_ON_PASS) : d0, "spawn-asic") < 0,
                "nano3s failed rail enable remains parked without spawning ASIC: %s",
                ev_str(d0 < 0 ? p0(TST_ON_PASS) : d0, d1));
    }

    scenario(TST_ON_PASS + 2, s_off_twice);
    {
        int e0 = api0(TST_ON_PASS), e1 = p1(TST_ON_PASS);
        CHECK(e0 >= 0 && e0 == e1 && g_api_req == 0 && g_softonoff.api_softoff == 1,
                "SOFTONOFF_NONE: a soft-off pressed while ALREADY in soft-off actuates "
                "nothing at all — no second SIGTERM, no second rail cut, no "
                "re-pin — and still eats the request (%d act(s) in the block, "
                "flag %d, latch %d)",
                e1 - e0, g_api_req, g_softonoff.api_softoff);
    }

    scenario(TST_OFF_PASS + 2, s_on_while_mining);
    {
        int f0 = api0(TST_OFF_PASS), f1 = p1(TST_OFF_PASS);
        CHECK(f0 >= 0 && f0 == f1 && g_softonoff.api_softoff == 0,
                "...and symmetrically, a soft-ON pressed while already mining is "
                "also SOFTONOFF_NONE: no spurious rail command and no second asic_miner "
                "(%d act(s) in the block, latch %d)",
                f1 - f0, g_softonoff.api_softoff);
    }

    scenario(TST_ON_PASS + 2, s_hold_then_press);
    {
        int g0 = api0(TST_OFF_PASS), g1 = p1(TST_OFF_PASS);
        int h0 = api0(TST_ON_PASS), h1 = p1(TST_ON_PASS);
        CHECK(g0 >= 0 && g0 == g1 && h0 >= 0 && h0 == h1 && tst_held_reports == 1 &&
                        tst_ignored_reports == 1 && g_softonoff.api_softoff == 1,
                "while a thermal hold owns the rail, a soft-off is latched "
                "(SOFTONOFF_API_OFF_HELD) and a soft-on dropped "
                "(SOFTONOFF_API_IGNORED), neither actuating — %d act(s) off / %d "
                "on, %d recorded and %d ignored line(s), latch %d, so when the "
                "hold expires the verdict is TO_SOFTOFF",
                g1 - g0, h1 - h0, tst_held_reports, tst_ignored_reports, g_softonoff.api_softoff);
    }

    scenario(TST_OFF_PASS + 2, s_off);
    {
        int t = -1;
        for (int i = p0(TST_OFF_PASS); i < p1(TST_OFF_PASS); i++)
            if (!strcmp(tst_ev[i].what, "tick")) {
                t = i;
                break;
            }
        int k = ev_first(p0(TST_OFF_PASS), "kill-asic");
        CHECK(t >= 0 && k > t,
                "the API block runs AFTER ctl_tick (marker at act %d, the block's "
                "first act at %d), which is what lets soft_sched_rule consume a due "
                "schedule and this block act on it in the SAME iteration. Move it "
                "above the tick and a schedule read one iteration late is spent but "
                "unacted — the exact failure softonoff_sched.h warns about",
                t - p0(TST_OFF_PASS), k - p0(TST_OFF_PASS));
    }

    scenario(TST_ON_PASS + 3, s_off_on);
    {
        int i0 = api0(TST_ON_PASS), i1 = p1(TST_ON_PASS);
        int pin_same = 0;
        for (int i = i0; i >= 0 && i < i1; i++)
            if (!strcmp(tst_ev[i].what, "fix") && tst_ev[i].val == FAN_DUTY_ENUM_PCT)
                pin_same = 1;
        CHECK(!pin_same,
                "the soft-on pass ends with the fan RELEASED on both boards: the "
                "enum pin cannot land in the same iteration, because spawn_asic "
                "runs below ctl_tick and the enum block that actuates the pin runs "
                "above it (saw %s)",
                ev_str(i0 < 0 ? p0(TST_ON_PASS) : i0, i1));

        int j0 = p0(TST_ON_PASS + 1), j1 = p1(TST_ON_PASS + 1);
        int pin_next = 0;
        for (int i = j0; i < j1 && i < tst_ev_n; i++)
            if (!strcmp(tst_ev[i].what, "fix") && tst_ev[i].val == FAN_DUTY_ENUM_PCT)
                pin_next = 1;
        CHECK(pin_next == (FAN_DUTY_ENUM_PCT > 0),
                "...and it lands on the NEXT one where the product has an enum pin "
                "at all: FAN_DUTY_ENUM_PCT is %d%% here, so the pin %s on pass %d "
                "(saw %s). One iteration — up to %u ms — of PID-controlled fan on "
                "a chain that is re-enumerating",
                FAN_DUTY_ENUM_PCT,
                FAN_DUTY_ENUM_PCT > 0 ? "DOES appear" :
                                        "never "
                                        "appears",
                TST_ON_PASS + 1, ev_str(j0, j1), TST_TICK_MS);
    }

    scenario(TST_OFF_PASS + 2, s_off);
    {
        int p = ev_first(api0(TST_OFF_PASS), "fix");
        CHECK(p >= 0 && tst_ev[p].val == FAN_DUTY_API_SOFTOFF_PCT &&
                        tst_ev[p].val != FAN_DUTY_SOFTOFF_PCT,
                "the soft-off pins FAN_DUTY_API_SOFTOFF_PCT (%d%%) and NOT the thermal soft-off's "
                "overheat pin (%d%%): a user soft-off is the fan policy's \"min for other\" "
                "(pinned %d)",
                FAN_DUTY_API_SOFTOFF_PCT, FAN_DUTY_SOFTOFF_PCT, p >= 0 ? tst_ev[p].val : -999);
    }

    scenario(TST_WEDGE_PASSES, s_off);
    {
        int n = 0, first = -1;
        for (int i = 0; i < tst_run_end(); i++)
            if (!strcmp(tst_ev[i].what, "kill-core") && tst_ev[i].val == SIGKILL) {
                if (first < 0)
                    first = i;
                n++;
            }
        CHECK(first >= 0 && n == 1 && tst_ev[first].now == (uint64_t)WDT_CORE_WEDGE_MS,
                "a core_miner silent through a softoff is SIGKILLed on the tick "
                "the silence reaches WDT_CORE_WEDGE_MS (%u ms): %d SIGKILL(s), the "
                "first at %llu ms. The softoff exempts what it stopped — "
                "asic_miner — and nothing else, so the supervisor still respawns a "
                "wedged core_miner and :4028 comes back",
                WDT_CORE_WEDGE_MS, n, first >= 0 ? (unsigned long long)tst_ev[first].now : 0ull);

        CHECK(ev_sig(0, tst_run_end(), "kill-asic", SIGTERM) == 1 &&
                        ev_sig(0, tst_run_end(), "kill-asic", SIGKILL) == 1 &&
                        ev_count(0, tst_run_end(), "kill-asic") == 2,
                "...and the wedge touches core_miner ONLY: the kill-asic in the "
                "run are the softoff's own SIGTERM and its one SIGKILL "
                "escalation, %u ms of softoff later (saw %d SIGTERM, %d SIGKILL)",
                TST_WEDGE_PASSES * TST_TICK_MS, ev_sig(0, tst_run_end(), "kill-asic", SIGTERM),
                ev_sig(0, tst_run_end(), "kill-asic", SIGKILL));
    }

    scenario(TST_KILL_ON_PASS + 2, s_off_stuck_on);
    {
        int t = ev_sig_first("kill-asic", SIGTERM);
        int k = ev_sig_first("kill-asic", SIGKILL);
        CHECK(t >= 0 && k > t && tst_ev[k].now == tst_ev[t].now + CHILD_TERM_MS &&
                        ev_sig(0, tst_run_end(), "kill-asic", SIGKILL) == 1,
                "an asic_miner that ignores the API soft-off's SIGTERM is "
                "SIGKILLed once, %u ms later, without blocking the loop "
                "(SIGTERM at %llu ms, SIGKILL at %llu ms, %d SIGKILL)",
                CHILD_TERM_MS, (unsigned long long)(t >= 0 ? tst_ev[t].now : 0),
                (unsigned long long)(k >= 0 ? tst_ev[k].now : 0),
                ev_sig(0, tst_run_end(), "kill-asic", SIGKILL));

        int                      rp  = ev_first(0, "reap-asic");
        int                      on0 = api0(TST_KILL_ON_PASS), on1 = p1(TST_KILL_ON_PASS);
        static const char *const on[] = { "fix", "rail-on", "spawn-asic" };
        CHECK(rp > k && ev_count(rp, on0 < 0 ? tst_ev_n : on0, "spawn-asic") == 0 && on0 >= 0 &&
                        ev_is(on0, 3, on, NULL) && on0 + 3 == on1 && g_softonoff.api_softoff == 0,
                "...the reap is the deliberate stop (no respawn before the "
                "soft-on), and the soft-on then respawns asic_miner itself "
                "(saw %s)",
                ev_str(on0 < 0 ? p0(TST_KILL_ON_PASS) : on0, on1));
    }

    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    g_fault_off   = 1;
    g_softoff_rsn = MC_SOFTOFF_FAN_ERR;
    pid_t stopped = -1;
    int   before  = tst_ev_n;
    mm_api_act(softonoff_api_step(&g_softonoff, 1), 1, &stopped);
    CHECK(g_fault_off && stopped == -1 && tst_ev_n == before,
            "direct ON after a fault preserves the existing no-op behavior");
    mm_api_act(softonoff_api_step(&g_softonoff, 0), 0, &stopped);
    CHECK(g_fault_off && g_softonoff.api_softoff && stopped == -1 &&
                    g_softoff_rsn == MC_SOFTOFF_FAN_ERR,
            "OFF during a fault latches user intent without starting ASIC or replacing "
            "the fault reason");
    mm_api_act(softonoff_api_step(&g_softonoff, 1), 1, &stopped);
    CHECK(!g_fault_off && !g_softonoff.api_softoff && stopped > 0,
            "OFF then ON explicitly retries and starts ASIC after a fault");

    const uint8_t faults[] = { MC_SOFTOFF_FAN_ERR, MC_SOFTOFF_POWER_FAULT, MC_SOFTOFF_HASH_FAULT };
    for (size_t i = 0; i < sizeof faults / sizeof faults[0]; i++) {
        g_fault_off   = 1;
        g_softoff_rsn = faults[i];
        stopped       = -1;
        mm_fan_set_fixed(FAN_DUTY_SOFTOFF_PCT);
        before = tst_ev_n;
        mm_thermal_act(SOFTONOFF_THERMAL_RESUME, &stopped);
        CHECK(g_fault_off && g_softoff_rsn == faults[i] && stopped == -1 &&
                        ev_count(before, tst_ev_n, "spawn-asic") == 0,
                "thermal resume preserves independent fault %u without spawning", faults[i]);
        CHECK(ev_count(before, tst_ev_n, "rail-on") == 0 &&
                        ev_count(before, tst_ev_n, "fix") == 0 &&
                        ev_count(before, tst_ev_n, "duty") == 0,
                "thermal resume leaves rail and fixed fan untouched for fault %u", faults[i]);
    }

    scenario(TST_RBF_PASS + 2 + (int)(RESTART_MS / TST_TICK_MS) + 2, s_reboot_failed);
    {
        int r0 = p0(TST_RBF_PASS);
        CHECK(ev_count(r0, tst_run_end(), "rail-on") == 0 &&
                        ev_count(r0, tst_run_end(), "spawn-asic") == 0,
                "a failed reboot keeps the rail off and starts no %s, even after "
                "the child exits and the restart backoff elapses (saw %s)",
                ASIC_BIN, ev_str(r0, tst_run_end()));
        CHECK(atomic_load(&g_rebooting) == 1 && atomic_load(&g_reboot_failed) == 0 &&
                        tst_rbf_stopped_logs == 1,
                "...the rebooting count stays held, the request is consumed and "
                "\"reboot failed — mining stays stopped\" is logged once "
                "(count %d, request %d, %d log(s))",
                atomic_load(&g_rebooting), atomic_load(&g_reboot_failed), tst_rbf_stopped_logs);
        atomic_store(&g_rebooting, 0);
    }

    scenario(TST_RBF_PASS + 2 + 4 * (int)(RESTART_MS / TST_TICK_MS) + 2, s_reboot_failed_exit);
    {
        int r0 = p0(TST_RBF_PASS);
        CHECK(ev_count(r0, tst_run_end(), "rail-on") == 0 &&
                        ev_count(r0, tst_run_end(), "spawn-asic") == 0 &&
                        tst_rbf_refused_logs == 1 && g_asic_retry_at == 0,
                "a mining %s that exits after a failed reboot is refused once, "
                "logged once and not retried over four restart backoffs (saw %s, "
                "%d log(s), retry at %llu)",
                ASIC_BIN, ev_str(r0, tst_run_end()), tst_rbf_refused_logs,
                (unsigned long long)g_asic_retry_at);
        atomic_store(&g_rebooting, 0);
        g_asic_ever_live = 0;
    }

    {
        pid_t asic = -1;

        memset(&g_softonoff, 0, sizeof g_softonoff);
        g_fault_off             = 0;
        g_softoff_rsn           = MC_SOFTOFF_NONE;
        g_softonoff.api_softoff = 1;
        atomic_store(&g_rebooting, 1);
        before = tst_ev_n;
        mm_api_act(softonoff_api_step(&g_softonoff, 1), 1, &asic);
        CHECK(ev_count(before, tst_ev_n, "rail-on") == 0 &&
                        ev_count(before, tst_ev_n, "spawn-asic") == 0 && asic == -1,
                "a soft-on during a reboot enables no rail and spawns nothing "
                "(saw %s)",
                ev_str(before, tst_ev_n));
        CHECK(!strcmp(tst_last_log, "soft-on by API — mining stays stopped"),
                "...and logs that mining stays stopped (saw %s)", tst_last_log);
        CHECK(!g_fault_off && g_softoff_rsn == MC_SOFTOFF_NONE && !g_softonoff.api_softoff,
                "...and latches no POWER FAULT from the refused enable (fault %d, "
                "rsn %u, latch %d)",
                (int)g_fault_off, (unsigned)g_softoff_rsn, g_softonoff.api_softoff);

        atomic_store(&g_reboot_failed, 1);
        tst_rbf_stopped_logs = 0;
        before               = tst_ev_n;
        mm_reboot_failed_act();
        mm_reboot_failed_act();
        CHECK(tst_ev_n == before && asic == -1 && atomic_load(&g_reboot_failed) == 0 &&
                        atomic_load(&g_rebooting) == 1 && tst_rbf_stopped_logs == 1,
                "a failed reboot enables no rail, starts nothing, leaves the "
                "rebooting count held and logs once (saw %s, request %d, count %d, "
                "%d log(s))",
                ev_str(before, tst_ev_n), atomic_load(&g_reboot_failed), atomic_load(&g_rebooting),
                tst_rbf_stopped_logs);

        g_softonoff.api_softoff = 1;
        atomic_store(&g_reboot_failed, 1);
        tst_rbf_stopped_logs = 0;
        before               = tst_ev_n;
        mm_reboot_failed_act();
        CHECK(tst_ev_n == before && atomic_load(&g_reboot_failed) == 0 && tst_rbf_stopped_logs == 1,
                "a failed reboot during a soft-off leaves the rail off and logs "
                "once (saw %s, %d log(s))",
                ev_str(before, tst_ev_n), tst_rbf_stopped_logs);
        atomic_store(&g_rebooting, 0);

        memset(&g_softonoff, 0, sizeof g_softonoff);
        g_softoff_rsn = MC_SOFTOFF_ASIC_OVERHOT;
        g_fault_off   = 0;
        atomic_store(&g_rebooting, 1);
        asic   = -1;
        before = tst_ev_n;
        mm_thermal_act(SOFTONOFF_THERMAL_RESUME, &asic);
        CHECK(ev_count(before, tst_ev_n, "rail-on") == 0 &&
                        ev_count(before, tst_ev_n, "spawn-asic") == 0 && asic == -1 &&
                        atomic_load(&g_rebooting) == 1,
                "a thermal recovery after a failed reboot restarts nothing (saw "
                "%s, count %d)",
                ev_str(before, tst_ev_n), atomic_load(&g_rebooting));
        atomic_store(&g_rebooting, 0);
        g_softoff_rsn = MC_SOFTOFF_NONE;
    }

    return tst_done();
}
