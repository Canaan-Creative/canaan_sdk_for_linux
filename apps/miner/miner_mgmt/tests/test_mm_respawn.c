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
#include "tst_fan_dev.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* no first asic_miner start delay, s */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_MAX_PASS 64 /* pass bound that sizes the sleep cap */
/* osif_sleep_ms() calls before a scenario counts as runaway */
#define TST_SLEEP_CAP (TST_MAX_PASS * 8)
#define TST_TICK_MS   1000u /* main-loop pass length, ms */

#define TST_EV_NO_QUERY /* this file defines its own ev_str()/ev_count()/ev_nth() */

#define TST_UP_PASS        2 /* pass on which the network comes up */
#define TST_BACKOFF_PASSES ((int)(RESTART_MS / TST_TICK_MS)) /* passes in one restart delay */
/* The first pass after the failed first asic_miner fork. */
#define TST_OFF_PASS 1

pid_t                        tst_fork(void);
int                          tst_system(const char *cmd);
int                          tst_kill(pid_t pid, int sig);
pid_t                        tst_waitpid(pid_t pid, int *status, int options);
int                          tst_stat(const char *path, struct stat *st);
static void                  tst_log(osif_loglvl_t lvl, const char *fmt, ...);
static thermal_telem_verdict tst_telem_check(thermal_telem_state_t *st, int asic_live,
        int temp_valid, uint64_t now_ms, uint32_t grace_ms);

#define fork()           tst_fork()                 /* scripted fork, no child */
#define system(c)        tst_system(c)              /* no-op, returns 0 */
#define kill(p, s)       tst_kill((p), (s))         /* records a kill event */
#define waitpid(p, s, o) tst_waitpid((p), (s), (o)) /* scripted reap */
#define stat(p, b)       tst_stat((p), (b))         /* every path missing */
#define osif_log(...)    tst_log(__VA_ARGS__)       /* discards the line */
/* thermal_telem_check() with a per-scenario state reset */
#define thermal_telem_check(a, b, c, d, e) tst_telem_check((a), (b), (c), (d), (e))

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
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork

/* Per-pass scenario step, called with the 0-based pass number */
typedef void (*tst_script_fn)(int pass);

static uint64_t tst_now; /* fake monotonic clock, ms */
/* passes run, passes wanted, osif_sleep_ms() calls, 1 = sleep cap exceeded */
static int           tst_pass, tst_want, tst_sleeps, tst_runaway;
static int           tst_net_up; /* 1 = network up with an IP */
static tst_script_fn tst_script;
/* mm_telem_get() values: poll_fresh, asic_live, core_live, in_work flags; tmax, degC */
static int   tst_fresh, tst_asic_live, tst_core_live, tst_in_work, tst_tmax;
static pid_t tst_stop_pids[2];           /* pids passed to children_stop(): [0] core, [1] asic */
static pid_t tst_next_pid = 4000;        /* pid the next successful fork returns */
static pid_t tst_pid_core, tst_pid_asic; /* last forked core_miner/asic_miner pid */
static int   tst_core_n, tst_asic_n;     /* core_miner/asic_miner fork attempts */
/* Failing fork plan: first failing attempt and failure count, core then asic */
static int      tst_fc_from, tst_fc_n, tst_fa_from, tst_fa_n;
static int      tst_reap_asic, tst_reap_core; /* 1 = next waitpid() reaps that child */
static int      tst_telem_reset; /* 1 = clear the thermal telem state on the next check */
static uint64_t tst_fork_ms;     /* clock advance inside each fork, ms */
static unsigned tst_ingest_conn; /* last mm_telem_ingest() conn */

#include "tst_events.h"

static const char *ev_str(int from, int to)
{
    static char b[640];
    b[0] = '\0';
    for (int i = from; i < to && i < tst_ev_n; i++) {
        char one[56];
        snprintf(one, sizeof one, "%s%s=%d@%llu", i > from ? "," : "", tst_ev[i].what,
                tst_ev[i].val, (unsigned long long)tst_ev[i].now);
        strncat(b, one, sizeof b - strlen(b) - 1);
    }
    return b[0] ? b : "(nothing)";
}
static int ev_count(const char *what)
{
    int n = 0;
    for (int i = 0; i < tst_ev_n && i < TST_EV_MAX; i++)
        if (!strcmp(tst_ev[i].what, what))
            n++;
    return n;
}
static int ev_nth(const char *what, int n)
{
    for (int i = 0; i < tst_ev_n && i < TST_EV_MAX; i++)
        if (!strcmp(tst_ev[i].what, what) && --n == 0)
            return i;
    return -1;
}
static int64_t ev_gap(const char *what, int n)
{
    int a = ev_nth(what, n), b = ev_nth(what, n + 1);
    if (a < 0 || b < 0)
        return -1;
    return (int64_t)(tst_ev[b].now - tst_ev[a].now);
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
        fprintf(stderr, "  FAIL: loop does not exit on cleared g_run (%d sleeps)\n", tst_sleeps);
        _exit(1);
    }
}

void mm_telem_ingest(const void *d, int l, unsigned c)
{
    (void)d;
    (void)l;
    tst_ingest_conn = c;
}
void mm_telem_ingest_core(const void *d, int l)
{
    (void)d;
    (void)l;
}
void mm_telem_poll(void)
{
    tst_script(tst_pass);
    if (++tst_pass >= tst_want)
        g_run = 0;
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
    (void)term_ms;
    (void)kill_ms;
    for (int i = 0; i < n && i < 2; i++)
        tst_stop_pids[i] = pids[i];
    ev("join", 0);
    return 0;
}

static void fail_plan(int cf, int cn, int af, int an)
{
    tst_fc_from = cf;
    tst_fc_n    = cn;
    tst_fa_from = af;
    tst_fa_n    = an;
}
pid_t tst_fork(void)
{
    int   is_asic = (g_enum_hold.phase == ENUM_HOLD_PHASE_ARMED);
    int   ord     = is_asic ? ++tst_asic_n : ++tst_core_n;
    int   from    = is_asic ? tst_fa_from : tst_fc_from;
    int   cnt     = is_asic ? tst_fa_n : tst_fc_n;
    pid_t p;

    tst_now += tst_fork_ms;
    if (cnt > 0 && ord >= from && ord < from + cnt)
        p = -1;
    else {
        p = tst_next_pid++;
        if (is_asic)
            tst_pid_asic = p;
        else
            tst_pid_core = p;
    }
    ev(is_asic ? "spawn-asic" : "spawn-core", (int)p);
    return p;
}
int board_power_rail_on(uint16_t counts)
{
    (void)counts;
    ev("rail-on", 0);
    return HAL_PSU_RAIL_OK;
}
int board_power_rail_off(void)
{
    ev("rail-off", 0);
    return HAL_PSU_RAIL_OK;
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

static void tst_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    va_list ap;
    va_start(ap, fmt);
    va_end(ap);
    (void)fmt;
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
    tst_pid_core = tst_pid_asic = 0;
    tst_duty_x10                = 0;
    tst_reap_asic = tst_reap_core = 0;
    tst_telem_reset               = 1;
    tst_stop_pids[0] = tst_stop_pids[1] = 0;
    tst_core_n = tst_asic_n = 0;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_api_req           = 0;
    g_fault_off         = 0;
    g_asic_ever_live    = 0;
    g_core_retry_at     = 0;
    g_asic_retry_at     = 0;
    atomic_store(&g_core_spawn_at, 0);
    atomic_store(&g_asic_spawn_at, 0);
    tst_net_up    = 0;
    tst_fresh     = 1;
    tst_asic_live = 1;
    tst_core_live = 1;
    tst_in_work   = 1;
    tst_tmax      = 60;
    g_run         = 1;
    mm_miner_main_unused();
}

static void tst_net_raise(void)
{
    tst_net_up = 1;
}

static void s_quiet(int p)
{
    (void)p;
}
static void s_up(int p)
{
    if (p == TST_UP_PASS)
        tst_net_raise();
}
static void s_reap_core(int p)
{
    if (p == TST_UP_PASS)
        tst_reap_core = 1;
}
static void s_reap_asic(int p)
{
    if (p == TST_UP_PASS)
        tst_net_raise();
    if (p == TST_UP_PASS + 2)
        tst_reap_asic = 1;
}
static void s_reap_asic_term(int p)
{
    if (p == TST_UP_PASS)
        tst_net_raise();
    if (p == TST_UP_PASS + 2)
        tst_reap_asic = 1;
    if (p == TST_UP_PASS + 3)
        g_run = 0;
}
static void s_reap_core_term(int p)
{
    if (p == TST_UP_PASS)
        tst_reap_core = 1;
    if (p == TST_UP_PASS + 1)
        g_run = 0;
}
static void s_up_then_off(int p)
{
    if (p == TST_UP_PASS)
        tst_net_raise();
    if (p == TST_OFF_PASS)
        g_api_req = -1;
}

int main(void)
{
    printf("descriptor %s: %d board(s), RESTART_MS %u\n", BOARDCONF_SECTION, (int)HBOARD_COUNT,
            RESTART_MS);

    _Static_assert(RESTART_MS % TST_TICK_MS == 0,
            "the retry backoff must be a whole number of supervised "
            "ticks or the gap assertions below cannot be exact");

    fail_plan(1, 1, 0, 0);
    scenario(TST_BACKOFF_PASSES + 4, s_quiet);
    CHECK(!tst_runaway, "the loop exits on a cleared g_run (%d sleeps)", tst_sleeps);
    CHECK(ev_count("spawn-core") == 2,
            "a failed BOOT fork of %s is retried — exactly one retry, and it is the "
            "last (saw %d spawn-core: %s)",
            CORE_BIN, ev_count("spawn-core"), ev_str(0, tst_ev_n));
    {
        int a = ev_nth("spawn-core", 1), b = ev_nth("spawn-core", 2);
        CHECK(a >= 0 && tst_ev[a].val == -1 && b >= 0 && tst_ev[b].val > 0,
                "...the first fork really did fail (%d) and the retry really did "
                "succeed (%d) — neither assertion is vacuous",
                a >= 0 ? tst_ev[a].val : 0, b >= 0 ? tst_ev[b].val : 0);
        CHECK(ev_gap("spawn-core", 1) == (int64_t)RESTART_MS,
                "...on the reap path's own RESTART_MS pacing, not the next tick "
                "(gap %lld ms, want %u)",
                (long long)ev_gap("spawn-core", 1), RESTART_MS);
    }

    fail_plan(0, 0, 0, 0);
    scenario(2 * TST_BACKOFF_PASSES + 8, s_reap_core);
    CHECK(ev_count("reap-core") == 1 && ev_count("spawn-core") == 2,
            "instrument check: with no fork failing, a reaped %s is respawned once "
            "by the reap branch and never again (reaps %d, spawns %d)",
            CORE_BIN, ev_count("reap-core"), ev_count("spawn-core"));

    fail_plan(2, 1, 0, 0);
    scenario(2 * TST_BACKOFF_PASSES + 8, s_reap_core);
    CHECK(ev_count("spawn-core") == 3,
            "a failed REAP-BRANCH respawn of %s is retried (saw %d spawn-core: %s)", CORE_BIN,
            ev_count("spawn-core"), ev_str(0, tst_ev_n));
    {
        int b = ev_nth("spawn-core", 2), c = ev_nth("spawn-core", 3);
        CHECK(b >= 0 && tst_ev[b].val == -1 && c >= 0 && tst_ev[c].val > 0,
                "...the respawn really did fail (%d) and the retry really did "
                "succeed (%d)",
                b >= 0 ? tst_ev[b].val : 0, c >= 0 ? tst_ev[c].val : 0);
        CHECK(ev_gap("spawn-core", 2) == (int64_t)RESTART_MS,
                "...one further RESTART_MS after the failure, on top of the backoff "
                "the reap branch had already waited (gap %lld ms, want %u)",
                (long long)ev_gap("spawn-core", 2), RESTART_MS);
    }

    fail_plan(1, 4, 0, 0);
    scenario(4 * TST_BACKOFF_PASSES + 6, s_quiet);
    CHECK(ev_count("spawn-core") == 5,
            "four failed forks in a row produce four retries and then stop (saw %d)",
            ev_count("spawn-core"));
    {
        int paced = 1;
        for (int n = 1; n <= 4; n++)
            if (ev_gap("spawn-core", n) != (int64_t)RESTART_MS)
                paced = 0;
        CHECK(paced,
                "...each exactly RESTART_MS (%u ms) apart — the retry is "
                "PACED, not run every tick (%s)",
                RESTART_MS, ev_str(0, tst_ev_n));
    }
    {
        int last = ev_nth("spawn-core", 5);
        CHECK(last >= 0 && tst_ev[last].val > 0,
                "...and the run ends on a live child, not another -1 (pid %d)",
                last >= 0 ? tst_ev[last].val : 0);
    }

    fail_plan(0, 0, 1, 1);
    scenario(TST_UP_PASS + TST_BACKOFF_PASSES + 4, s_up);
    CHECK(ev_count("spawn-asic") == 2 && ev_gap("spawn-asic", 1) == (int64_t)RESTART_MS,
            "a failed first fork of %s is retried on the same RESTART_MS "
            "pacing (spawns %d, gap %lld ms: %s)",
            ASIC_BIN, ev_count("spawn-asic"), (long long)ev_gap("spawn-asic", 1),
            ev_str(0, tst_ev_n));
    CHECK(g_softonoff.start_wait == 0,
            "...and the start_wait latch stays clear across the retry — the retry "
            "is the recovery path, not a gate release (latch %d)",
            g_softonoff.start_wait);

    fail_plan(0, 0, 1, 1);
    scenario(TST_UP_PASS + TST_BACKOFF_PASSES + 6, s_up_then_off);
    CHECK(ev_count("spawn-asic") == 1,
            "an API soft-off between a failed %s fork and its retry DROPS the retry "
            "— a deliberately stopped machine is never resurrected by it (spawns %d: "
            "%s)",
            ASIC_BIN, ev_count("spawn-asic"), ev_str(0, tst_ev_n));
    CHECK(g_softonoff.api_softoff == 1,
            "...and the soft-off really is latched, so the assertion above is not "
            "passing because nothing happened (api_softoff %d)",
            g_softonoff.api_softoff);

    fail_plan(0, 0, 0, 0);
    scenario(TST_UP_PASS + TST_BACKOFF_PASSES + 6, s_reap_asic_term);
    CHECK(ev_count("reap-asic") == 1 && ev_count("join") == 1 && ev_count("spawn-asic") == 1 &&
                    tst_stop_pids[1] <= 0 && tst_stop_pids[0] == tst_pid_core,
            "a SIGTERM during the respawn backoff of a reaped %s does not hand "
            "its stale pid to the shutdown join (asic %d, core %d, live core %d)",
            ASIC_BIN, (int)tst_stop_pids[1], (int)tst_stop_pids[0], (int)tst_pid_core);

    scenario(TST_UP_PASS + TST_BACKOFF_PASSES + 6, s_reap_core_term);
    CHECK(ev_count("reap-core") == 1 && ev_count("join") == 1 && ev_count("spawn-core") == 1 &&
                    tst_stop_pids[0] <= 0,
            "...nor does a reaped %s (core %d)", CORE_BIN, (int)tst_stop_pids[0]);

    extern int stub_vf_asic_spawned_n, stub_telem_asic_reset_n;
    stub_vf_asic_spawned_n = stub_telem_asic_reset_n = 0;
    scenario(TST_UP_PASS + TST_BACKOFF_PASSES + 6, s_reap_asic);
    CHECK(ev_count("reap-asic") == 1 && ev_count("spawn-asic") == 2 &&
                    stub_telem_asic_reset_n == 2 && stub_vf_asic_spawned_n == 2,
            "every %s spawn, the crash respawn included, drops the previous "
            "instance's telemetry and re-seeds the V/F policy (%d spawn(s), "
            "%d telemetry reset(s), %d V/F re-seed(s))",
            ASIC_BIN, ev_count("spawn-asic"), stub_telem_asic_reset_n, stub_vf_asic_spawned_n);

    scenario(TST_UP_PASS + 2 * TST_BACKOFF_PASSES + 6, s_up);
    CHECK(ev_count("spawn-core") == 1 && ev_count("spawn-asic") == 1,
            "with every fork succeeding the retry never fires: one %s and one %s "
            "spawn for the whole run (core %d, asic %d)",
            CORE_BIN, ASIC_BIN, ev_count("spawn-core"), ev_count("spawn-asic"));
    CHECK(g_core_retry_at == 0 && g_asic_retry_at == 0,
            "...and no retry deadline is left armed behind it (core %llu, asic %llu)",
            (unsigned long long)g_core_retry_at, (unsigned long long)g_asic_retry_at);

    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    tst_ev_n    = 0;
    tst_now     = 5000;
    tst_fork_ms = 1;
    {
        pid_t    pc      = spawn_core();
        uint64_t live_at = g_core_live_at;

        tst_fork_ms = 0;
        mm_core_wedge_check(5000, 0, pc);
        CHECK(pc > 0 && live_at == 5001 && ev_count("kill-core") == 0,
                "a %s spawned after the tick's clock read, so that its liveness stamp "
                "is later than that tick's now, is not taken as wedged (stamp %llu, "
                "now 5000: %s)",
                CORE_BIN, (unsigned long long)live_at, ev_str(0, tst_ev_n));
        mm_core_wedge_check(live_at + WDT_CORE_WEDGE_MS - 1u, 0, pc);
        CHECK(ev_count("kill-core") == 0, "...nor one tick short of WDT_CORE_WEDGE_MS (%s)",
                ev_str(0, tst_ev_n));
        mm_core_wedge_check(live_at + WDT_CORE_WEDGE_MS, 0, pc);
        {
            int k = ev_nth("kill-core", 1);
            CHECK(ev_count("kill-core") == 1 && k >= 0 && tst_ev[k].val == SIGKILL,
                    "...while a %s silent for WDT_CORE_WEDGE_MS (%u ms) is still "
                    "SIGKILLed, exactly once (%s)",
                    CORE_BIN, WDT_CORE_WEDGE_MS, ev_str(0, tst_ev_n));
        }
    }

    extern unsigned stub_ipc_conn_seq, stub_telem_asic_reset_conn;
    stub_ipc_conn_seq          = 41;
    stub_telem_asic_reset_conn = 0;
    tst_ingest_conn            = 0;
    {
        uint8_t msg[8] = { 0 };

        (void)spawn_asic();
        CHECK(stub_telem_asic_reset_conn == osif_ipc_conn_seq(g_ipc_asic_mm) + 1u,
                "spawn_asic() drops telemetry from connections up to the current one: "
                "it passes osif_ipc_conn_seq() + 1 (passed %u, seq %u)",
                stub_telem_asic_reset_conn, osif_ipc_conn_seq(g_ipc_asic_mm));
        mm_asic_cb(MSG_ASIC_TELEM, msg, (int)sizeof msg);
        CHECK(tst_ingest_conn == osif_ipc_conn_seq(g_ipc_asic_mm),
                "an asic telemetry message is ingested under the current connection "
                "sequence (got %u, seq %u)",
                tst_ingest_conn, osif_ipc_conn_seq(g_ipc_asic_mm));
    }
    stub_ipc_conn_seq = 0;

    return tst_done();
}
