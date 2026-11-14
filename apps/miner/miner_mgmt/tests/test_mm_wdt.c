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
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "boardconf.h"
#include "mm_spawn.h"
#include "fan_dev.h"
#include "mc_api_bootby.h"
#include "mm_cfgstore.h"
#include "mm_telem.h"
#include "mm_wdt_sig.h"
#include "osif.h"
#include "psu_ops.h"
#include "wdt_dev.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"
#include "tst_fan_dev.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* Overridden: asic_miner spawns with no delay, s */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_MAX_PASS  512                /* Capacity of the per-pass arrays, passes */
#define TST_SLEEP_CAP (TST_MAX_PASS * 4) /* Sleeps before the loop counts as runaway */
#define TST_NS_MAX    64                 /* Capacity of the per-sleep arrays, sleeps */
#define TST_TICK_MS   1000u              /* Simulated time per tick, ms */
/* Ticks in WDT_STARVE_MS */
#define TST_STARVE_PASS ((int)(WDT_STARVE_MS / TST_TICK_MS))
#define TST_EV_MAX      32   /* Capacity of tst_ev[], events */
#define TST_TURN_SUP    0    /* Lockstep turn: the supervisor runs */
#define TST_TURN_WDT    1    /* Lockstep turn: the watchdog thread runs */
#define TST_STEP_MAX    4096 /* Slices before a lockstep run counts as runaway */
#define TST_STEP_WAIT_S 5    /* Lockstep handoff timeout, s */
#define TST_KILL_MAX    16   /* Capacity of the SIGKILL records */
/* First unfed tick when a TID is missing from tick 0 */
#define TST_WF_STARVE_TICK (TST_STARVE_PASS - 1)
#define TST_SLIDE_BREAK    80  /* Only tick on which h_slide() reports healthy */
#define TST_RUN            200 /* Ticks in the core spawn-grace scenarios */
#define TST_RUN_GRACE      300 /* Ticks in the asic spawn-grace scenarios */
/* Last tick inside the core spawn grace */
#define TST_CORE_GRACE_LAST ((int)(WDT_CORE_GRACE_MS / TST_TICK_MS) - 1)
/* Last tick inside the asic spawn grace */
#define TST_ASIC_GRACE_LAST ((int)(WDT_ASIC_GRACE_MS / TST_TICK_MS) - 1)
#define TST_STARVE_S        ((int)(WDT_STARVE_MS / 1000u))
#define TST_THREAD_PASS     195  /* Supervisor passes in the threaded scenarios */
#define TST_THREAD_FAST_MS  5000 /* Short feed period of the threaded scenario, ms */
/* Feed ticks after the entry feed in TST_THREAD_PASS slices at feed period p, ms */
#define TST_THREAD_TICKS(p) ((TST_THREAD_PASS - 1) / (int)((p) / (int)WDT_THREAD_SLICE_MS))
#define TST_BEAT_STEPS      150 /* Clock steps of TST_TICK_MS in the single-beat scenario */
/* Feed ticks one beat keeps the gate fed for */
#define TST_BEAT_FED      ((int)(WDT_STARVE_MS / WDT_FEED_PERIOD_MS_MAX))
#define TST_MASK_ALL      ((uint32_t)((1u << WDT_TID_TOTAL) - 1u))
#define TST_RESPAWN_TICKS 20 /* Ticks between asic respawns in the respawn scenarios */
/* Ticks the asic grace budget covers at one respawn per TST_RESPAWN_TICKS */
#define TST_BUDGET_LAST ((int)WDT_GRACE_TRIES_MAX * TST_RESPAWN_TICKS)
/* Ticks per core wedge-and-respawn cycle */
#define TST_CORE_CYCLE_TICKS ((int)((WDT_CORE_WEDGE_MS + RESTART_MS) / TST_TICK_MS))
/* Tick of the last core respawn the grace budget covers */
#define TST_CORE_LAST_SPAWN (((int)WDT_GRACE_TRIES_MAX - 1) * TST_CORE_CYCLE_TICKS)
/* Last tick of the core grace budget */
#define TST_CORE_BUDGET_END (TST_CORE_LAST_SPAWN + TST_CORE_GRACE_LAST)
/* Ticks in the core wedge-cycle scenario */
#define TST_CORE_CYCLE_RUN (TST_CORE_BUDGET_END + TST_STARVE_PASS + 20)
#define TST_SOFT_PERIOD    20 /* Ticks per asic softoff/respawn cycle */
#define TST_SOFT_OFF_TICK  10 /* Tick within a cycle on which the API softoff starts */
#define TST_SOFT_CYCLES    3  /* asic softoff cycles */
/* Tick of the last asic respawn */
#define TST_SOFT_LAST_SPAWN (TST_SOFT_CYCLES * TST_SOFT_PERIOD)
/* Last tick of the last asic spawn grace */
#define TST_SOFT_GRACE_LAST (TST_SOFT_LAST_SPAWN + TST_ASIC_GRACE_LAST)
/* Ticks in the asic softoff-cycle scenario */
#define TST_SOFT_RUN (TST_SOFT_GRACE_LAST + TST_STARVE_PASS + 20)
/* First unfed tick if the softoff respawns had spent the grace budget */
#define TST_SOFT_SPENT_UNFED   (TST_SOFT_LAST_SPAWN - 1 + TST_STARVE_PASS)
#define TST_CORE_SOFT_PERIOD   10 /* Ticks per core thermal-hold/respawn cycle */
#define TST_CORE_SOFT_OFF_TICK 5  /* Tick within a cycle on which the thermal hold starts */
/* Tick on which the core grace budget is refused */
#define TST_CORE_SOFT_SPENT ((int)WDT_GRACE_TRIES_MAX * TST_CORE_SOFT_PERIOD)
/* Ticks in the core thermal-cycle scenario */
#define TST_CORE_SOFT_RUN (TST_CORE_SOFT_SPENT + TST_STARVE_PASS + 20)
/* Enabled set while core_miner is in its spawn grace */
#define TST_A3_ENABLED (TST_MASK_ALL & ~(1u << WDT_TID_CORE))
/* Gate published with only SUPERVISOR marked and TST_A3_ENABLED */
#define TST_A3_GATE \
    ((uint32_t)(1u << WDT_TID_SUPERVISOR) | (TST_A3_ENABLED << WDT_FLAGS_ENABLED_SHIFT))

pid_t       tst_fork(void);
int         tst_chdir(const char *dir);
int         tst_system(const char *cmd);
int         tst_kill(pid_t pid, int sig);
pid_t       tst_waitpid(pid_t pid, int *status, int options);
int         tst_stat(const char *path, struct stat *st);
int         tst_pthread_create(pthread_t *t, void *(*fn)(void *), void *arg);
int         tst_pthread_join(pthread_t t, void **ret);
int         tst_nanosleep(const struct timespec *req, struct timespec *rem);
int         tst_sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *ts);
void        tst_publish(uint32_t gate);
static void tst_log(osif_loglvl_t lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define fork()           tst_fork()                 /* Counts spawns; forks only if tst_fork_real */
#define chdir(d)         tst_chdir(d)               /* Signals readiness, then blocks */
#define system(c)        tst_system(c)              /* Does nothing */
#define kill(p, s)       tst_kill((p), (s))         /* Records SIGKILLs */
#define waitpid(p, s, o) tst_waitpid((p), (s), (o)) /* Scripted reaps */
#define stat(p, b)       tst_stat((p), (b))         /* Always fails */
#define nanosleep(r, m)  tst_nanosleep((r), (m))    /* Steps the clock or the lockstep */
/* Optionally polls, then sleeps */
#define sigtimedwait(s, i, t) tst_sigtimedwait((s), (i), (t))
#define osif_log(...)         tst_log(__VA_ARGS__) /* Records ERR lines */
/* Only mm_wdt_thread is started */
#define pthread_create(t, a, f, arg) tst_pthread_create((t), (f), (arg))
/* Reaps the watchdog thread */
#define pthread_join(t, r)    tst_pthread_join((t), (r))
#define mm_wdt_sig_publish(g) tst_publish(g) /* Records the gate, then publishes */

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
#include "mm_miner_tu.h"
#undef mm_wdt_sig_publish
#undef pthread_join
#undef pthread_create
#undef osif_log
#undef sigtimedwait
#undef nanosleep
#undef stat
#undef waitpid
#undef kill
#undef system
#undef chdir
#undef fork

_Static_assert(WDT_CORE_WEDGE_MS < WDT_STARVE_MS,
        "the wedged-core SIGKILL deadline must fall due BEFORE the "
        "watchdog starve budget expires, which is the whole reason 60 s "
        "was chosen over 180 s: order them the other way "
        "and the dog has already reset the SoC by the time the kill is "
        "scheduled, so the wedge detector is dead code and a wedged "
        "core_miner costs a reboot instead of a respawn");
_Static_assert(WDT_CORE_WEDGE_MS + RESTART_MS - WDT_CORE_GRACE_MS < WDT_STARVE_MS,
        "the CORE leg of the watchdog feed gate is NOT a bare TELEM_STALE_MS + "
        "WDT_STARVE_MS trip wire on message arrival, and this is the "
        "arithmetic that says so. core_live is derived from the arrival "
        "of MSG_CORE_STATS and nothing else, so anything that stops those "
        "datagrams reaching mm_telem_ingest_core — not only a core_miner "
        "that has genuinely wedged — clears the bit. What bounds it is "
        "the supervisor: a core_live that has been false for "
        "WDT_CORE_WEDGE_MS earns a SIGKILL, the reap branch respawns "
        "RESTART_MS later, and the respawn opens WDT_CORE_GRACE_MS in "
        "which wdt_flags_enable() takes CORE OUT of the gate's enabled "
        "set, so the gate completes on the remaining TIDs and the dog is "
        "fed. The longest run with no feed is therefore one wedge cycle "
        "minus that window, and it must stay under the starve budget — "
        "otherwise the cycle that is supposed to recover a silent core "
        "resets the board instead, and the 189 s figure becomes real");

/* Per-tick health script: sets the liveness inputs for pass */
typedef void (*tst_health_fn)(int pass);

static uint32_t tst_gate;                  /* Last packed gate published */
static int      tst_net_up;                /* 1: net_state_get() reports the network up */
static uint64_t tst_now;                   /* Simulated clock, ms */
static int      tst_pass;                  /* Current supervisor pass */
static int      tst_want;                  /* Passes before g_run is cleared */
static int      tst_sleeps, tst_runaway;   /* Sleeps so far; runaway flag */
static uint64_t tst_pass_ms[TST_MAX_PASS]; /* Simulated time at the start of each pass, ms */
static int      tst_wdt_feed_calls;
/* nanosleep() calls, call that clears g_run (0 never), runaway flag */
static int  tst_ns_n, tst_ns_stop_at, tst_ns_runaway;
static long tst_ns_ms[TST_NS_MAX]; /* Requested time of each nanosleep() call, ms */
/* tst_ns_n and feed calls when the last pass ended */
static int tst_ns_at_stop, tst_feed_at_stop;
/* mm_telem_get() inputs: poll fresh, asic live, core live */
static int           tst_fresh, tst_asic_live, tst_core_live;
static tst_health_fn tst_health;
static const char   *tst_ev[TST_EV_MAX]; /* Event log, in call order */
static int           tst_ev_n;
/* 1: the watchdog thread is real and runs in lockstep */
static int             tst_real_thread;
static pthread_mutex_t tst_step_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  tst_step_cv;
static int             tst_step_turn;    /* Lockstep turn, TST_TURN_SUP or TST_TURN_WDT */
static int             tst_step_release; /* 1 lets the watchdog thread run to its exit */
static int             tst_wdt_up;       /* Watchdog thread reached its first slice */
static int             tst_wdt_live;     /* Real watchdog thread is running */
static int             tst_wdt_done;     /* Watchdog thread body returned */
static int             tst_wdt_joined;   /* Watchdog thread reaped by tst_wdt_reap() */
static pthread_t       tst_wdt_real;
static void *(*tst_wdt_body)(void *);      /* Thread body tst_wdt_tramp() runs */
static int tst_deadline_s = WDT_TIMEOUT_S; /* Value wdt_dev_deadline_s() returns, s */
/* Value wdt_dev_feed_period_ms() returns, ms */
static int tst_feed_period = WDT_FEED_PERIOD_MS_MAX;
/* Pipe the disarm, rail-off and reboot stubs trace to; -1 none */
static int      tst_pipe_fd = -1;
static int      tst_rail_fault;           /* 1: board_power_rail_off() raises SIGBUS */
static int      tst_wf_tick = -1;         /* Current wdt_tick_run() tick; -1 outside a run */
static int      tst_wf_fed[TST_MAX_PASS]; /* Per tick: 1 if the dog was fed */
static uint64_t tst_feed_ms[TST_NS_MAX];  /* Simulated time of each feed, ms */
/* SIGUSR2 blocked at the last threaded feed; -1 not sampled */
static int      tst_feed_usr2_blocked = -1;
static pid_t    tst_next_pid          = 4000;       /* PID the next tst_fork() returns */
static int      tst_spawn_core_n, tst_spawn_asic_n; /* core_miner and asic_miner spawns */
static pid_t    tst_core_pid;                       /* PID of the last core_miner spawn */
static int      tst_fork_real;                      /* 1: tst_fork() really forks */
static int      tst_chdir_fd = -1;    /* Pipe tst_chdir() signals readiness on; -1 none */
static int      tst_reboot_n;         /* mm_system_reboot() calls */
static int      tst_reboot_tick = -1; /* tst_wf_tick at the first reboot; -1 none */
static uint32_t tst_reboot_sub;       /* Subreason of the first reboot */
static int      tst_sigkill_n;
static uint64_t tst_sigkill_ms[TST_KILL_MAX];     /* Simulated time of each SIGKILL, ms */
static pid_t    tst_sigkill_pid[TST_KILL_MAX];    /* Target of each SIGKILL */
static int      tst_wdt_create_n, tst_wdt_join_n; /* Watchdog thread creates and joins */
/* 1: tst_sigtimedwait() first takes a pending signal */
static int tst_sigwait_real;
/* 1: every other waitpid() reaps the newest child; phase */
static int            tst_reap_alt, tst_reap_turn;
static wdt_flags_t    tst_wf;                      /* Gate state for direct mm_wdt_tick() runs */
static wdt_grace_t    tst_wf_grace[WDT_TID_TOTAL]; /* Per-TID grace state for those runs */
static uint64_t       tst_wf_healthy_at;           /* Time of the last complete gate, ms */
static fault_report_t tst_wf_missing;              /* Missing-TID report state for those runs */
/* Per tick: CORE and ASIC in the enabled set */
static int tst_wf_on_core[TST_MAX_PASS], tst_wf_on_asic[TST_MAX_PASS];
/* Per tick: core and asic named missing */
static int  tst_wf_named_core[TST_MAX_PASS], tst_wf_named_asic[TST_MAX_PASS];
static char tst_last_err[512];
/* ERR lines, starve reports, then feeds and ERR lines of the last starve run */
static int tst_err_n, tst_starve_err_n, tst_starve_feeds, tst_starve_errs;
static int tst_prod_joined;       /* 1: mm reaped the watchdog thread itself */
static int tst_usr2_run      = 1; /* g_run in the SIGUSR2 child */
static int tst_usr2_released = 1; /* g_wdt_released in the SIGUSR2 child */

extern uint64_t tst_vf_beat_ms;

/* The V/F beat the stub reports follows the simulated clock until tick tst_vf_stop_tick */
static int tst_vf_stop_tick;

void tst_publish(uint32_t gate)
{
    tst_gate = gate;
    mm_wdt_sig_publish(gate);
}

int net_state_get(void)
{
    return tst_net_up ? 2 : 0;
}
int net_state_ip(char *ip, size_t cap)
{
    if (!tst_net_up || !ip || !cap)
        return -1;
    snprintf(ip, cap, "10.0.0.1");
    return 0;
}

static void tst_net_raise(void)
{
    tst_net_up = 1;
}

static void ev(const char *what)
{
    if (tst_ev_n < TST_EV_MAX)
        tst_ev[tst_ev_n++] = what;
}
static const char *ev_str(void)
{
    static char b[256];
    b[0] = '\0';
    for (int i = 0; i < tst_ev_n; i++) {
        if (i)
            strncat(b, ",", sizeof b - strlen(b) - 1);
        strncat(b, tst_ev[i], sizeof b - strlen(b) - 1);
    }
    return b;
}
static int ev_is(int from, const char *const *want, int n)
{
    if (tst_ev_n < from + n)
        return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(tst_ev[from + i], want[i]))
            return 0;
    return 1;
}
static int ev_is_tail(const char *const *want, int n)
{
    return tst_ev_n >= n && ev_is(tst_ev_n - n, want, n);
}

static void tst_step_deadline(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += TST_STEP_WAIT_S;
}

static void tst_step_stuck(const char *what)
{
    fprintf(stderr,
            "  FAIL: the watchdog thread harness waited %d s for %s and the "
            "watchdog thread never got there — it is not sleeping its period "
            "in %u ms slices; aborting\n",
            TST_STEP_WAIT_S, what, WDT_THREAD_SLICE_MS);
    _exit(1);
}

static void tst_step_hand(uint32_t ms)
{
    struct timespec dl;

    tst_step_deadline(&dl);
    pthread_mutex_lock(&tst_step_lk);
    tst_now += ms;
    tst_step_turn = TST_TURN_WDT;
    pthread_cond_broadcast(&tst_step_cv);
    while (tst_step_turn != TST_TURN_SUP && !tst_wdt_done)
        if (pthread_cond_timedwait(&tst_step_cv, &tst_step_lk, &dl) == ETIMEDOUT) {
            pthread_mutex_unlock(&tst_step_lk);
            tst_step_stuck("the slice it was handed");
        }
    pthread_mutex_unlock(&tst_step_lk);
}

static void tst_step_slice(void)
{
    pthread_mutex_lock(&tst_step_lk);
    tst_wdt_up    = 1;
    tst_step_turn = TST_TURN_SUP;
    pthread_cond_broadcast(&tst_step_cv);
    while (tst_step_turn != TST_TURN_WDT && !tst_step_release)
        pthread_cond_wait(&tst_step_cv, &tst_step_lk);
    pthread_mutex_unlock(&tst_step_lk);
}

static void *tst_wdt_tramp(void *arg)
{
    void *r = tst_wdt_body(arg);

    pthread_mutex_lock(&tst_step_lk);
    tst_wdt_done = 1;
    pthread_cond_broadcast(&tst_step_cv);
    pthread_mutex_unlock(&tst_step_lk);
    return r;
}

static void tst_wdt_spawn(void *(*fn)(void *), void *arg)
{
    struct timespec dl;

    tst_wdt_body     = fn;
    tst_step_turn    = TST_TURN_SUP;
    tst_step_release = 0;
    tst_wdt_up       = 0;
    tst_wdt_done     = 0;
    tst_wdt_joined   = 0;
    if (pthread_create(&tst_wdt_real, NULL, tst_wdt_tramp, arg) != 0) {
        fprintf(stderr, "  FAIL: the harness could not start a real watchdog thread\n");
        _exit(1);
    }
    tst_step_deadline(&dl);
    pthread_mutex_lock(&tst_step_lk);
    while (!tst_wdt_up && !tst_wdt_done)
        if (pthread_cond_timedwait(&tst_step_cv, &tst_step_lk, &dl) == ETIMEDOUT) {
            pthread_mutex_unlock(&tst_step_lk);
            tst_step_stuck("its first slice");
        }
    pthread_mutex_unlock(&tst_step_lk);
    tst_wdt_live = 1;
}

static void tst_wdt_reap(void)
{
    pthread_mutex_lock(&tst_step_lk);
    tst_step_release = 1;
    pthread_cond_broadcast(&tst_step_cv);
    pthread_mutex_unlock(&tst_step_lk);
    pthread_join(tst_wdt_real, NULL);
    tst_wdt_live   = 0;
    tst_wdt_joined = 1;
}

uint64_t osif_now_ms(void)
{
    return tst_now;
}
void osif_sleep_ms(uint32_t ms)
{
    if (tst_real_thread && tst_wdt_live)
        tst_step_hand(ms);
    else
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
        tst_pass_ms[tst_pass] = tst_now;
    tst_health(tst_pass);
    if (++tst_pass >= tst_want) {
        g_run            = 0;
        tst_ns_at_stop   = tst_ns_n;
        tst_feed_at_stop = tst_wdt_feed_calls;
    }
}

void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
    out->poll_fresh = tst_fresh;
    out->ps_vout_mv = POWER_VALID_VOLT_MIN;
    out->asic_live  = tst_asic_live;
    out->in_work    = tst_asic_live;
    out->core_live  = tst_core_live;
    out->tmax       = 60;
    out->tavg       = 55;
    if (CAP_HB_MCU)
        out->hb_count = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++)
        out->hb_outlet[b] = out->hb_inlet[b] = 60;
}

int wdt_dev_init(void)
{
    ev("wdt-init");
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
    return tst_deadline_s;
}
int wdt_dev_feed_period_ms(void)
{
    return tst_feed_period;
}
static void tst_pipe_put(const char *str)
{
    if (tst_pipe_fd >= 0) {
        ssize_t w = write(tst_pipe_fd, str, strlen(str));
        (void)w;
    }
}
void wdt_dev_disarm(void)
{
    ev("disarm");
    tst_pipe_put("d");
}
void wdt_dev_feed(void)
{
    if (tst_real_thread) {
        sigset_t m;

        pthread_sigmask(SIG_BLOCK, NULL, &m);
        tst_feed_usr2_blocked = sigismember(&m, SIGUSR2);
    }
    if (tst_wdt_feed_calls < TST_NS_MAX)
        tst_feed_ms[tst_wdt_feed_calls] = tst_now;
    tst_wdt_feed_calls++;
    if (tst_wf_tick >= 0 && tst_wf_tick < TST_MAX_PASS)
        tst_wf_fed[tst_wf_tick] = 1;
}

int tst_children_stop(const pid_t *pids, int n, unsigned term_ms, unsigned kill_ms)
{
    (void)pids;
    (void)n;
    (void)term_ms;
    (void)kill_ms;
    ev("join");
    return 0;
}

pid_t tst_fork(void)
{
    if (tst_fork_real)
        return fork();
    if (g_enum_hold.phase == ENUM_HOLD_PHASE_ARMED)
        tst_spawn_asic_n++;
    else {
        tst_spawn_core_n++;
        tst_core_pid = tst_next_pid;
    }
    ev("spawn");
    return tst_next_pid++;
}
int tst_chdir(const char *dir)
{
    ssize_t w;

    (void)dir;
    w = write(tst_chdir_fd, "c", 1);
    (void)w;
    for (;;)
        pause();
}
int board_power_rail_on(uint16_t counts)
{
    (void)counts;
    ev("rail-on");
    return 0;
}
int board_power_rail_off(void)
{
    ev("rail-off");
    tst_pipe_put("r");
    if (tst_rail_fault)
        raise(SIGBUS);
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
    return 0;
}

int mm_system_reboot(uint8_t reason, uint32_t sub)
{
    char rec[32];

    snprintf(rec, sizeof rec, "R%02x:%08x;", (unsigned)reason, (unsigned)sub);
    tst_pipe_put(rec);
    if (!tst_reboot_n) {
        tst_reboot_tick = tst_wf_tick;
        tst_reboot_sub  = sub;
    }
    tst_reboot_n++;
    ev("reboot");
    return 0;
}

int tst_kill(pid_t pid, int sig)
{
    if (sig == SIGKILL) {
        if (tst_sigkill_n < TST_KILL_MAX) {
            tst_sigkill_ms[tst_sigkill_n]  = tst_now;
            tst_sigkill_pid[tst_sigkill_n] = pid;
        }
        tst_sigkill_n++;
    }
    return 0;
}

int tst_pthread_create(pthread_t *t, void *(*fn)(void *), void *arg)
{
    (void)arg;
    if (fn != mm_wdt_thread)
        return -1;
    tst_wdt_create_n++;
    memset(t, 0, sizeof *t);
    if (tst_real_thread)
        tst_wdt_spawn(fn, arg);
    return 0;
}
int tst_pthread_join(pthread_t t, void **ret)
{
    (void)t;
    (void)ret;
    tst_wdt_join_n++;
    ev("wdt-join");
    if (tst_real_thread)
        tst_wdt_reap();
    return 0;
}

int tst_nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    long ms = (long)req->tv_sec * 1000 + req->tv_nsec / 1000000L;
    if (tst_ns_n < TST_NS_MAX)
        tst_ns_ms[tst_ns_n] = ms;
    tst_ns_n++;
    if (tst_real_thread) {
        if (tst_ns_n > TST_STEP_MAX) {
            tst_ns_runaway = 1;
            g_run          = 0;
        }
        tst_step_slice();
        return 0;
    }
    tst_now += (uint64_t)ms;
    if (tst_ns_stop_at && tst_ns_n >= tst_ns_stop_at)
        g_run = 0;
    if (tst_ns_n >= TST_NS_MAX) {
        tst_ns_runaway = 1;
        g_run          = 0;
    }
    return 0;
}

int tst_sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *ts)
{
    if (tst_sigwait_real) {
        struct timespec none = { 0, 0 };
        int             sig  = sigtimedwait(set, info, &none);

        if (sig > 0)
            return sig;
    }
    tst_nanosleep(ts, NULL);
    errno = EAGAIN;
    return -1;
}

pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    if (status)
        *status = 0;
    if (tst_reap_alt && tst_pass >= 2 && pid == tst_next_pid - 1 &&
            (tst_reap_turn = !tst_reap_turn))
        return pid;
    return 0;
}
int tst_stat(const char *path, struct stat *st)
{
    (void)path;
    if (st)
        memset(st, 0, sizeof *st);
    return -1;
}

static void scenario(int passes, tst_health_fn h, int api_req)
{
    tst_now            = 0;
    tst_pass           = 0;
    tst_want           = passes;
    tst_health         = h;
    tst_sleeps         = 0;
    tst_runaway        = 0;
    tst_ev_n           = 0;
    tst_reap_turn      = 0;
    tst_spawn_core_n   = 0;
    tst_spawn_asic_n   = 0;
    tst_wdt_feed_calls = 0;
    tst_sigkill_n      = 0;
    tst_wdt_create_n   = 0;
    tst_wdt_join_n     = 0;
    tst_reboot_n       = 0;
    memset(tst_pass_ms, 0, sizeof tst_pass_ms);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_api_req           = api_req;
    g_run               = 1;
    tst_net_raise();
    mm_miner_main_unused();
}

static void h_healthy(int p)
{
    (void)p;
    tst_fresh     = 1;
    tst_asic_live = 1;
}

static uint32_t tst_gate_mask(void)
{
    wdt_flags_t g;

    wdt_flags_unpack(&g, tst_gate);
    return wdt_flags_raw(&g);
}

static uint32_t tst_gate_enabled(void)
{
    wdt_flags_t g;

    wdt_flags_unpack(&g, tst_gate);
    return wdt_flags_enabled_mask(&g);
}

static void tst_gate_missing(char *buf, size_t cap)
{
    wdt_flags_t g;

    wdt_flags_unpack(&g, tst_gate);
    wdt_flags_missing(&g, buf, cap);
}

static void wdt_tick_reset(void)
{
    wdt_flags_reset(&tst_wf);
    memset(tst_wf_grace, 0, sizeof tst_wf_grace);
    tst_wf_healthy_at = 0;
    memset(&tst_wf_missing, 0, sizeof tst_wf_missing);
    tst_wf_tick = -1;
    memset(tst_wf_fed, 0, sizeof tst_wf_fed);
    memset(tst_wf_on_core, 0, sizeof tst_wf_on_core);
    memset(tst_wf_on_asic, 0, sizeof tst_wf_on_asic);
    memset(tst_wf_named_core, 0, sizeof tst_wf_named_core);
    memset(tst_wf_named_asic, 0, sizeof tst_wf_named_asic);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    atomic_store(&g_core_spawn_at, 0);
    atomic_store(&g_asic_spawn_at, 0);
    g_fault_off = 0;
    tst_fresh = tst_asic_live = tst_core_live = 0;
    g_wdt_beat                                = 0;
    tst_now                                   = 0;
}

static void wdt_tick_run(int n, tst_health_fn h)
{
    for (int i = 0; i < n; i++) {
        char named[64];

        if (h)
            h(i);
        tst_wf_tick = i;
        tst_now     = (uint64_t)(i + 1) * TST_TICK_MS;
        mm_wdt_tick(&tst_wf, tst_wf_grace, &tst_wf_healthy_at, &tst_wf_missing, tst_now);
        if (i >= TST_MAX_PASS)
            continue;
        wdt_flags_missing(&tst_wf, named, sizeof named);
        tst_wf_on_core[i]    = wdt_flags_enabled(&tst_wf, WDT_TID_CORE);
        tst_wf_on_asic[i]    = wdt_flags_enabled(&tst_wf, WDT_TID_ASIC);
        tst_wf_named_core[i] = strstr(named, "core") != NULL;
        tst_wf_named_asic[i] = strstr(named, "asic") != NULL;
    }
}

static int wf_first_unfed(int from, int to)
{
    for (int i = from; i < to; i++)
        if (!tst_wf_fed[i])
            return i;
    return -1;
}
static int wf_first_fed(int from, int to)
{
    for (int i = from; i < to; i++)
        if (tst_wf_fed[i])
            return i;
    return -1;
}
static int wf_first_set(const int *v, int from, int to)
{
    for (int i = from; i < to; i++)
        if (v[i])
            return i;
    return -1;
}
static int wf_first_clear(const int *v, int from, int to)
{
    for (int i = from; i < to; i++)
        if (!v[i])
            return i;
    return -1;
}

static void h_dead(int p)
{
    (void)p;
    tst_fresh     = 0;
    tst_asic_live = 0;
    tst_core_live = 0;
    g_wdt_beat    = 0;
}
static void h_wf_healthy(int p)
{
    (void)p;
    tst_fresh     = 1;
    tst_asic_live = 1;
    tst_core_live = 1;
    g_wdt_beat    = 1;
}
static void h_asic_gone(int p)
{
    (void)p;
    tst_fresh     = 1;
    tst_asic_live = 0;
    tst_core_live = 1;
    g_wdt_beat    = 1;
}
static void h_parked(int p)
{
    (void)p;
    tst_fresh     = 1;
    tst_asic_live = 0;
    tst_core_live = 1;
    g_wdt_beat    = 1;
}

static void h_slide(int p)
{
    int ok        = (p == TST_SLIDE_BREAK);
    tst_fresh     = ok;
    tst_asic_live = ok;
    tst_core_live = ok;
    g_wdt_beat    = ok;
}

static void h_core_gone(int p)
{
    (void)p;
    tst_fresh     = 1;
    tst_asic_live = 1;
    tst_core_live = 0;
    g_wdt_beat    = 1;
}

static void h_vf(int p)
{
    h_wf_healthy(p);
    if (p <= tst_vf_stop_tick)
        tst_vf_beat_ms = (uint64_t)(p + 1) * TST_TICK_MS;
}

static void tst_vf_stall(void)
{
    int u, stall_tick, last_ok;

    wdt_tick_reset();
    tst_vf_stop_tick = TST_RUN;
    wdt_tick_run(TST_RUN, h_vf);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "a V/F thread that keeps ticking keeps the supervisor leg "
            "reporting: fed for all %d ticks (first unfed %d)",
            TST_RUN, u);

    wdt_tick_reset();
    tst_err_n        = 0;
    tst_vf_stop_tick = 10;
    wdt_tick_run(TST_RUN, h_vf);
    tst_vf_beat_ms = 0;
    /* Its last beat is at tick 10's clock; the leg drops once the gap passes
     * WDT_VF_STALL_MS, and the dog starves WDT_STARVE_MS after the last report */
    stall_tick = 10 + (int)(WDT_VF_STALL_MS / TST_TICK_MS) + 1;
    last_ok    = stall_tick - 1;
    u          = wf_first_unfed(0, TST_RUN);
    CHECK(u == last_ok + TST_STARVE_PASS,
            "a V/F thread that stops ticking while the main loop still beats takes "
            "the supervisor leg out after %u ms, so the dog starves %u ms later: "
            "first unfed tick %d (saw %d)",
            WDT_VF_STALL_MS, WDT_STARVE_MS, last_ok + TST_STARVE_PASS, u);
    CHECK(tst_err_n == 1 && strstr(tst_last_err, "V/F control has not run") != NULL,
            "...and the stall is logged once as an ERR (%d ERR, last \"%s\")", tst_err_n,
            tst_last_err);

    wdt_tick_reset();
    tst_vf_beat_ms = 0;
    wdt_tick_run(TST_RUN, h_wf_healthy);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "with no V/F thread running (beat 0) only the main loop beat "
            "counts: fed for all %d ticks (first unfed %d)",
            TST_RUN, u);
}

static void tst_spawn_grace(void)
{
    int u, a, b;

    wdt_tick_reset();
    atomic_store(&g_core_spawn_at, 1);
    wdt_tick_run(TST_RUN, h_core_gone);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_CORE_GRACE_LAST + TST_STARVE_PASS,
            "a core_miner that has not answered yet is NOT SUPERVISED for %u ms "
            "from its spawn stamp: the last unsupervised tick is %d, the gate "
            "completes on the TIDs that remain, and the %u ms starve window runs "
            "from THERE, so the first unfed tick is %d (saw %d). Pass a literal 1 "
            "in place of `!core_starting` to wdt_flags_enable() and the dog starves "
            "at tick %d, %d ticks earlier, on a core_miner that is merely still "
            "starting",
            WDT_CORE_GRACE_MS, TST_CORE_GRACE_LAST, WDT_STARVE_MS,
            TST_CORE_GRACE_LAST + TST_STARVE_PASS, u, TST_WF_STARVE_TICK, TST_CORE_GRACE_LAST + 1);

    a = wf_first_set(tst_wf_on_core, 0, TST_CORE_GRACE_LAST + 1);
    CHECK(a < 0 && tst_wf_on_core[TST_CORE_GRACE_LAST + 1],
            "...and the window is expressed as NOT SUPERVISED, not as a bit marked "
            "on the child's behalf: wdt_flags_enabled() reads 0 for CORE on every "
            "tick 0..%d and 1 from tick %d (first supervised tick inside the window "
            "%d, enabled at tick %d reads %d). Marking the bit instead buys the same feed "
            "decision from a false statement — that a child which has said nothing "
            "has reported in",
            TST_CORE_GRACE_LAST, TST_CORE_GRACE_LAST + 1, a, TST_CORE_GRACE_LAST + 1,
            tst_wf_on_core[TST_CORE_GRACE_LAST + 1]);

    a = wf_first_set(tst_wf_named_core, 0, TST_CORE_GRACE_LAST + 1);
    b = wf_first_clear(tst_wf_named_core, TST_CORE_GRACE_LAST + 1, TST_RUN);
    CHECK(a < 0 && b < 0,
            "...and THAT is what the starving-watchdog diagnostic is for: "
            "wdt_flags_missing() never names `core` across ticks 0..%d while the "
            "child is starting (first named %d) and names it on every tick from %d "
            "once it is supervised (first silent %d). A marked TID is never listed, "
            "so marking the bit instead prints nothing in the one window where the "
            "name is the whole explanation. Delete the `if (!(f->enabled & bit)) continue;` "
            "skip in wdt_flags_missing() and `core` is named from tick 0 — the gate "
            "completes there, and wdt_flags_mark() clears the WHOLE mask when it "
            "does, so every TID reads as missing",
            TST_CORE_GRACE_LAST, a, TST_CORE_GRACE_LAST + 1, b);

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_core_gone);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_WF_STARVE_TICK && tst_wf_on_core[0],
            "a core_miner that was NEVER spawned is supervised from tick 0 (saw "
            "enabled %d) and gets no window at all — the stamp is 0 and the term is "
            "guarded on it, so the dog starves on the ordinary schedule (tick %d, "
            "saw %d). Drop the `core_at &&` guard and a zero stamp reads as a spawn "
            "at t=0, leaving a child that does not exist unsupervised for %u ms",
            tst_wf_on_core[0], TST_WF_STARVE_TICK, u, WDT_CORE_GRACE_MS);

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 1);
    wdt_tick_run(TST_RUN_GRACE, h_asic_gone);
    u = wf_first_unfed(0, TST_RUN_GRACE);
    CHECK(u == TST_ASIC_GRACE_LAST + TST_STARVE_PASS,
            "asic_miner gets the same treatment on a longer clock — %u ms, because "
            "enumeration and the PLL ramp take minutes: last unsupervised tick %d, "
            "first unfed tick %d (saw %d). Same mutation, the other TID: a literal "
            "1 for `!asic_starting`",
            WDT_ASIC_GRACE_MS, TST_ASIC_GRACE_LAST, TST_ASIC_GRACE_LAST + TST_STARVE_PASS, u);

    a = wf_first_set(tst_wf_on_asic, 0, TST_ASIC_GRACE_LAST + 1);
    b = wf_first_set(tst_wf_named_asic, 0, TST_ASIC_GRACE_LAST + 1);
    CHECK(a < 0 && b < 0 && tst_wf_on_asic[TST_ASIC_GRACE_LAST + 1] &&
                    tst_wf_named_asic[TST_ASIC_GRACE_LAST + 1],
            "...and the ASIC side reads the same way at both seams: across ticks "
            "0..%d wdt_flags_enabled() is 0 and wdt_flags_missing() does not name "
            "`asic` (first supervised %d, first named %d), and on tick %d both flip "
            "together (enabled %d, named %d). A starting child is invisible to the "
            "diagnostic because it is invisible to the gate — one fact, not two",
            TST_ASIC_GRACE_LAST, a, b, TST_ASIC_GRACE_LAST + 1,
            tst_wf_on_asic[TST_ASIC_GRACE_LAST + 1], tst_wf_named_asic[TST_ASIC_GRACE_LAST + 1]);

    wdt_tick_reset();
    wdt_tick_run(TST_RUN_GRACE, h_asic_gone);
    u = wf_first_unfed(0, TST_RUN_GRACE);
    CHECK(u == TST_WF_STARVE_TICK && tst_wf_on_asic[0],
            "and the same zero-stamp guard on the ASIC side: never spawned is not "
            "the same as just spawned, so the TID is supervised from tick 0 (saw "
            "enabled %d) and the dog starves at tick %d (saw %d). Without the "
            "`asic_at &&` guard the first unfed tick moves to %d — over three "
            "minutes of free feeding for a child that was never started",
            tst_wf_on_asic[0], TST_WF_STARVE_TICK, u, TST_ASIC_GRACE_LAST + TST_STARVE_PASS);
}

static void h_core_alive(int p)
{
    (void)p;
    tst_fresh     = 1;
    tst_asic_live = 1;
    tst_core_live = 1;
}

static void h_core_gone_thermal(int p)
{
    (void)p;
    tst_fresh                 = 1;
    tst_asic_live             = 1;
    tst_core_live             = 0;
    g_softonoff.thermal_until = (uint64_t)TST_RUN * TST_TICK_MS * 10;
}

static void tst_core_wedge(void)
{
    scenario(TST_RUN, h_core_gone, 0);
    CHECK(tst_sigkill_n == 3 && tst_sigkill_ms[0] == WDT_CORE_WEDGE_MS &&
                    tst_sigkill_pid[0] == tst_core_pid &&
                    tst_sigkill_ms[1] - tst_sigkill_ms[0] == WDT_CORE_WEDGE_MS &&
                    tst_sigkill_ms[2] - tst_sigkill_ms[1] == WDT_CORE_WEDGE_MS,
            "a core_miner that stops answering for %u ms is SIGKILLed and the "
            "deadline re-armed, so a wedged child is killed once per window and not "
            "once per pass: over %d passes that is 3 kills at %u/%u/%u ms, on the "
            "core pid %d (saw %d kill(s), first at %u ms on pid %d)",
            WDT_CORE_WEDGE_MS, TST_RUN, WDT_CORE_WEDGE_MS, 2 * WDT_CORE_WEDGE_MS,
            3 * WDT_CORE_WEDGE_MS, (int)tst_core_pid, tst_sigkill_n, (unsigned)tst_sigkill_ms[0],
            (int)tst_sigkill_pid[0]);

    CHECK(tst_sigkill_n >= 1 && tst_sigkill_ms[0] < (uint64_t)WDT_STARVE_MS,
            "...and that first kill falls due at %u ms, INSIDE the %u ms starve "
            "budget the same silence is counting down — the ordering the 60 s "
            "figure exists for, and the only thing making the wedge detector "
            "reachable at all. At 180000 the dog has already bitten "
            "and the SIGKILL is scheduled for a board that is no longer running, so "
            "a wedged core_miner costs a reboot instead of a respawn (saw the first "
            "kill at %u ms)",
            WDT_CORE_WEDGE_MS, WDT_STARVE_MS, (unsigned)tst_sigkill_ms[0]);

    scenario(TST_RUN, h_core_alive, 0);
    CHECK(tst_sigkill_n == 0,
            "a core_miner that keeps answering is never SIGKILLed over %d passes — "
            "the deadline is re-stamped on every live snapshot, so the wedge "
            "detector is a silence detector and not a %u ms restart timer (saw %d "
            "kill(s))",
            TST_RUN, WDT_CORE_WEDGE_MS, tst_sigkill_n);

    scenario(TST_RUN, h_core_gone_thermal, 0);
    CHECK(tst_sigkill_n == 3 && tst_sigkill_ms[0] == WDT_CORE_WEDGE_MS &&
                    tst_sigkill_pid[0] == tst_core_pid,
            "a core_miner silent under a THERMAL softoff IS SIGKILLed, on exactly "
            "the same %u ms schedule as any other silence (saw %d kill(s), first at "
            "%u ms on pid %d — want 3, %u ms, pid %d): the thermal soft-off does not SIGTERM "
            "core_miner, so a silent one during an overheat hold is a genuine wedge "
            "and not the state mm asked for. core_miner is the :4028 server, so "
            "exempting it here would leave the API dead for the whole %u ms hold "
            "with nothing left to restart it",
            WDT_CORE_WEDGE_MS, tst_sigkill_n, (unsigned)tst_sigkill_ms[0], (int)tst_sigkill_pid[0],
            WDT_CORE_WEDGE_MS, (int)tst_core_pid, OVERHOT_HOLD_MS);
}

static int tst_sig_child(int sig, char *log, size_t cap, int *termsig)
{
    int     fd[2];
    pid_t   p;
    int     st = 0;
    size_t  n  = 0;
    ssize_t r;

    *termsig = 0;
    log[0]   = '\0';
    if (pipe(fd) != 0)
        return -1;
    fflush(stdout);
    fflush(stderr);
    p = fork();
    if (p < 0) {
        close(fd[0]);
        close(fd[1]);
        return -1;
    }
    if (p == 0) {
        close(fd[0]);
        tst_pipe_fd = fd[1];
        if (sig == SIGUSR2) {
            sigset_t usr2;

            sigemptyset(&usr2);
            sigaddset(&usr2, SIGUSR2);
            pthread_sigmask(SIG_BLOCK, &usr2, NULL);
            kill(getpid(), SIGUSR2);
            tst_sigwait_real = 1;
            tst_real_thread  = 0;
            tst_ns_n         = 0;
            tst_ns_runaway   = 0;
            tst_ns_stop_at   = 1;
            g_run            = tst_usr2_run;
            g_wdt_released   = tst_usr2_released;
            mm_wdt_thread(NULL);
            _exit(9);
        }
        raise(sig);
        _exit(9);
    }
    close(fd[1]);
    while (n + 1 < cap && (r = read(fd[0], log + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    log[n] = '\0';
    close(fd[0]);
    if (waitpid(p, &st, 0) != p)
        return -1;
    if (WIFSIGNALED(st))
        *termsig = WTERMSIG(st);
    return 0;
}

static void tst_pretimeout(void)
{
    uint32_t mask = ((uint32_t)1u << WDT_TID_TOTAL) - 1u;
    uint32_t gate = mask | (mask << WDT_FLAGS_ENABLED_SHIFT);
    char     want[64], log[128] = "";
    int      termsig = 0;

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_wf_healthy);
    snprintf(want, sizeof want, "rR%02x:%08x;r", (unsigned)MC_BOOTBY_WDT, (unsigned)gate);

    CHECK(tst_gate_mask() == mask && tst_sig_child(SIGUSR2, log, sizeof log, &termsig) == 0 &&
                    !strncmp(log, want, strlen(want) - 1),
            "a process-directed SIGUSR2, held pending while every thread blocks it, "
            "is taken by the watchdog thread's own wait and reboots from there: "
            "the rail is cut first, then "
            "mm_system_reboot is called with MC_BOOTBY_WDT and the whole gate the tick last "
            "PUBLISHED as the "
            "subreason — report mask low, supervised set high (want %s, saw %s)",
            want, log);

    CHECK(!strcmp(log, want) && termsig == SIGUSR2,
            "...and when reboot() returns, the pretimeout path falls through to the fatal "
            "exit: the rail is cut once more and the process dies of SIGUSR2 "
            "without wdt_dev_disarm(), so the dog still resets the SoC (saw %s, "
            "signal %d)",
            log, termsig);
}

static void tst_pretimeout_on_stop(void)
{
    char want[16], log[128] = "";
    int  termsig = 0, rc;

    snprintf(want, sizeof want, "rR%02x:", (unsigned)MC_BOOTBY_WDT);
    tst_usr2_run      = 0;
    tst_usr2_released = 0;
    rc                = tst_sig_child(SIGUSR2, log, sizeof log, &termsig);
    CHECK(rc == 0 && !strncmp(log, want, strlen(want)) && termsig == SIGUSR2,
            "a pretimeout that fires after g_run is cleared and before mm_shutdown() "
            "has disarmed the dog is still taken by the watchdog thread and reboots "
            "with MC_BOOTBY_WDT: the thread keeps waiting on SIGUSR2 until "
            "g_wdt_released, it does not leave the signal pending through the "
            "shutdown tail (want %s..., saw %s, signal %d)",
            want, log, termsig);

    tst_usr2_released = 1;
    rc                = tst_sig_child(SIGUSR2, log, sizeof log, &termsig);
    CHECK(rc == 0 && log[0] == '\0' && termsig == 0,
            "...and once g_wdt_released is set the thread returns without "
            "another wait, so the join after mm_shutdown() completes (saw %s, "
            "signal %d)",
            log, termsig);
    tst_usr2_run = 1;
}

static void tst_wdt_thread_loop(void)
{
    unsigned period = (unsigned)wdt_dev_feed_period_ms();
    unsigned slices = period / WDT_THREAD_SLICE_MS;
    uint64_t t0;
    int      bad = -1;

    tst_ns_n           = 0;
    tst_ns_runaway     = 0;
    tst_ns_stop_at     = (int)(2 * slices + slices / 2);
    tst_wdt_feed_calls = 0;
    tst_wf_tick        = -1;
    tst_fresh = tst_asic_live = tst_core_live = 1;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    g_run = 1;
    t0    = tst_now;
    mm_wdt_thread(NULL);

    for (int i = 0; i < tst_ns_n && i < TST_NS_MAX; i++)
        if (tst_ns_ms[i] != (long)WDT_THREAD_SLICE_MS) {
            bad = i;
            break;
        }
    CHECK(!tst_ns_runaway && bad < 0 && slices > 1,
            "the watchdog thread sleeps its %u ms period in %u ms SLICES, never in "
            "one block: every one of %d sleeps asked for exactly %u ms (first "
            "mismatch %d). That is what bounds mm's stop latency — one slice, not "
            "one period",
            period, WDT_THREAD_SLICE_MS, tst_ns_n, WDT_THREAD_SLICE_MS, bad);

    CHECK(tst_wdt_feed_calls == 3 && tst_feed_ms[0] == t0 && tst_feed_ms[1] == t0 + period &&
                    tst_feed_ms[2] == t0 + 2 * period,
            "...and it feeds IMMEDIATELY on entry and then once per whole period, "
            "not once per slice: %d slices produce a feed at +0 ms and 2 ticks at "
            "+%u ms and +%u ms (saw %d feed(s) at +%u/+%u/+%u ms). The period is "
            "the HAL's negotiated one (%d s dog / WDT_FEED_MARGIN %d), read through "
            "wdt_dev_feed_period_ms() rather than written here as a literal",
            tst_ns_stop_at, period, 2 * period, tst_wdt_feed_calls, (unsigned)(tst_feed_ms[0] - t0),
            (unsigned)(tst_feed_ms[1] - t0), (unsigned)(tst_feed_ms[2] - t0), WDT_TIMEOUT_S,
            WDT_FEED_MARGIN);

    CHECK(tst_feed_ms[0] == t0,
            "THE ENTRY FEED IS THE POINT: the dog is armed in main() and nothing "
            "feeds it until this thread exists, so a thread that slept its %u ms "
            "period FIRST added that period to an already unbounded start-up "
            "window — UI start, LED start, channel open, two child spawns, network "
            "provisioning and the OTA listener, all against the dog's own deadline. "
            "Delete the entry feed and the first feed lands at +%u ms instead of "
            "+0 ms",
            period, period);

    tst_ns_n           = 0;
    tst_ns_runaway     = 0;
    tst_ns_stop_at     = 3;
    tst_wdt_feed_calls = 0;
    g_run              = 1;
    mm_wdt_thread(NULL);

    CHECK(tst_ns_n == 3 && tst_wdt_feed_calls == 1,
            "a cleared g_run is noticed WITHIN one slice and costs no TICK feed: "
            "g_run drops on sleep 3 of the period's %u and the thread returns there "
            "(saw %d sleep(s), %d feed(s) — the one being the entry feed, which "
            "happens before any sleep). Without the per-slice re-test mm's stop "
            "path would block on this join for up to %u ms; without the break after "
            "it, a stopping miner would feed the dog one last time",
            slices, tst_ns_n, tst_wdt_feed_calls, period);

    tst_ns_stop_at = 0;
}

static void tst_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    if (lvl != OSIF_LOG_ERR)
        return;
    va_start(ap, fmt);
    vsnprintf(tst_last_err, sizeof tst_last_err, fmt, ap);
    va_end(ap);
    tst_err_n++;
    if (strstr(tst_last_err, "is longer than the"))
        tst_starve_err_n++;
}

static int tst_starve_report(int deadline_s)
{
    tst_deadline_s     = deadline_s;
    tst_starve_err_n   = 0;
    tst_err_n          = 0;
    tst_last_err[0]    = '\0';
    tst_ns_n           = 0;
    tst_ns_runaway     = 0;
    tst_ns_stop_at     = 1;
    tst_wdt_feed_calls = 0;
    tst_wf_tick        = -1;
    tst_fresh = tst_asic_live = tst_core_live = 1;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    g_run = 1;
    mm_wdt_thread(NULL);
    tst_starve_feeds = tst_wdt_feed_calls;
    tst_starve_errs  = tst_err_n;
    tst_ns_stop_at   = 0;
    tst_deadline_s   = WDT_TIMEOUT_S;
    return tst_starve_err_n;
}

static void tst_wdt_starve_vs_deadline(void)
{
    int shipping, shipping_errs, equal, over, over_feeds, absent;

    shipping      = tst_starve_report(WDT_TIMEOUT_S);
    shipping_errs = tst_starve_errs;
    equal         = tst_starve_report(TST_STARVE_S);
    over          = tst_starve_report(TST_STARVE_S + 1);
    over_feeds    = tst_starve_feeds;
    absent        = tst_starve_report(0);

    CHECK(over == 1 && over_feeds == 1,
            "the %ums starve budget is checked against the deadline the DRIVER "
            "granted, and a %d s grant — one second past the budget — is reported "
            "exactly once at ERR while the thread carries on and still feeds on "
            "entry (saw %d report(s), %d feed(s)). This is the only tie between "
            "WDT_STARVE_MS and the hardware: the two _Static_asserts at the head "
            "of that file tie TELEM_STALE_FAILSAFE_MS and WDT_CORE_WEDGE_MS to "
            "WDT_STARVE_MS and stop there, and the granted deadline is a "
            "runtime read-back no "
            "compile-time check can see. Delete the check and a board whose dog "
            "does not act until long after this thread has given up on it says "
            "nothing at all; make the check disarm or refuse instead and a "
            "reporting defect becomes a safety change",
            WDT_STARVE_MS, TST_STARVE_S + 1, over, over_feeds);

    CHECK(shipping == 0 && shipping_errs == 0 && equal == 0,
            "...and it does NOT fire on the grant both boards actually get (a %d "
            "s deadline under a %ums budget — the whole prologue is SILENT there, "
            "saw %d ERR(s) of any kind) or at the boundary itself (%d s), where "
            "the deadline is still honoured: the gate stops feeding and the dog "
            "acts within the budget (saw %d and %d starve report(s)). Write the "
            "comparison as >= and every shipping board logs this at every boot, "
            "which is how a real report gets ignored",
            WDT_TIMEOUT_S, WDT_STARVE_MS, shipping_errs, TST_STARVE_S, shipping, equal);

    CHECK(absent == 0,
            "...and a deadline of 0 is silence, not an alarm: that is a board "
            "with no dog at all, or a grant the driver never reported, and the "
            "HAL has already logged both. Drop the `deadline_ms != 0` guard and "
            "every host build and every board whose WDIOC_GETTIMEOUT failed gains "
            "a watchdog ERR about a dog that is not there (saw %d report(s))",
            absent);
}

static void scenario_threaded(int passes, tst_health_fn h, int period_ms)
{
    tst_real_thread  = 1;
    tst_feed_period  = period_ms;
    tst_ns_n         = 0;
    tst_ns_runaway   = 0;
    tst_ns_stop_at   = 0;
    tst_ns_at_stop   = -1;
    tst_feed_at_stop = -1;
    tst_wf_tick      = -1;
    tst_wdt_joined   = 0;
    scenario(passes, h, 0);
    tst_prod_joined = tst_wdt_joined;
    if (tst_wdt_live)
        tst_wdt_reap();
    tst_real_thread = 0;
    tst_feed_period = WDT_FEED_PERIOD_MS_MAX;
}

static void tst_wdt_thread_live(void)
{
    int slow_want = TST_THREAD_TICKS(WDT_FEED_PERIOD_MS_MAX);
    int fast_want = TST_THREAD_TICKS(TST_THREAD_FAST_MS);
    int spt       = WDT_FEED_PERIOD_MS_MAX / (int)WDT_THREAD_SLICE_MS;
    int slow, fast, last;

    tst_feed_usr2_blocked = -1;
    scenario_threaded(TST_THREAD_PASS, h_core_alive, WDT_FEED_PERIOD_MS_MAX);
    slow = tst_wdt_feed_calls;
    last = slow > 0 && slow <= TST_NS_MAX ? slow - 1 : 0;

    CHECK(tst_feed_usr2_blocked == 1,
            "the watchdog thread keeps SIGUSR2 blocked like every other thread: "
            "mm_miner blocks it before any thread starts, so the pretimeout is "
            "never run as an asynchronous handler and stays pending until this "
            "thread takes it with sigtimedwait (saw blocked %d)",
            tst_feed_usr2_blocked);

    CHECK(tst_wdt_create_n == 1 && tst_wdt_join_n == 1 && tst_prod_joined &&
                    tst_ns_n == TST_THREAD_PASS && !tst_ns_runaway,
            "the watchdog thread runs for real here — mm's own pthread_create "
            "started it and mm's own pthread_join REAPED it, with the supervisor "
            "loop running beside it the whole time: %d create(s), %d join(s), "
            "reaped-by-mm %d, and %d slice(s) against %d supervisor passes. Delete "
            "the pthread_join and the thread outlives main(); the harness has to "
            "reap it instead and reports it here",
            tst_wdt_create_n, tst_wdt_join_n, tst_prod_joined, tst_ns_n, TST_THREAD_PASS);

    CHECK(slow == slow_want + 1 && tst_feed_ms[0] == 0 &&
                    tst_feed_ms[1] == (uint64_t)WDT_FEED_PERIOD_MS_MAX &&
                    tst_feed_ms[last] == (uint64_t)slow_want * WDT_FEED_PERIOD_MS_MAX &&
                    tst_gate_mask() == TST_MASK_ALL,
            "...and across %d s — %.1fx the %u ms starve budget — it fed once on "
            "entry and then ticked ONCE per %d ms period, feeding on every tick: "
            "%d feed(s), the first two at 0 ms and %d ms, the last at %u ms, gate "
            "mask 0x%08x (saw %d feed(s), first %u ms, second %u ms, last %u ms, "
            "mask 0x%08x). Three mutations land here: deleting the entry feed "
            "leaves the dog unfed for a whole period after an already unbounded "
            "start-up, a tick INSIDE the slice loop feeds ~%d times instead of %d, "
            "and deleting the supervisor's `atomic_store(&g_wdt_beat, 1)` "
            "leaves the SUPERVISOR bit unmarked on every tick, so the gate never "
            "completes and the feed stops %u ms in, at tick %d of %d",
            TST_THREAD_PASS, (double)TST_THREAD_PASS * TST_TICK_MS / WDT_STARVE_MS, WDT_STARVE_MS,
            WDT_FEED_PERIOD_MS_MAX, slow_want + 1, WDT_FEED_PERIOD_MS_MAX,
            (unsigned)((uint64_t)slow_want * WDT_FEED_PERIOD_MS_MAX), TST_MASK_ALL, slow,
            (unsigned)tst_feed_ms[0], (unsigned)tst_feed_ms[1], (unsigned)tst_feed_ms[last],
            tst_gate_mask(), TST_THREAD_PASS, slow_want, WDT_STARVE_MS,
            (int)(WDT_STARVE_MS / WDT_FEED_PERIOD_MS_MAX), slow_want);

    CHECK(tst_ns_at_stop == TST_THREAD_PASS && tst_ns_n == tst_ns_at_stop &&
                    tst_wdt_feed_calls == tst_feed_at_stop,
            "...and it notices the cleared g_run WITHIN the %u ms slice it is "
            "already in, %d ms into a %d ms period it never finishes: it enters NO "
            "further slice after the supervisor's last pass and does NOT feed on "
            "the way out (saw %d extra slice(s), %d extra feed(s)). Hoist the "
            "`g_run` re-test out of the slice loop and it sleeps the period's "
            "remaining %d slices first, so mm's stop path blocks on this join for "
            "up to %d ms instead of %u ms; drop the `break` after the loop and a "
            "stopping miner feeds the dog one last time",
            WDT_THREAD_SLICE_MS, (TST_THREAD_PASS - 1) % spt * (int)WDT_THREAD_SLICE_MS,
            WDT_FEED_PERIOD_MS_MAX, tst_ns_n - tst_ns_at_stop,
            tst_wdt_feed_calls - tst_feed_at_stop, spt - TST_THREAD_PASS % spt,
            WDT_FEED_PERIOD_MS_MAX, WDT_THREAD_SLICE_MS);

    scenario_threaded(TST_THREAD_PASS, h_core_alive, TST_THREAD_FAST_MS);
    fast = tst_wdt_feed_calls;

    CHECK(fast == fast_want + 1 && fast != slow && tst_feed_ms[0] == 0 &&
                    tst_feed_ms[1] == (uint64_t)TST_THREAD_FAST_MS && tst_ns_n == TST_THREAD_PASS,
            "...and the period is the HAL's, not a literal: the SAME %d passes over "
            "the SAME %u ms slices produce %d tick(s) when wdt_dev_feed_period_ms() "
            "negotiates %d ms and %d when it negotiates %d ms, the entry feed "
            "landing at 0 ms and the FIRST TICK at the period itself (saw %d "
            "feed(s), first at %u ms, second at %u ms). Write "
            "either period into mm_wdt_thread as a literal and one of the two runs "
            "is wrong; the real dog negotiates this from %d s / WDT_FEED_MARGIN %d "
            "and a board whose kernel clamps the timeout gets a different one",
            TST_THREAD_PASS, WDT_THREAD_SLICE_MS, fast_want, TST_THREAD_FAST_MS, slow_want,
            WDT_FEED_PERIOD_MS_MAX, fast, (unsigned)tst_feed_ms[0], (unsigned)tst_feed_ms[1],
            WDT_TIMEOUT_S, WDT_FEED_MARGIN);
}

static void tst_wdt_beat_handoff(void)
{
    int fed, last;

    tst_real_thread    = 1;
    tst_feed_period    = WDT_FEED_PERIOD_MS_MAX;
    tst_now            = 0;
    tst_ns_n           = 0;
    tst_ns_runaway     = 0;
    tst_ns_stop_at     = 0;
    tst_wdt_feed_calls = 0;
    tst_wf_tick        = -1;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    atomic_store(&g_core_spawn_at, 0);
    atomic_store(&g_asic_spawn_at, 0);
    g_fault_off = 0;
    tst_fresh = tst_asic_live = tst_core_live = 1;
    g_wdt_beat                                = 1;
    g_run                                     = 1;

    tst_wdt_spawn(mm_wdt_thread, NULL);
    for (int i = 0; i < TST_BEAT_STEPS; i++)
        tst_step_hand(TST_TICK_MS);
    g_run = 0;
    tst_step_hand(TST_TICK_MS);
    tst_wdt_reap();
    tst_real_thread = 0;

    fed  = tst_wdt_feed_calls;
    last = fed > 0 && fed <= TST_NS_MAX ? fed - 1 : 0;

    CHECK(fed == TST_BEAT_FED + 1 && tst_feed_ms[0] == 0 &&
                    tst_feed_ms[1] == (uint64_t)WDT_FEED_PERIOD_MS_MAX &&
                    tst_feed_ms[last] == WDT_STARVE_MS,
            "one beat is worth exactly one tick: a supervisor that sets g_wdt_beat "
            "ONCE and then goes silent completes the gate on the first tick and "
            "starves it from then on, so the dog is fed for %u ms and no longer — "
            "%d feed(s) (the entry feed at 0 ms plus one per tick), first tick at "
            "%d ms, last at %u ms (saw %d, %u, %u) out of %d "
            "ticks the thread ran. The tick CONSUMES the beat with "
            "atomic_exchange; make that a load and one stale beat feeds the dog "
            "for all %d ticks, which is a wedged supervisor holding the watchdog "
            "open forever",
            WDT_STARVE_MS, TST_BEAT_FED + 1, WDT_FEED_PERIOD_MS_MAX, (unsigned)WDT_STARVE_MS, fed,
            (unsigned)tst_feed_ms[1], (unsigned)tst_feed_ms[last],
            TST_BEAT_STEPS / (WDT_FEED_PERIOD_MS_MAX / (int)WDT_THREAD_SLICE_MS),
            TST_BEAT_STEPS / (WDT_FEED_PERIOD_MS_MAX / (int)WDT_THREAD_SLICE_MS));

    CHECK(tst_gate_mask() == (TST_MASK_ALL & ~(1u << WDT_TID_SUPERVISOR)),
            "...and the bit it is missing is the SUPERVISOR one and only that one: "
            "the last mask the thread published is 0x%08x, with ASIC and CORE still "
            "marked from a live telemetry snapshot (saw 0x%08x). That is the whole "
            "handoff in one value — the supervisor's store is the ONLY thing that "
            "can set bit %d, and the watchdog thread is the only thing that clears "
            "it",
            TST_MASK_ALL & ~(1u << WDT_TID_SUPERVISOR), tst_gate_mask(), WDT_TID_SUPERVISOR);
}

static void h_core_gone_at_grace_end(int p)
{
    tst_fresh     = 1;
    tst_asic_live = 1;
    g_wdt_beat    = 1;
    tst_core_live = p <= TST_CORE_GRACE_LAST;
}

static void h_asic_gone_at_grace_end(int p)
{
    tst_fresh     = 1;
    tst_core_live = 1;
    g_wdt_beat    = 1;
    tst_asic_live = p <= TST_ASIC_GRACE_LAST;
}

static void tst_grace_stale(void)
{
    char named[64];
    int  a;

    wdt_tick_reset();
    atomic_store(&g_core_spawn_at, 1);
    wdt_tick_run(TST_CORE_GRACE_LAST + 2, h_core_gone_at_grace_end);

    a = wf_first_set(tst_wf_named_core, 0, TST_CORE_GRACE_LAST + 1);
    CHECK(a < 0 && tst_wf_named_asic[0],
            "a core_miner that is ALIVE AND ANSWERING throughout its %u ms grace "
            "window is still not supervised there: `core` is named missing on none "
            "of ticks 0..%d (first named %d). The list is NOT empty in that window "
            "— it carries the other two participants, `asic` among them on tick 0 "
            "(%d) — so this is core's ABSENCE from the list and never an empty "
            "list",
            WDT_CORE_GRACE_MS, TST_CORE_GRACE_LAST, a, tst_wf_named_asic[0]);

    wdt_flags_missing(&tst_wf, named, sizeof named);
    CHECK(!strcmp(named, "core") && tst_gate_mask() == (TST_MASK_ALL & ~(1u << WDT_TID_CORE)),
            "...and on tick %d, the tick the window CLOSES, that same child going "
            "silent is caught immediately: the gate is INCOMPLETE and names "
            "exactly `core`, publishing 0x%08x (saw '%s', 0x%08x). Drop the "
            "`& f->enabled` from wdt_flags_mark() and the bit this child set on "
            "every tick of its window is still in the mask when the window opens: "
            "the gate completes here on a stale bit, publishes 0x%08x — all three "
            "reported — stamps healthy_at, and logs nothing missing, so the first "
            "supervised cycle is satisfied with no report in it. Every other grace "
            "scenario in this file KILLS the child, which is why this one has to "
            "exist: a dead child never sets the bit that carries over",
            TST_CORE_GRACE_LAST + 1, TST_MASK_ALL & ~(1u << WDT_TID_CORE), named, tst_gate_mask(),
            TST_MASK_ALL);

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 1);
    wdt_tick_run(TST_ASIC_GRACE_LAST + 2, h_asic_gone_at_grace_end);

    a = wf_first_set(tst_wf_named_asic, 0, TST_ASIC_GRACE_LAST + 1);
    CHECK(a < 0 && tst_wf_named_core[0],
            "the same shape on the ASIC seam and its longer %u ms clock: `asic` is "
            "named on none of ticks 0..%d (first named %d) while `core` is on the "
            "list throughout (tick 0: %d)",
            WDT_ASIC_GRACE_MS, TST_ASIC_GRACE_LAST, a, tst_wf_named_core[0]);

    wdt_flags_missing(&tst_wf, named, sizeof named);
    CHECK(!strcmp(named, "asic") && tst_gate_mask() == (TST_MASK_ALL & ~(1u << WDT_TID_ASIC)),
            "...and on tick %d the gate is incomplete and names exactly `asic`, "
            "publishing 0x%08x (saw '%s', 0x%08x). This arm survives the mark() "
            "revert where the core one does not, and the reason is mark ORDER "
            "alone: mm_wdt_tick marks SUPERVISOR, ASIC, then CORE, so a stale ASIC "
            "bit is wiped by CORE's completing mark inside the window while a "
            "stale CORE bit, set last, has nothing after it. Move the CORE mark "
            "above the ASIC one and the revert fails here too — which is the point "
            "of pinning both seams rather than the one that happens to bite",
            TST_ASIC_GRACE_LAST + 1, TST_MASK_ALL & ~(1u << WDT_TID_ASIC), named, tst_gate_mask());
}

static void tst_softonoff_scope(void)
{
    int u;

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 0);
    g_softonoff.api_softoff = 1;
    wdt_tick_run(TST_RUN, h_core_gone);
    g_softonoff.api_softoff = 0;
    u                       = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_WF_STARVE_TICK,
            "a softoff does NOT exempt the CORE bit: it stopped asic_miner only, "
            "core_miner and :4028 stay up on purpose, so a core_miner that goes "
            "silent through a softoff starves the dog on schedule (tick %d, first "
            "unfed %d). The softoff means \"do not check what was stopped\", not "
            "\"stop checking\"",
            TST_WF_STARVE_TICK, u);

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 0);
    g_softonoff.thermal_until = 1;
    wdt_tick_run(TST_RUN, h_core_gone);
    g_softonoff.thermal_until = 0;
    u                         = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_WF_STARVE_TICK,
            "a THERMAL softoff does NOT exempt the CORE bit either: the thermal soft-off stops "
            "asic_miner only, so a core_miner that goes silent through an "
            "overheat hold is a real wedge and starves the dog on the ordinary "
            "schedule (tick %d, first unfed %d). Give softonoff_children_stopped() "
            "a CORE bit and OR it into this leg and a core_miner that dies "
            "during a %u ms hold is fed for, and the :4028 server it is stays dead "
            "with nothing to notice",
            TST_WF_STARVE_TICK, u, OVERHOT_HOLD_MS);

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 0);
    g_softonoff.thermal_until = 1;
    wdt_tick_run(TST_RUN, h_wf_healthy);
    g_softonoff.thermal_until = 0;
    u                         = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "...and THAT is safe only because the CORE leg is satisfied by the REAL "
            "heartbeat through the hold: core_stats_push (app/core_stats.c) is "
            "called unconditionally from pump_circle at 1 Hz and sets core_health "
            "from CORE_MAIN_STALL_MS and CORE_API_STALL_MS alone — it reads no asic "
            "telemetry, no rail and no pool — so core_live stays true and the dog is "
            "fed for all %d ticks "
            "of the hold (first unfed %d). This is the whole safety case for "
            "dropping the exemption: were it ever false, every 3-minute thermal hold "
            "would reboot the board %u ms in",
            TST_RUN, u, WDT_STARVE_MS);

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 0);
    g_softonoff.thermal_until = 1;
    wdt_tick_run(TST_RUN, h_asic_gone);
    g_softonoff.thermal_until = 0;
    u                         = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "a THERMAL softoff exempts the ASIC bit, for all %d ticks: the thermal soft-off stops "
            "asic_miner, so the dead `Hash Board` heartbeat is the state mm asked "
            "for — and it is the ONLY bit a thermal hold exempts, the CORE bit "
            "having lost its exemption with the SIGTERM that earned it. This is "
            "the only scenario that runs a thermal hold against a dead heartbeat — "
            "drop SOFTONOFF_CHILD_ASIC from the `st->thermal_until` line of "
            "softonoff_children_stopped() and the board reboots ~%u ms into every "
            "overheat hold, which is a cooling miner being power-cycled for cooling "
            "(first unfed %d)",
            TST_RUN, WDT_STARVE_MS, u);

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 0);
    g_fault_off = 1;
    wdt_tick_run(TST_RUN, h_asic_gone);
    g_fault_off = 0;
    u           = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "a FAULT softoff (MC_SOFTOFF_HASH_FAULT / MC_SOFTOFF_POWER_FAULT) "
            "exempts the ASIC bit for all %d ticks: mm stopped asic_miner and is "
            "deliberately not respawning it, so the dead `Hash Board` heartbeat "
            "is the state mm asked for. Without this a board whose hashboards do "
            "not enumerate is reset — the first "
            "reset ~%u ms in (%u ms of bring-up window plus the %u ms starve "
            "budget) and NOT on that period thereafter: the budget is worth %u "
            "windows, so a child that keeps being respawned is supervised from "
            "its first tick once it is spent and the tolerance drops to the bare "
            "%u ms (first unfed %d)",
            TST_RUN, WDT_STARVE_MS + WDT_ASIC_GRACE_MS, WDT_ASIC_GRACE_MS, WDT_STARVE_MS,
            WDT_GRACE_TRIES_MAX, WDT_STARVE_MS, u);

    wdt_tick_reset();
    atomic_store(&g_asic_spawn_at, 0);
    g_fault_off = 1;
    wdt_tick_run(TST_RUN, h_core_gone);
    g_fault_off = 0;
    u           = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_WF_STARVE_TICK,
            "...and it does NOT exempt the CORE bit, on the same rule as the "
            "user's softoff: nothing stopped core_miner, so a silent one still "
            "starves the dog on schedule (tick %d, first unfed %d)",
            TST_WF_STARVE_TICK, u);
}

static void h_asic_crashloop(int p)
{
    tst_fresh     = 1;
    tst_core_live = 1;
    g_wdt_beat    = 1;
    tst_asic_live = p < 3;
    if (p % TST_RESPAWN_TICKS == 0)
        atomic_store(&g_asic_spawn_at, (uint64_t)(p + 1) * TST_TICK_MS);
}

static void h_asic_cold_start(int p)
{
    tst_fresh     = 1;
    tst_core_live = 1;
    g_wdt_beat    = 1;
    tst_asic_live = p >= TST_BUDGET_LAST - TST_RESPAWN_TICKS + 5;
    if (p == 0 || p == TST_RESPAWN_TICKS || p == 2 * TST_RESPAWN_TICKS)
        atomic_store(&g_asic_spawn_at, (uint64_t)(p + 1) * TST_TICK_MS);
}

static void h_core_wedge_cycles(int p)
{
    tst_fresh     = 1;
    tst_asic_live = 1;
    g_wdt_beat    = 1;
    tst_core_live = 0;
    if (p % TST_CORE_CYCLE_TICKS == 0)
        atomic_store(&g_core_spawn_at, (uint64_t)(p + 1) * TST_TICK_MS);
}

static void tst_grace_budget(void)
{
    int u, a;

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_asic_crashloop);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_BUDGET_LAST - 1 + TST_STARVE_PASS,
            "a child that keeps dying stops being exempt: asic_miner is ALIVE for "
            "the first %d ticks, then respawns every %d ticks without ever "
            "answering again. The grace window is a BRING-UP allowance worth %u "
            "respawns, not a renewable one, so the %u ms window is granted to "
            "spawns 1..%u and refused from spawn %u at tick %d; the gate is "
            "incomplete from there and the dog starves %u ms later, at tick %d "
            "(saw %d). Every respawn re-stamps g_asic_spawn_at, so a window "
            "computed from the stamp alone — delete the `tries` cap from "
            "wdt_grace_open() — is re-opened by the crash loop itself and NOTHING "
            "in this %d-tick run is ever unfed: a machine that hashes nothing, "
            "reboots never, and holds the dog open with a child it cannot keep "
            "alive",
            3, TST_RESPAWN_TICKS, WDT_GRACE_TRIES_MAX, WDT_ASIC_GRACE_MS, WDT_GRACE_TRIES_MAX,
            WDT_GRACE_TRIES_MAX + 1u, TST_BUDGET_LAST, WDT_STARVE_MS,
            TST_BUDGET_LAST - 1 + TST_STARVE_PASS, u, TST_RUN);

    a = wf_first_set(tst_wf_on_asic, 0, TST_BUDGET_LAST);
    CHECK(a < 0 && tst_wf_on_asic[TST_BUDGET_LAST],
            "...and the budget is spent on WINDOWS, not shortened into one: the "
            "ASIC bit is unsupervised across every tick of the first %u windows "
            "(ticks 0..%d, first supervised %d) and supervised from tick %d "
            "(enabled %d). A fix that simply narrowed WDT_ASIC_GRACE_MS would "
            "starve the dog on the FIRST slow enumeration instead",
            WDT_GRACE_TRIES_MAX, TST_BUDGET_LAST - 1, a, TST_BUDGET_LAST,
            tst_wf_on_asic[TST_BUDGET_LAST]);

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_asic_cold_start);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "and the case this must not break is protected: a cold start that "
            "needs all %u attempts — two failed bring-ups, then a third that "
            "enumerates and answers — is fed on every one of %d ticks (first "
            "unfed %d). The budget counts CONSECUTIVE windows and a report from "
            "the child while it is SUPERVISED clears it, so a board that is slow "
            "to bring its hashboards up costs windows and a board that cannot "
            "bring them up at all costs a reboot. Reset the budget on any report "
            "instead of a supervised one and the crash loop above does NOT go "
            "free — its child answers only inside its FIRST window, so the "
            "mutation buys one extra window and slides the first unfed tick from "
            "%d to %d rather than removing it; the first assertion in this "
            "function is the one that catches it, not this one",
            WDT_GRACE_TRIES_MAX, TST_RUN, u, TST_BUDGET_LAST - 1 + TST_STARVE_PASS,
            TST_BUDGET_LAST - 1 + TST_RESPAWN_TICKS + TST_STARVE_PASS);

    wdt_tick_reset();
    wdt_tick_run(TST_CORE_CYCLE_RUN, h_core_wedge_cycles);
    u = wf_first_unfed(0, TST_CORE_CYCLE_RUN);
    CHECK(u == TST_CORE_BUDGET_END + TST_STARVE_PASS,
            "and the CORE leg is bounded the same way, which is the answer to "
            "\"core_live is derived from message arrival, so a lost MSG_CORE_STATS "
            "resets a healthy board %u ms later\". It does not: core_live false is "
            "a WEDGE, and the supervisor recovers a wedge on its own cadence — "
            "SIGKILL at %u ms, respawn %u ms after that — and every respawn opens "
            "a %u ms window in which CORE is taken OUT of the enabled set and the "
            "gate completes without it. This run models exactly that: core_live is "
            "false from tick 0 and g_core_spawn_at is re-stamped every %d ticks, "
            "and the dog is fed through every tick of the %u-window budget, first "
            "unfed at tick %d (saw %d). So starving the dog on the CORE leg needs "
            "the silence to survive %u full core_miner restarts, not %u ms of "
            "missing datagrams — and the gap inside one cycle is the difference "
            "the _Static_assert at the head of this file pins under %u ms",
            WDT_STARVE_MS, WDT_CORE_WEDGE_MS, RESTART_MS, WDT_CORE_GRACE_MS, TST_CORE_CYCLE_TICKS,
            WDT_GRACE_TRIES_MAX, TST_CORE_BUDGET_END + TST_STARVE_PASS, u, WDT_GRACE_TRIES_MAX,
            WDT_STARVE_MS, WDT_STARVE_MS);

    a = wf_first_clear(tst_wf_on_core, TST_CORE_GRACE_LAST + 1, TST_CORE_CYCLE_TICKS);
    CHECK(a < 0 && !tst_wf_on_core[TST_CORE_CYCLE_TICKS] && !tst_wf_on_core[TST_CORE_LAST_SPAWN],
            "...and the feed BETWEEN two of those windows is carried by the "
            "healthy stamp, not by CORE being masked: the bit IS supervised on "
            "every tick of the gap, %d..%d (first unsupervised %d), and goes "
            "unsupervised again only where the next respawn re-stamps it, ticks "
            "%d and %d (enabled %d and %d). The gap is %d ticks against the %d-tick "
            "starve budget, and that margin is the whole reason a silent "
            "core_miner is recovered rather than rebooted — widen "
            "WDT_CORE_WEDGE_MS past WDT_CORE_GRACE_MS + WDT_STARVE_MS - "
            "RESTART_MS and the board resets inside the gap, on a core_miner the "
            "supervisor is in the middle of restarting",
            TST_CORE_GRACE_LAST + 1, TST_CORE_CYCLE_TICKS - 1, a, TST_CORE_CYCLE_TICKS,
            TST_CORE_LAST_SPAWN, tst_wf_on_core[TST_CORE_CYCLE_TICKS],
            tst_wf_on_core[TST_CORE_LAST_SPAWN], TST_CORE_CYCLE_TICKS - 1 - TST_CORE_GRACE_LAST,
            TST_STARVE_PASS);
}

static void h_asic_softoff_cycles(int p)
{
    tst_fresh     = 1;
    tst_core_live = 1;
    g_wdt_beat    = 1;
    tst_asic_live = 0;
    if (p == 0)
        atomic_store(&g_asic_spawn_at, (uint64_t)TST_TICK_MS);
    if (p < TST_SOFT_LAST_SPAWN && p % TST_SOFT_PERIOD == TST_SOFT_OFF_TICK)
        g_softonoff.api_softoff = 1;
    if (p > 0 && p <= TST_SOFT_LAST_SPAWN && p % TST_SOFT_PERIOD == 0) {
        g_softonoff.api_softoff = 0;
        atomic_store(&g_asic_spawn_at, (uint64_t)(p + 1) * TST_TICK_MS);
    }
}

static void tst_softoff_grace_budget(void)
{
    int u, a;

    wdt_tick_reset();
    wdt_tick_run(TST_SOFT_RUN, h_asic_softoff_cycles);
    g_softonoff.api_softoff = 0;
    u                       = wf_first_unfed(0, TST_SOFT_RUN);
    CHECK(u == TST_SOFT_GRACE_LAST + TST_STARVE_PASS,
            "a DELIBERATE stop does not spend a crash window: %d softoff/softon "
            "pairs %d ticks apart — every one of them faster than the %u ms ASIC "
            "window — kill and respawn asic_miner %d times without it ever having "
            "failed, and the window granted to the LAST respawn (tick %d) is still "
            "a whole one, so the first unfed tick is %d (saw %d). A softoff kills "
            "the child without moving g_asic_spawn_at and the softon re-stamps it, "
            "so a budget charged on the stamp alone — drop the "
            "softonoff_children_stopped() term from wdt_grace_open() — is "
            "exhausted by %u pairs, which the unauthenticated :4028 "
            "`softoff`/`softon` commands can issue at will, and equally by %u "
            "thermal softoffs tripping during enumeration: the first unfed tick "
            "moves to %d, %d ticks earlier, on a child that has crashed exactly "
            "zero times",
            TST_SOFT_CYCLES, TST_SOFT_PERIOD, WDT_ASIC_GRACE_MS, TST_SOFT_CYCLES,
            TST_SOFT_LAST_SPAWN, TST_SOFT_GRACE_LAST + TST_STARVE_PASS, u, WDT_GRACE_TRIES_MAX,
            WDT_GRACE_TRIES_MAX, TST_SOFT_SPENT_UNFED,
            TST_SOFT_GRACE_LAST + TST_STARVE_PASS - TST_SOFT_SPENT_UNFED);

    a = wf_first_set(tst_wf_on_asic, 0, TST_SOFT_GRACE_LAST + 1);
    CHECK(a < 0 && tst_wf_on_asic[TST_SOFT_GRACE_LAST + 1],
            "...and the window that survives is the LAST respawn's own, not the "
            "boot one carried over: the ASIC bit is unsupervised across every "
            "tick 0..%d (first supervised %d) and supervised from tick %d "
            "(enabled %d) — %u ms measured from the softon at tick %d, not from "
            "the spawn at tick 0. This is the only scenario in this file where a "
            "softoff OVERLAPS a live grace window; every other one forces the "
            "spawn stamp to 0, which disables the grace machinery outright and "
            "leaves this path compiled and unexercised",
            TST_SOFT_GRACE_LAST, a, TST_SOFT_GRACE_LAST + 1,
            tst_wf_on_asic[TST_SOFT_GRACE_LAST + 1], WDT_ASIC_GRACE_MS, TST_SOFT_LAST_SPAWN);
}

static void h_core_thermal_cycles(int p)
{
    tst_fresh     = 1;
    tst_asic_live = 1;
    g_wdt_beat    = 1;
    tst_core_live = 0;
    if (p == 0)
        atomic_store(&g_core_spawn_at, (uint64_t)TST_TICK_MS);
    if (p < TST_CORE_SOFT_SPENT && p % TST_CORE_SOFT_PERIOD == TST_CORE_SOFT_OFF_TICK)
        g_softonoff.thermal_until = 1;
    if (p > 0 && p <= TST_CORE_SOFT_SPENT && p % TST_CORE_SOFT_PERIOD == 0) {
        g_softonoff.thermal_until = 0;
        atomic_store(&g_core_spawn_at, (uint64_t)(p + 1) * TST_TICK_MS);
    }
}

static void tst_core_grace_has_no_deliberate(void)
{
    int u;

    wdt_tick_reset();
    wdt_tick_run(TST_CORE_SOFT_RUN, h_core_thermal_cycles);
    g_softonoff.thermal_until = 0;
    u                         = wf_first_unfed(0, TST_CORE_SOFT_RUN);
    CHECK(u == TST_CORE_SOFT_SPENT - 1 + TST_STARVE_PASS,
            "THE MIRROR IMAGE OF tst_softoff_grace_budget, AND IT IS DELIBERATE: "
            "core_miner has NO deliberate-stop input, so EVERY core respawn spends "
            "a crash window. %u thermal softoff/on pairs %d ticks apart respawn "
            "core_miner across live windows exactly as the ASIC case does, and "
            "here the budget IS exhausted -- refused at tick %d, first unfed %d "
            "(saw %d). This is correct rather than tolerated: "
            "NOTHING in mm deliberately stops core_miner (the thermal soft-off does not SIGTERM "
            "it, softonoff_intentional_exit is `is_asic &&` by construction), so "
            "the only kill of core left is the %u ms wedge SIGKILL -- a failure, "
            "which MUST spend a window. wdt_grace_open's `stopped` argument for "
            "CORE is a literal 0 because there is no true case to pass it. "
            "Add a CORE bit and this run is never unfed, which is the outage "
            "this guards against: a core_miner that dies inside a thermal hold holds the "
            "dog open with :4028 dead",
            WDT_GRACE_TRIES_MAX, TST_CORE_SOFT_PERIOD, TST_CORE_SOFT_SPENT,
            TST_CORE_SOFT_SPENT - 1 + TST_STARVE_PASS, u, WDT_CORE_WEDGE_MS);
}

static void h_a3_asic_missing(int p)
{
    (void)p;
    tst_fresh     = 1;
    tst_asic_live = 0;
    tst_core_live = 1;
    g_wdt_beat    = 1;
}

static void tst_gate_names_supervised(void)
{
    char named[64], want[64], log[128] = "";
    int  termsig = 0;

    wdt_tick_reset();
    atomic_store(&g_core_spawn_at, 1);
    wdt_tick_run(TST_CORE_GRACE_LAST + 1, h_a3_asic_missing);

    tst_gate_missing(named, sizeof named);
    CHECK(!strcmp(named, "asic") && tst_gate_enabled() == TST_A3_ENABLED,
            "the published gate carries the SUPERVISED SET, so the report blames "
            "only participants that were being watched: core_miner is inside its "
            "%u ms bring-up window and asic_miner is the one that owes a report, "
            "and a reader reconstructing the gate from the published value alone "
            "names exactly `asic` (saw '%s', supervised 0x%08x of 0x%08x). Publish "
            "the mask alone and rebuild with wdt_flags_reset() — which supervises "
            "everything — and this reads `asic,core`: the one diagnostic that says "
            "which participant killed the board, naming a child that was "
            "deliberately exempt",
            WDT_CORE_GRACE_MS, named, tst_gate_enabled(), TST_MASK_ALL);

    snprintf(want, sizeof want, "rR%02x:%08x;", (unsigned)MC_BOOTBY_WDT, (unsigned)TST_A3_GATE);
    CHECK(tst_gate == TST_A3_GATE && tst_sig_child(SIGUSR2, log, sizeof log, &termsig) == 0 &&
                    !strncmp(log, want, strlen(want)),
            "...and the pretimeout path hands that same value to "
            "mm_system_reboot as MC_BOOTBY_WDT's subreason (want %s — mask "
            "0x%08x, supervised 0x%08x), so the evidence that survives the reboot "
            "can still tell a missing participant from an exempt one (saw %s)",
            want, (uint32_t)(1u << WDT_TID_SUPERVISOR), TST_A3_ENABLED, log);
}

static void tst_fatal_rail_off(void)
{
    char log[128];
    int  termsig;

    scenario(TST_RUN, h_healthy, 0);

    CHECK(tst_sig_child(SIGSEGV, log, sizeof log, &termsig) == 0 && !strcmp(log, "r") &&
                    termsig == SIGSEGV,
            "mm_miner takes the rail down when it dies from a fatal signal: the "
            "handler mm_wdt_sig_install() registers calls board_power_rail_off "
            "exactly once, never disarms the dog (saw %s) and the process still "
            "dies from the signal it was handed (saw %d, want %d) because the "
            "handler re-raises under SIG_DFL. Without the rail cut the crash "
            "leaves an ENERGIZED board with the fan pinned at whatever duty the "
            "PID last wrote and NO thermal trip of any kind until the dog resets "
            "the SoC",
            log, termsig, SIGSEGV);

    tst_rail_fault = 1;
    CHECK(tst_sig_child(SIGSEGV, log, sizeof log, &termsig) == 0 && !strcmp(log, "r") &&
                    termsig == SIGBUS,
            "a fault INSIDE the rail cut re-enters the handler, which only dies: "
            "the rail-off runs once (saw %s) and the process goes down on the "
            "signal its own fault raised (saw %d, want %d)",
            log, termsig, SIGBUS);
    tst_rail_fault = 0;
}

static void tst_spawn_pdeathsig(void)
{
    int   ready[2], out[2];
    pid_t mid, child = -1, r = 0;
    int   st = 0;
    char  c;

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0 || pipe(ready) != 0 || pipe(out) != 0) {
        CHECK(0, "the PR_SET_PDEATHSIG harness could not be set up");
        return;
    }
    fflush(stdout);
    fflush(stderr);
    mid = fork();
    if (mid == 0) {
        close(out[0]);
        tst_fork_real = 1;
        tst_chdir_fd  = ready[1];
        child         = spawn_core();
        close(ready[1]);
        if (child <= 0 || read(ready[0], &c, 1) != 1)
            child = -1;
        if (write(out[1], &child, sizeof child) != sizeof child)
            _exit(1);
        _exit(0);
    }
    close(ready[0]);
    close(ready[1]);
    close(out[1]);
    if (mid < 0 || read(out[0], &child, sizeof child) != sizeof child)
        child = -1;
    close(out[0]);
    if (mid > 0)
        waitpid(mid, NULL, 0);
    for (int i = 0; child > 0 && i < 500 && r == 0; i++) {
        struct timespec ts = { 0, 10000000L };

        r = waitpid(child, &st, WNOHANG);
        if (r == 0)
            nanosleep(&ts, NULL);
    }
    if (child > 0 && r == 0) {
        kill(child, SIGKILL);
        waitpid(child, &st, 0);
    }
    prctl(PR_SET_CHILD_SUBREAPER, 0);

    CHECK(child > 0 && r == child && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
            "a child spawn_child() started is SIGKILLed when the process that "
            "spawned it exits: PR_SET_PDEATHSIG is armed before exec, so a "
            "core_miner or asic_miner never outlives a dead mm_miner (saw pid %d, "
            "reaped %d, signal %d)",
            (int)child, (int)r, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

int main(void)
{
    int                u, f;
    struct sigaction   pre_usr2, post_usr2, post_pipe;
    sigset_t           mask;
    pthread_condattr_t step_ca;

    pthread_condattr_init(&step_ca);
    pthread_condattr_setclock(&step_ca, CLOCK_MONOTONIC);
    pthread_cond_init(&tst_step_cv, &step_ca);
    pthread_condattr_destroy(&step_ca);

    printf("descriptor %s: %d board(s), WDT_STARVE_MS %u (= %d passes at %u ms), "
           "enum pin %d%%\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, WDT_STARVE_MS, TST_STARVE_PASS, TST_TICK_MS,
            FAN_DUTY_ENUM_PCT);

    sigaction(SIGUSR2, NULL, &pre_usr2);

    scenario(TST_RUN, h_healthy, 0);

    sigaction(SIGUSR2, NULL, &post_usr2);
    sigaction(SIGPIPE, NULL, &post_pipe);
    CHECK(pre_usr2.sa_handler == SIG_DFL && post_usr2.sa_handler == SIG_DFL &&
                    post_pipe.sa_handler == SIG_IGN,
            "main() installs the watchdog signal handlers before anything else "
            "runs: SIGPIPE goes to SIG_IGN and SIGUSR2 gets NO handler — it stays "
            "SIG_DFL, never SIG_IGN, which would discard the pretimeout instead "
            "of leaving it pending for the watchdog thread's sigtimedwait");
    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR2) == 1,
            "...and it leaves SIGUSR2 blocked on the main thread, so every thread "
            "inherits the block, SIG_DFL never terminates mm_miner, and the "
            "pretimeout stays pending until the watchdog thread takes it");

    CHECK(tst_wdt_create_n == 1 && tst_wdt_join_n == 1,
            "the watchdog thread is created exactly once at bring-up and joined "
            "exactly once on the way out (saw %d create(s), %d join(s)) — the "
            "supervisor does not leave it running past the shutdown tail",
            tst_wdt_create_n, tst_wdt_join_n);

    CHECK(!tst_runaway,
            "the supervisor loop exits on a cleared g_run — the SIGTERM path — "
            "after %d sleeps, well inside the %d cap; a loop that ignored it would "
            "never reach the join/rail-off tail",
            tst_sleeps, TST_SLEEP_CAP);

    {
        int bad = -1;
        for (int k = 0; k < TST_RUN; k++)
            if (tst_pass_ms[k] != (uint64_t)k * TST_TICK_MS) {
                bad = k;
                break;
            }
        CHECK(bad < 0,
                "the supervisor loop ticks at %u ms, so the %u ms budget is "
                "%d passes wide (first mismatch %d)",
                TST_TICK_MS, WDT_STARVE_MS, TST_STARVE_PASS, bad);
    }

    CHECK(tst_wdt_feed_calls == 0,
            "the supervisor loop calls wdt_dev_feed() zero times over %d passes — "
            "the feed gate is entirely the separate thread's (%d calls)",
            TST_RUN, tst_wdt_feed_calls);

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_wf_healthy);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "a healthy miner is fed on every one of %d ticks (%.1fx the "
            "budget) — the healthy stamp follows `now`, so the window never "
            "expires (first unfed tick %d)",
            TST_RUN, (double)TST_RUN / TST_STARVE_PASS, u);

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_dead);
    u = wf_first_unfed(0, TST_WF_STARVE_TICK);
    CHECK(u < 0,
            "a silent miner is still fed for the whole %u ms grace — ticks 0"
            "..%d cover bring-up (first unfed %d)",
            WDT_STARVE_MS, TST_WF_STARVE_TICK - 1, u);

    CHECK(!tst_wf_fed[TST_WF_STARVE_TICK],
            "the feed stops on the tick where the silence REACHES %u ms (tick %d, "
            "now %u ms), not one tick later: the gate is `<`, not `<=`",
            WDT_STARVE_MS, TST_WF_STARVE_TICK,
            (unsigned)((uint64_t)(TST_WF_STARVE_TICK + 1) * TST_TICK_MS));

    f = wf_first_fed(TST_WF_STARVE_TICK, TST_RUN);
    CHECK(f < 0,
            "...and it stays stopped for the remaining ticks, so the "
            "hardware dog reaches its own %d s timeout and resets the SoC (tick %d "
            "fed again)",
            WDT_TIMEOUT_S, f);

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_slide);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_SLIDE_BREAK + TST_STARVE_PASS,
            "ONE complete tick at %d s re-opens the FULL %u ms: the feed runs "
            "unbroken to tick %d and the first unfed tick is %d, counted from the "
            "healthy tick — a from-spawn deadline would have stopped it at tick "
            "%d, %d ticks earlier. The window is CONSECUTIVE SILENCE, not a "
            "countdown to bring-up (first unfed %d)",
            TST_SLIDE_BREAK, WDT_STARVE_MS, TST_SLIDE_BREAK + TST_STARVE_PASS - 1,
            TST_SLIDE_BREAK + TST_STARVE_PASS, TST_STARVE_PASS - 1, TST_STARVE_PASS, u);

    f = wf_first_fed(TST_SLIDE_BREAK + TST_STARVE_PASS, TST_RUN);
    CHECK(f < 0,
            "...and the second drought is not refunded again: nothing after "
            "tick %d is fed (tick %d fed)",
            TST_SLIDE_BREAK + TST_STARVE_PASS, f);

    wdt_tick_reset();
    wdt_tick_run(TST_RUN, h_asic_gone);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u == TST_WF_STARVE_TICK,
            "a fresh estats reply and a live core_miner with the `Hash Board` "
            "heartbeat DEAD starves the dog on schedule (tick %d): the ASIC bit "
            "needs asic_live or idle, and neither holds here (first unfed %d)",
            TST_WF_STARVE_TICK, u);

    wdt_tick_reset();
    g_softonoff.api_softoff = 1;
    wdt_tick_run(TST_RUN, h_parked);
    u = wf_first_unfed(0, TST_RUN);
    CHECK(u < 0,
            "the same dead `Hash Board` heartbeat under a softoff is fed for all "
            "%d ticks — the softoff stopped asic_miner, so the ASIC bit is exempt, "
            "while core_miner is still running and still answering, so the CORE "
            "bit is marked the ordinary way. The dog does not reset a board the "
            "user deliberately softed off (first unfed %d)",
            TST_RUN, u);

    scenario(TST_RUN, h_healthy, 0);
    {
        static const char *const up[] = { "rail-on", "wdt-init", "spawn", "spawn" };
        CHECK(ev_is(0, up, 4),
                "bring-up order: the rail is energized and the dog ARMED before "
                "either child is spawned, so a hang during bring-up is already "
                "covered (saw %s)",
                ev_str());

        static const char *const down[] = { "join", "rail-off", "disarm" };
        CHECK(tst_ev_n >= 4 && ev_is(tst_ev_n - 4, down, 3),
                "stop order: the children are JOINED before the rail-off, "
                "so the rail cannot be cut while asic_miner still drives the ASIC "
                "UART, and the dog is released only after (saw %s)",
                ev_str());

        static const char *const wdown[] = { "join", "rail-off", "disarm", "wdt-join" };
        CHECK(ev_is_tail(wdown, 4),
                "...and the WATCHDOG THREAD is joined last of all, after the "
                "children, the rail and wdt_dev_disarm() — so a pretimeout during "
                "the shutdown tail still has a thread waiting for it, and mm cannot "
                "exit with that thread still running. The `join` in this tail is "
                "mm_children_stop's, a different one (saw %s)",
                ev_str());
    }

    tst_spawn_grace();
    tst_vf_stall();
    tst_grace_stale();
    tst_core_wedge();
    tst_pretimeout();
    tst_pretimeout_on_stop();
    tst_wdt_thread_loop();
    tst_wdt_starve_vs_deadline();
    tst_wdt_thread_live();
    tst_wdt_beat_handoff();
    tst_core_live = 1;

    tst_reap_alt = 1;
    scenario(40, h_healthy, 0);
    tst_reap_alt = 0;
    {
        int bad = -1;
        for (int k = 0; k + 1 < 40; k++)
            if (tst_pass_ms[k + 1] - tst_pass_ms[k] != TST_TICK_MS) {
                bad = k;
                break;
            }
        CHECK(bad < 0,
                "a reaped child does not cost a supervised pass: the %u ms "
                "respawn delay is a deadline for mm_spawn_retry, not a sleep, so "
                "consecutive passes stay %llu ms apart and mm_telem_poll and the "
                "ctl_tick rule set keep running (first mismatch %d)",
                RESTART_MS, (unsigned long long)TST_TICK_MS, bad);

        CHECK(tst_spawn_core_n == 1 && tst_spawn_asic_n > 1,
                "the crash loop restarts asic_miner, not core_miner: core_miner "
                "was spawned once (bring-up only, %d spawn(s)), asic_miner was "
                "spawned %d times (bring-up + every crash respawn) — a waitpid "
                "stub that reaps by call order instead of pid would restart "
                "core_miner instead",
                tst_spawn_core_n, tst_spawn_asic_n);
    }

    tst_softonoff_scope();
    tst_grace_budget();
    tst_softoff_grace_budget();
    tst_core_grace_has_no_deliberate();
    tst_gate_names_supervised();
    tst_fatal_rail_off();
    tst_spawn_pdeathsig();

    return tst_done();
}
