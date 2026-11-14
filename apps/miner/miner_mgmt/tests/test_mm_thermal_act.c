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
#include <pthread.h>
#include <time.h>

#include "boardconf.h"
#include "mm_spawn.h"
#include "fan_dev.h"
#include "mm_telem.h"
#include "psu_ops.h"
#include "wdt_dev.h"
#include "fan_pid.h"
#include "thermal.h"
#include "softonoff.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* Overridden: asic_miner spawns with no delay, s */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_MAX_PASS  260                /* Per-pass array capacity, passes */
#define TST_SLEEP_CAP (TST_MAX_PASS * 4) /* Sleeps before the loop counts as runaway */
#define TST_TICK_MS   1000u              /* Simulated time per pass, ms */
/* Passes in one thermal softoff hold */
#define TST_HOLD_PASS ((int)(OVERHOT_HOLD_MS / TST_TICK_MS))
#define TST_CHK_MAX   512 /* Capacity of tst_chk[], checks */
#define TST_TRIP_PASS 2   /* Pass s_trip() raises the overheat request on */
/* Pass the thermal hold ends on */
#define TST_RESUME_PASS (TST_TRIP_PASS + TST_HOLD_PASS)
#define TST_HOT_FROM    1 /* First pass the s_hot_* scripts run hot */
/* Core-wedge scenario passes: wedge time + 3 */
#define TST_WEDGE_PASSES ((int)(WDT_CORE_WEDGE_MS / TST_TICK_MS) + 3)

#define TST_EV_STR_MAX 512 /* Size of the ev_str() result buffer, bytes */

#define TST_FAN_DUTY_EV /* fan_dev stubs log each duty as a "duty" event */

pid_t       tst_fork(void);
int         tst_system(const char *cmd);
int         tst_kill(pid_t pid, int sig);
pid_t       tst_waitpid(pid_t pid, int *status, int options);
int         tst_stat(const char *path, struct stat *st);
int         tst_pthread_create(void);
static void tst_fan_fixed(fan_pid_state_t *st, int duty_pct);
static int  tst_thermal_check(const thermal_params_t *pp, thermal_softoff_state_t *st,
         int temp_summax, const int *outlet_temp, int n_outlet, int in_work);

#define fork()           tst_fork()                 /* tst_fork(): forks nothing */
#define system(c)        tst_system(c)              /* tst_system(): logs "system?" */
#define kill(p, s)       tst_kill((p), (s))         /* tst_kill(): logs the signal */
#define waitpid(p, s, o) tst_waitpid((p), (s), (o)) /* tst_waitpid(): scripted reaps */
#define stat(p, b)       tst_stat((p), (b))         /* tst_stat(): always fails */
/* tst_pthread_create(): always fails */
#define pthread_create(t, a, f, arg) ((void)(f), tst_pthread_create())
#define fan_pid_set_fixed(s, d)      tst_fan_fixed((s), (d)) /* tst_fan_fixed(): logs "fix" */
/* tst_thermal_check(): records into tst_chk[] */
#define thermal_softoff_check(a, b, c, d, e, f) tst_thermal_check((a), (b), (c), (d), (e), (f))

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
#include "mm_miner_tu.h"
#undef thermal_softoff_check
#undef fan_pid_set_fixed
#undef pthread_create
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork

_Static_assert(FAN_DUTY_SOFTOFF_PCT != FAN_DUTY_API_SOFTOFF_PCT,
        "the thermal soft-off overheat pin and the API soft-off pin are the same duty "
        "on this descriptor — the thermal-vs-API-soft-off duty assertions would "
        "be vacuous");
_Static_assert(FAN_DUTY_SOFTOFF_PCT > 0,
        "FAN_DUTY_SOFTOFF_PCT must be a real duty; 0 would collide with "
        "the fan-released sentinel handling below");
_Static_assert(TST_WEDGE_PASSES < TST_HOLD_PASS,
        "the wedge scenario must end INSIDE the thermal hold, or the "
        "hold would expire and the SIGKILL under test would no longer be "
        "happening under a thermal softoff at all");
_Static_assert(TST_WEDGE_PASSES <= TST_MAX_PASS,
        "the wedge scenario must fit the pass budget, or it would end "
        "before WDT_CORE_WEDGE_MS and pass vacuously");

/* One thermal check: time (ms), its inputs and its verdict */
typedef struct {
    uint64_t now;
    int      summax, n_outlet, in_work, verdict;
} tst_chk_rec;
typedef void (*tst_script_fn)(int pass); /* Per-pass scenario script, called from mm_telem_poll() */

static int      tst_net_up; /* 1: net_state_get() reports the network up */
static uint64_t tst_now;    /* Simulated clock, ms */
/* Current pass, passes wanted, sleeps so far, runaway flag */
static int           tst_pass, tst_want, tst_sleeps, tst_runaway;
static uint64_t      tst_pass_ms[TST_MAX_PASS]; /* Simulated time at the start of each pass, ms */
static int           tst_pass_ev0[TST_MAX_PASS + 1]; /* tst_ev_n at the start of each pass */
static int           tst_wp_calls, tst_wp_reaps; /* tst_waitpid() calls and the reaps it reported */
static tst_chk_rec   tst_chk[TST_CHK_MAX];       /* Thermal checks, in call order */
static int           tst_chk_n;
static tst_script_fn tst_script;
/* mm_telem_get() inputs: fresh/asic/core/work flags, hottest chip and outlet (degC), boards */
static int   tst_fresh, tst_asic_live, tst_core_live, tst_in_work, tst_tmax, tst_outlet, tst_hbn;
static pid_t tst_next_pid = 4000;        /* PID the next tst_fork() returns */
static pid_t tst_pid_core, tst_pid_asic; /* PIDs of the current core_miner and asic_miner */
/* Reap knobs: alternate asic reaps, alternation phase, one-shot asic reap, its use count */
static int  tst_reap_alt, tst_reap_turn, tst_reap_once, tst_reap_once_n;
static int  tst_reap_armed;      /* 1 arms the one-shot asic reap; set by s_trip() */
static int  tst_req_during_hold; /* 1: s_trip() repeats the overheat request mid-hold */
static int  tst_work_from;       /* First pass the asic is live and in work */
static char tst_op_last[160];    /* Last OP log line */
static int  tst_op_n;            /* OP log lines */

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

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    if (lvl != OSIF_LOG_OP)
        return;
    va_start(ap, fmt);
    vsnprintf(tst_op_last, sizeof tst_op_last, fmt, ap);
    va_end(ap);
    tst_op_n++;
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
    if (tst_pass < TST_MAX_PASS) {
        tst_pass_ms[tst_pass]  = tst_now;
        tst_pass_ev0[tst_pass] = tst_ev_n;
    }
    tst_script(tst_pass);
    if (++tst_pass >= tst_want)
        g_run = 0;
    if (tst_pass <= TST_MAX_PASS)
        tst_pass_ev0[tst_pass] = tst_ev_n;
}

void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
    out->ps_vout_mv = POWER_VALID_VOLT_MIN;
    out->poll_fresh = tst_fresh;
    out->asic_live  = tst_asic_live;
    out->core_live  = tst_core_live;
    out->in_work    = tst_in_work;
    out->tmax       = tst_tmax;
    out->tavg       = tst_tmax;
    out->hb_count   = tst_hbn;
    for (int b = 0; b < HBOARD_COUNT; b++)
        out->hb_outlet[b] = out->hb_inlet[b] = tst_outlet;
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
int tst_system(const char *cmd)
{
    (void)cmd;
    ev("system?", 0);
    return 0;
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
int tst_kill(pid_t pid, int sig)
{
    const char *who = pid == tst_pid_core ? "kill-core" :
            pid == tst_pid_asic           ? "kill-asic" :
                                            "kill-?";
    ev(who, sig);
    return 0;
}
int tst_pthread_create(void)
{
    return -1;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    tst_wp_calls++;
    if (status)
        *status = 0;
    if (tst_reap_once && tst_reap_armed && tst_reap_once_n == 0 && pid == tst_pid_asic) {
        tst_reap_once_n = 1;
        tst_wp_reaps++;
        return tst_pid_asic;
    }
    if (tst_reap_alt && pid == tst_pid_asic && (tst_reap_turn = !tst_reap_turn)) {
        tst_wp_reaps++;
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
static int tst_thermal_check(const thermal_params_t *pp, thermal_softoff_state_t *st,
        int temp_summax, const int *outlet_temp, int n_outlet, int in_work)
{
    int v = thermal_softoff_check(pp, st, temp_summax, outlet_temp, n_outlet, in_work);
    if (tst_chk_n < TST_CHK_MAX)
        tst_chk[tst_chk_n++] = (tst_chk_rec){ tst_now, temp_summax, n_outlet, in_work, v };
    return v;
}

static void scenario(int passes, tst_script_fn s, int api_req)
{
    tst_now         = 0;
    tst_pass        = 0;
    tst_want        = passes;
    tst_script      = s;
    tst_sleeps      = 0;
    tst_runaway     = 0;
    tst_ev_n        = 0;
    tst_chk_n       = 0;
    tst_reap_turn   = 1;
    tst_reap_once_n = 0;
    tst_reap_armed  = 0;
    tst_wp_calls    = 0;
    tst_wp_reaps    = 0;
    tst_pid_core = tst_pid_asic = 0;
    memset(tst_pass_ms, 0, sizeof tst_pass_ms);
    memset(tst_pass_ev0, 0, sizeof tst_pass_ev0);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_softoff_rsn       = MC_SOFTOFF_NONE;
    g_fault_off         = 0;
    g_api_req           = api_req;
    tst_fresh           = 1;
    tst_asic_live       = 1;
    tst_core_live       = 1;
    tst_in_work         = 1;
    tst_tmax            = 60;
    tst_outlet          = 60;
    tst_hbn             = HBOARD_COUNT;
    g_run               = 1;
    tst_net_raise();
    mm_miner_main_unused();
}

static void s_trip(int p)
{
    if (p == TST_TRIP_PASS) {
        g_overheat_req = 1;
        tst_reap_armed = 1;
    }
    if (tst_req_during_hold && p == TST_TRIP_PASS + TST_HOLD_PASS / 2)
        g_overheat_req = 1;
}
static void s_quiet(int p)
{
    (void)p;
}

static void s_hot_pvt(int p)
{
    if (p >= TST_HOT_FROM)
        tst_tmax = PVT_TEMP_OVER_HR_MAX + 10;
}
static void s_hot_outlet(int p)
{
    if (p >= TST_HOT_FROM)
        tst_outlet = HASHOUT_OVER_HR_MAX + 5;
}

static void s_hot_idle(int p)
{
    if (HASHOUT_OVERHOT_CNT_MAX > 0)
        tst_outlet = HASHOUT_OVER_HR_MAX + 5;
    else
        tst_tmax = PVT_TEMP_OVER_HR_MAX + 10;
    tst_asic_live = tst_in_work = (p >= tst_work_from);
}

static uint64_t chk_interval(int from, int to)
{
    if (to - from < 2)
        return (uint64_t)-1;
    uint64_t d = tst_chk[from + 1].now - tst_chk[from].now;
    for (int i = from + 1; i + 1 < to; i++)
        if (tst_chk[i + 1].now - tst_chk[i].now != d)
            return (uint64_t)-1;
    return d;
}
static int chk_first_hot(void)
{
    for (int i = 0; i < tst_chk_n; i++)
        if (tst_chk[i].verdict)
            return i + 1;
    return -1;
}
static uint64_t chk_hot_run_start(int n)
{
    int i = chk_first_hot() - n;
    return i >= 0 && i < tst_chk_n ? tst_chk[i].now : (uint64_t)-1;
}

static void s_trip_noreap(int p)
{
    if (p == TST_TRIP_PASS)
        g_overheat_req = 1;
    tst_core_live = 0;
}

int main(void)
{
    printf("descriptor %s: %d board(s), thermal-softoff pin %d%%, api-softoff pin %d%%, "
           "enum pin %d%%, PVT over %d degC, outlet over %d degC x%d, "
           "hold %u ms (%d passes), respawn delay %u ms\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, FAN_DUTY_SOFTOFF_PCT, FAN_DUTY_API_SOFTOFF_PCT,
            FAN_DUTY_ENUM_PCT, PVT_TEMP_OVER_HR_MAX, HASHOUT_OVER_HR_MAX, HASHOUT_OVERHOT_CNT_MAX,
            OVERHOT_HOLD_MS, TST_HOLD_PASS, RESTART_MS);

    tst_req_during_hold = 0;
    scenario(TST_TRIP_PASS + 3, s_trip, 0);
    CHECK(!tst_runaway, "the loop exits on a cleared g_run (%d sleeps, cap %d)", tst_sleeps,
            TST_SLEEP_CAP);

    {
        static const char *const up[] = { "rail-on", "spawn-core", "spawn-asic" };
        CHECK(ev_is(0, 3, up, NULL),
                "bring-up: the rail is energized before either child is spawned, "
                "core_miner before asic_miner (saw %s)",
                ev_str(0, 4));
    }

    int t0 = p0(TST_TRIP_PASS), t1 = p1(TST_TRIP_PASS);
    {
        static const char *const off[] = { "kill-asic", "rail-off", "fix", "duty" };
        const int offv[] = { SIGTERM, 0, FAN_DUTY_SOFTOFF_PCT, FAN_DUTY_SOFTOFF_PCT * 10 };
        CHECK(ev_is(t0, 4, off, offv),
                "SOFTONOFF_THERMAL_OFF actuates exactly, and in this order: SIGTERM "
                "asic_miner, rail OFF (board_power_rail_off, "
                "in-process), then the fan "
                "pinned at FAN_DUTY_SOFTOFF_PCT (%d%%) through BOTH the PID override "
                "and the HAL. Heat source stopped, rail cut, fans pinned — that "
                "sequence IS the thermal soft-off (saw %s)",
                FAN_DUTY_SOFTOFF_PCT, ev_str(t0, t1));
    }

    CHECK(ev_count(t0, t1, "kill-core") == 0,
            "...and core_miner is NOT signalled: it is the :4028 API server, so "
            "killing it made an over-hot board unreachable to the phone app and to "
            "every :4028 API client for the whole %u ms hold, exactly when the "
            "operator needs to read the fault (saw %d kill-core)",
            OVERHOT_HOLD_MS, ev_count(t0, t1, "kill-core"));

    CHECK(ev_count(t0, t1, "spawn-asic") == 0 && ev_count(t0, t1, "spawn-core") == 0,
            "...and NEITHER child is respawned on this branch: the trip only stops "
            "the miner, the hold decides when anything comes back (saw %d core / %d "
            "asic spawns)",
            ev_count(t0, t1, "spawn-core"), ev_count(t0, t1, "spawn-asic"));

    CHECK(ev_count(t0, t1, "join") == 0,
            "...and the rail is cut with NO join: the SIGTERM is fire-and-"
            "forget, unlike the shutdown tail, which joins the children "
            "before the rail-off and says why. Reported, not changed (saw %s)",
            ev_str(t0, t1));

    {
        int r = ev_first(t0, "rail-off"), f = ev_first(t0, "fix");
        CHECK(r >= 0 && f > r,
                "...and the fan pin is STILL the LAST act, after the blocking "
                "rail-off — the fork is gone but the call is synchronous, so full "
                "airflow on an overheated board still waits for the i2c/GPIO "
                "sequence (rail-off at act %d, fan pin at act %d)",
                r - t0, f - t0);
    }

    CHECK(g_overheat_req == 0,
            "g_overheat_req is CONSUMED by the verdict (:1248, cleared on any "
            "non-NONE verdict and only then), so the same hot sample cannot trip "
            "twice (flag %d after the run)",
            g_overheat_req);

    CHECK(g_softoff_rsn == MC_SOFTOFF_ASIC_OVERHOT,
            "...and the trip LATCHES the reason the wire reports: SoftOFF[%d] "
            "(saw %d), which is what :4028 and the phone app read while the board "
            "is held off",
            MC_SOFTOFF_ASIC_OVERHOT, g_softoff_rsn);

    tst_req_during_hold = 0;
    scenario(TST_RESUME_PASS, s_trip, 0);
    {
        int after = p1(TST_TRIP_PASS), end = tst_run_end();
        CHECK(ev_count(0, end, "rail-off") == 1,
                "exactly ONE rail cut for the whole %u ms hold (%d passes of "
                "SOFTONOFF_NONE), not one per iteration (saw %d)",
                OVERHOT_HOLD_MS, TST_HOLD_PASS, ev_count(0, end, "rail-off"));
        int k = ev_first(after, "kill-asic");
        int t = ev_first(p0(TST_TRIP_PASS), "kill-asic");
        CHECK(ev_count(after, end, "kill-core") == 0 && ev_count(after, end, "kill-asic") == 1 &&
                        k >= 0 && t >= 0 && tst_ev[k].val == SIGKILL &&
                        tst_ev[k].now == tst_ev[t].now + CHILD_TERM_MS &&
                        ev_count(after, end, "fix") == 0,
                "...and across the hold the only further signal is ONE SIGKILL to "
                "an asic_miner still unreaped %u ms after the SIGTERM; nothing is "
                "re-pinned: %d kill-core, %d kill-asic (sig %d at %llu ms), %d "
                "fan pins after the trip pass",
                CHILD_TERM_MS, ev_count(after, end, "kill-core"), ev_count(after, end, "kill-asic"),
                k >= 0 ? tst_ev[k].val : 0, (unsigned long long)(k >= 0 ? tst_ev[k].now : 0),
                ev_count(after, end, "fix"));
    }

    tst_reap_once       = 1;
    tst_req_during_hold = 1;
    scenario(TST_RESUME_PASS + 2, s_trip, 0);
    tst_reap_once       = 0;
    tst_req_during_hold = 0;

    int r0 = p0(TST_RESUME_PASS), r1 = p1(TST_RESUME_PASS);
    {
        static const char *const res[] = { "fix", "rail-on", "spawn-asic" };
        CHECK(ev_is(r0, 3, res, NULL) && tst_ev[r0].val == -1,
                "SOFTONOFF_THERMAL_RESUME actuates exactly, and in this order: release "
                "the fan to the PID (fix=-1), re-enable the rail, THEN respawn "
                "asic_miner — so the rail is hot before the child that drives the "
                "ASIC UART exists (saw %s)",
                ev_str(r0, r1));
    }

    CHECK(ev_count(r0, r1, "spawn-core") == 0,
            "...and core_miner is NOT respawned, because it was never stopped: "
            "mm_thermal_act holds no core pid and has no core arm at all, so :4028 "
            "answered without interruption across the whole hold (saw %d "
            "spawn-core). Re-add an `if (*core <= 0) spawn_child(CORE_DIR, ...)` "
            "arm and it is dead code that reads as live protection: the reap branch "
            "respawns a crashed core_miner and `continue`s, so a core that has died "
            "but not been reaped still has core > 0 here and the arm cannot fire",
            ev_count(r0, r1, "spawn-core"));

    CHECK(ev_count(r0, r1, "rail-off") == 0 && ev_count(r0, r1, "kill-core") == 0 &&
                    ev_count(r0, r1, "kill-asic") == 0,
            "...and the resume signals nothing and cuts nothing (saw %s)", ev_str(r0, r1));

    {
        int want = FAN_DUTY_ENUM_PCT > 0 ? FAN_DUTY_ENUM_PCT : -1;
        CHECK(g_fan_st.fixed_duty == want,
                "the effective pin after the resume pass is %d: FAN_DUTY_ENUM_PCT "
                "is %d%% on this board, so the thermal release to the PID %s "
                "overridden by the enum pin in the SAME iteration (spawn_asic "
                "re-arms it, the enum block runs after) — pin %d",
                want, FAN_DUTY_ENUM_PCT, FAN_DUTY_ENUM_PCT > 0 ? "IS" : "is NOT",
                g_fan_st.fixed_duty);
    }

    CHECK(g_softoff_rsn == MC_SOFTOFF_NONE,
            "...and the RESUME clears the reason as well as the fault latch (saw "
            "%d, want %d). Without this the board mines again while :4028 goes on "
            "reporting SoftOFF[%d] until the next reboot, and the web dashboard's "
            "card stays red for the life of the process — MC_SOFTOFF_NONE had no "
            "assignment anywhere in the non-test tree",
            g_softoff_rsn, MC_SOFTOFF_NONE, MC_SOFTOFF_ASIC_OVERHOT);

    CHECK(g_overheat_req == 0 && ev_count(r1, tst_run_end(), "rail-off") == 0,
            "a hot sample raised DURING the hold is DROPPED at expiry, not queued: "
            "the verdict is RESUME, the flag is consumed by it, and the miner comes "
            "back up needing a FRESH hot sample to trip again — it does not re-trip "
            "on the stale one (flag %d, %d further rail cuts)",
            g_overheat_req, ev_count(r1, tst_run_end(), "rail-off"));

    tst_reap_once = 1;
    scenario(TST_RESUME_PASS + 2, s_trip, -1);
    tst_reap_once = 0;

    int k0 = p0(TST_RESUME_PASS), k1 = p1(TST_RESUME_PASS);
    {
        static const char *const softoff_ev[] = { "fix", "duty" };
        const int softoff_evv[] = { FAN_DUTY_API_SOFTOFF_PCT, FAN_DUTY_API_SOFTOFF_PCT * 10 };
        CHECK(g_softonoff.api_softoff && ev_is(k0, 2, softoff_ev, NULL) &&
                        tst_ev[k0].val == softoff_evv[0] && tst_ev[k0 + 1].val == softoff_evv[1] &&
                        ev_count(k0, k1, "spawn-core") == 0,
                "SOFTONOFF_THERMAL_TO_SOFTOFF actuates exactly: hand the duty to "
                "FAN_DUTY_API_SOFTOFF_PCT (%d%%) — NOT to the PID and NOT "
                "at the overheat pin's %d%% — and respawn nothing, core_miner having "
                "stayed up throughout (api_softoff %d, saw %s)",
                FAN_DUTY_API_SOFTOFF_PCT, FAN_DUTY_SOFTOFF_PCT, g_softonoff.api_softoff,
                ev_str(k0, k1));
    }

    CHECK(ev_count(k0, tst_run_end(), "rail-on") == 0 &&
                    ev_count(k0, tst_run_end(), "spawn-asic") == 0,
            "...and the rail stays DOWN with asic_miner STOPPED for as long as the "
            "user's soft-off holds — the thermal hold expiring does not soft-on a "
            "deliberately soft-off miner (saw %d rail-ons, %d asic spawns)",
            ev_count(k0, tst_run_end(), "rail-on"), ev_count(k0, tst_run_end(), "spawn-asic"));

    CHECK(g_overheat_req == 0,
            "...and the request is consumed on THIS verdict too (:1248 tests "
            "!= SOFTONOFF_NONE, not == SOFTONOFF_THERMAL_OFF) — flag %d",
            g_overheat_req);

    CHECK(g_softoff_rsn == MC_SOFTOFF_API,
            "...and the over-temperature reason is replaced on this arm too (saw "
            "%d, want %d): the cool-down is over and the API soft-off that still "
            "holds the machine reports the same reason an API soft-off does",
            g_softoff_rsn, MC_SOFTOFF_API);

    scenario(12, s_quiet, 0);
    CHECK(tst_chk_n == 12 && chk_interval(0, tst_chk_n) == TST_TICK_MS,
            "healthy: the thermal soft-off rule is invoked once per supervised pass, %llu ms "
            "apart — 1 Hz, so five consecutive calls ARE the 5 s "
            "debounce window, exactly as :1088 claims (%d calls, interval %lld)",
            (unsigned long long)TST_TICK_MS, tst_chk_n, (long long)chk_interval(0, tst_chk_n));
    CHECK(tst_wp_reaps == 0 && tst_wp_calls == 2 * tst_pass,
            "...and one loop iteration per pass, so nothing is being skipped in "
            "the baseline: both children are polled every pass (core pid then "
            "asic pid), pass 0 included because asic_miner is spawned at "
            "start-up, and neither is ever reaped (%d calls, %d reaps, %d "
            "passes)",
            tst_wp_calls, tst_wp_reaps, tst_pass);

    tst_reap_alt = 1;
    scenario(12, s_quiet, 0);
    tst_reap_alt = 0;
    CHECK(tst_wp_reaps > 1 && ev_count(0, tst_run_end(), "spawn-asic") > 1 &&
                    chk_interval(0, tst_chk_n) == TST_TICK_MS,
            "a crash-looping asic_miner does not stretch the thermal soft-off's "
            "call interval: the %u ms respawn delay is a deadline for "
            "mm_spawn_retry, not a sleep, so the rule still runs every %u ms "
            "(%d reaps, %d asic spawns, interval %lld)",
            RESTART_MS, TST_TICK_MS, tst_wp_reaps, ev_count(0, tst_run_end(), "spawn-asic"),
            (long long)chk_interval(0, tst_chk_n));

    scenario(6, s_hot_pvt, 0);
    uint64_t pvt_fast = tst_ev[ev_first(0, "rail-off")].now - chk_hot_run_start(1);
    CHECK(chk_first_hot() == TST_HOT_FROM + 1 && pvt_fast == 1 * TST_TICK_MS,
            "the PVT arm is single-sample: the FIRST hot call trips it (call %d) "
            "and the rail is cut %llu ms later — one call, so 1 x the interval "
            "(rail-off %llu ms after that call)",
            chk_first_hot(), (unsigned long long)TST_TICK_MS, (unsigned long long)pvt_fast);

    tst_reap_alt = 1;
    scenario(6, s_hot_pvt, 0);
    tst_reap_alt      = 0;
    uint64_t pvt_loop = tst_ev[ev_first(0, "rail-off")].now - chk_hot_run_start(1);
    CHECK(chk_first_hot() == TST_HOT_FROM + 1 && pvt_loop == pvt_fast,
            "...and a crash loop does not delay it (%llu ms from the first hot "
            "sample to the rail cut, %llu ms healthy)",
            (unsigned long long)pvt_loop, (unsigned long long)pvt_fast);

    if (HASHOUT_OVERHOT_CNT_MAX > 0) {
        const int n = HASHOUT_OVERHOT_CNT_MAX;

        scenario(n + 5, s_hot_outlet, 0);
        uint64_t fast = tst_ev[ev_first(0, "rail-off")].now - chk_hot_run_start(n);
        CHECK(chk_first_hot() == n + TST_HOT_FROM && fast == (uint64_t)n * TST_TICK_MS,
                "the OUTLET arm debounces on %d consecutive CALLS, so at 1 Hz the "
                "window is %d x %u = %llu ms — %d s (tripped on "
                "call %d, rail cut %llu ms after the first hot sample)",
                n, n, TST_TICK_MS, (unsigned long long)((uint64_t)n * TST_TICK_MS), n,
                chk_first_hot(), (unsigned long long)fast);

        tst_reap_alt = 1;
        scenario(n + 5, s_hot_outlet, 0);
        tst_reap_alt  = 0;
        uint64_t loop = tst_ev[ev_first(0, "rail-off")].now - chk_hot_run_start(n);
        CHECK(chk_first_hot() == n + TST_HOT_FROM && loop == fast,
                "...and a crash-looping child does not dilate that window: the "
                "arm counts calls and the calls stay at 1 Hz (tripped on call "
                "%d, rail cut %llu ms after the first hot sample; %llu ms "
                "healthy)",
                chk_first_hot(), (unsigned long long)loop, (unsigned long long)fast);
    } else {
        scenario(12, s_hot_outlet, 0);
        CHECK(ev_count(0, tst_run_end(), "rail-off") == 0 && chk_first_hot() < 0,
                "this descriptor has NO counted arm (HASHOUT_OVERHOT_CNT_MAX 0, no "
                "hashboard MCU), so hot outlets alone never trip the thermal soft-off however long "
                "they stay hot (%d in-loop rail cuts, first hot call %d)",
                ev_count(0, tst_run_end(), "rail-off"), chk_first_hot());
    }

    tst_reap_once       = 1;
    tst_req_during_hold = 0;
    scenario(TST_TRIP_PASS + 8, s_trip, 0);
    tst_reap_once = 0;
    {
        int bad = -1;
        for (int k = 0; k + 1 < tst_pass; k++)
            if (tst_pass_ms[k + 1] - tst_pass_ms[k] != TST_TICK_MS) {
                bad = k;
                break;
            }
        CHECK(bad < 0 && chk_interval(0, tst_chk_n) == TST_TICK_MS,
                "the INTENTIONAL-exit reap (:1187) costs no wall time and drops NO "
                "rule tick: it `continue`s without sleeping, so `now` is frozen and "
                "the next ctl_tick lands on its original 1 Hz schedule — the thermal soft-off's "
                "interval is still %llu ms right through both reaps (first pass-gap "
                "mismatch %d, interval %lld)",
                (unsigned long long)TST_TICK_MS, bad, (long long)chk_interval(0, tst_chk_n));
        CHECK(tst_wp_reaps == 1,
                "...and its whole cost is ONE extra loop iteration, not two: the "
                "thermal softoff SIGTERMs asic_miner and nothing else, so asic_miner "
                "is the only INTENTIONAL exit there. core_miner is never signalled, "
                "never exits and is never reaped — it stays up serving :4028 for the "
                "whole hold (%d reap events)",
                tst_wp_reaps);
    }

    {
        const int n   = HASHOUT_OVERHOT_CNT_MAX > 0 ? HASHOUT_OVERHOT_CNT_MAX : 1;
        tst_work_from = 2 * n + 3;
        scenario(tst_work_from + 4, s_hot_idle, 0);

        int cold = 0;
        for (int i = 0; i < tst_chk_n && tst_chk[i].now < (uint64_t)tst_work_from * TST_TICK_MS;
                i++)
            if (tst_chk[i].verdict)
                cold = 1;
        int idle_end = p0(tst_work_from);
        CHECK(!cold && ev_count(0, idle_end, "rail-off") == 0,
                "a HOT board that is not In Work does not trip the thermal soft-off for %d calls "
                "— %dx its whole debounce window — because thermal_softoff_check "
                "returns at `if (!in_work)`. A crash-looping asic_miner takes "
                "SYSTEMSTATU to \"In Init\", so this, not the call rate, is what "
                "gates the trip on that path (early verdict %d, %d rail cuts)",
                tst_work_from, tst_work_from / n, cold, ev_count(0, idle_end, "rail-off"));

        int hot_call = chk_first_hot();
        CHECK(hot_call == tst_work_from + 1,
                "...and the FIRST In-Work call trips it outright (call %d of %d, at "
                "%llu ms): the run counter kept advancing while inhibited and the "
                "`if (hot) run = 0` clear sits below the early return, so it "
                "SATURATED — there is no debounce left to stretch, and %s",
                hot_call, tst_chk_n,
                (unsigned long long)(hot_call > 0 ? tst_chk[hot_call - 1].now : 0),
                HASHOUT_OVERHOT_CNT_MAX > 0 ?
                        "the counted arm's window was spent while the miner was idle" :
                        "this arm never had a window to begin with");

        int ro = ev_first(0, "rail-off");
        CHECK(ro >= 0 && tst_ev[ro].now == (uint64_t)(tst_work_from + 1) * TST_TICK_MS,
                "...so the rail is cut one pass after that first working sample, "
                "at %llu ms — the recovery, not the heat, is what schedules the thermal soft-off "
                "here (rail-off at %llu ms)",
                (unsigned long long)((uint64_t)(tst_work_from + 1) * TST_TICK_MS),
                (unsigned long long)(ro >= 0 ? tst_ev[ro].now : 0));
    }

    scenario(TST_WEDGE_PASSES, s_trip_noreap, 0);
    {
        int      sigterm = 0, sigkill = 0;
        uint64_t kill_at = 0;
        for (int i = 0; i < tst_run_end(); i++)
            if (!strcmp(tst_ev[i].what, "kill-core")) {
                if (tst_ev[i].val == SIGKILL) {
                    if (!sigkill)
                        kill_at = tst_ev[i].now;
                    sigkill++;
                } else {
                    sigterm++;
                }
            }
        CHECK(sigterm == 0 && sigkill == 1 && kill_at == (uint64_t)WDT_CORE_WEDGE_MS &&
                        tst_pass_ms[TST_WEDGE_PASSES - 1] >= (uint64_t)WDT_CORE_WEDGE_MS,
                "core_miner is never SIGTERMed by the thermal soft-off, and a core_miner that has "
                "gone silent IS SIGKILLed on schedule at WDT_CORE_WEDGE_MS (%u ms), "
                "over a run that reaches %llu ms — %u ms past it — with the hold "
                "still up: the thermal soft-off stops the heat source only, so core_miner's "
                "silence "
                "is NOT the state mm asked for and the wedge detector is live for "
                "the whole %u ms hold (%d SIGTERM, %d SIGKILL, first at %llu ms)",
                WDT_CORE_WEDGE_MS, (unsigned long long)tst_pass_ms[TST_WEDGE_PASSES - 1],
                (unsigned)(TST_WEDGE_PASSES * TST_TICK_MS - WDT_CORE_WEDGE_MS), OVERHOT_HOLD_MS,
                sigterm, sigkill, (unsigned long long)kill_at);
    }

    {
        static const uint8_t faults[] = { MC_SOFTOFF_FAN_ERR, MC_SOFTOFF_POWER_FAULT,
            MC_SOFTOFF_HASH_FAULT };
        for (size_t i = 0; i < sizeof faults / sizeof faults[0]; i++) {
            pid_t stopped = -1;
            memset(&g_softonoff, 0, sizeof g_softonoff);
            g_fault_off   = 1;
            g_softoff_rsn = faults[i];
            int e0        = tst_ev_n;
            mm_thermal_act(SOFTONOFF_THERMAL_OFF, &stopped);
            CHECK(g_softoff_rsn == faults[i] && ev_count(e0, tst_ev_n, "rail-off") == 1,
                    "a thermal trip over latched fault %u still cuts the rail "
                    "(%d rail-off) but keeps the fault's reason (%d)",
                    faults[i], ev_count(e0, tst_ev_n, "rail-off"), g_softoff_rsn);
            e0 = tst_ev_n;
            mm_thermal_act(SOFTONOFF_THERMAL_RESUME, &stopped);
            CHECK(g_fault_off && g_softoff_rsn == faults[i] && stopped == -1 &&
                            ev_count(e0, tst_ev_n, "rail-on") == 0 &&
                            ev_count(e0, tst_ev_n, "spawn-asic") == 0,
                    "...so the thermal resume does not clear fault %u: latch %d, "
                    "reason %d, no rail-on, no spawn",
                    faults[i], g_fault_off, g_softoff_rsn);
            g_softonoff.api_softoff = 1;
            mm_thermal_act(SOFTONOFF_THERMAL_TO_SOFTOFF, &stopped);
            CHECK(g_fault_off && g_softoff_rsn == faults[i],
                    "...nor does a cool-down ending under an API soft-off: "
                    "fault %u keeps latch %d and reason %d",
                    faults[i], g_fault_off, g_softoff_rsn);
        }
        {
            pid_t stopped = -1;
            memset(&g_softonoff, 0, sizeof g_softonoff);
            g_softonoff.api_softoff = 1;
            g_fault_off             = 0;
            g_softoff_rsn           = MC_SOFTOFF_ASIC_OVERHOT;
            mm_thermal_act(SOFTONOFF_THERMAL_TO_SOFTOFF, &stopped);
            CHECK(g_softonoff.api_softoff && g_softoff_rsn == MC_SOFTOFF_API,
                    "a cool-down ending under an API soft-off replaces the "
                    "over-temperature reason with the soft-off's (latch %d, "
                    "reason %d)",
                    g_softonoff.api_softoff, g_softoff_rsn);
            mm_api_act(softonoff_api_step(&g_softonoff, 1), 1, &stopped);
            CHECK(!g_softonoff.api_softoff && !g_fault_off && g_softoff_rsn == MC_SOFTOFF_NONE,
                    "...and the API soft-on that follows releases it and clears the "
                    "reason to %d (latch %d, fault %d)",
                    g_softoff_rsn, g_softonoff.api_softoff, g_fault_off);
        }
        g_fault_off   = 0;
        g_softoff_rsn = MC_SOFTOFF_NONE;
    }

    {
        static const char skip_on[] =
                "scheduled soft-on skipped — over-temperature cool-down in progress";
        int64_t now = (int64_t)time(NULL);

        memset(&g_softonoff, 0, sizeof g_softonoff);
        g_api_req                 = 0;
        g_soft                    = (soft_sched_t){ 0 };
        g_softonoff.thermal_until = 1;
        soft_sched_arm(&g_soft, 0, 1, now);
        tst_op_n = 0;
        soft_sched_rule(NULL, 0);
        CHECK(mm_api_req_take() == -1 && g_soft.off_valid == 0 && tst_op_n == 0,
                "a scheduled softoff due during the thermal hold is requested, for "
                "the API block to latch (valid %d, %d OP logs)",
                g_soft.off_valid, tst_op_n);

        g_softonoff.api_softoff = 1;
        soft_sched_arm(&g_soft, 1, 1, now);
        tst_op_n = 0;
        soft_sched_rule(NULL, 0);
        CHECK(mm_api_req_take() == 0 && g_soft.on_valid == 0 && tst_op_n == 1 &&
                        strcmp(tst_op_last, skip_on) == 0,
                "a scheduled softon due during the hold under an API soft-off is "
                "not executed, is cleared and is logged (valid %d, %d OP logs, "
                "last \"%s\")",
                g_soft.on_valid, tst_op_n, tst_op_last);

        g_softonoff.api_softoff = 0;
        soft_sched_arm(&g_soft, 0, 1, now + 3600);
        tst_op_n = 0;
        soft_sched_rule(NULL, 0);
        CHECK(mm_api_req_take() == 0 && g_soft.off_valid == 1 && tst_op_n == 0,
                "...while a schedule not yet due stays armed through the hold "
                "(valid %d, %d OP logs)",
                g_soft.off_valid, tst_op_n);

        g_softonoff.thermal_until = 0;
        soft_sched_arm(&g_soft, 0, 1, now);
        tst_op_n = 0;
        soft_sched_rule(NULL, 0);
        CHECK(mm_api_req_take() == -1 && g_soft.off_valid == 0 && tst_op_n == 0,
                "outside the hold a due scheduled softoff is requested as before "
                "(valid %d, %d OP logs)",
                g_soft.off_valid, tst_op_n);

        g_softonoff.api_softoff = 1;
        soft_sched_arm(&g_soft, 1, 1, now);
        tst_op_n = 0;
        soft_sched_rule(NULL, 0);
        CHECK(mm_api_req_take() == 1 && g_soft.on_valid == 0 && tst_op_n == 0,
                "...and so is a due scheduled softon (valid %d, %d OP logs)", g_soft.on_valid,
                tst_op_n);

        memset(&g_softonoff, 0, sizeof g_softonoff);
        g_soft = (soft_sched_t){ 0 };
    }

    return tst_done();
}
