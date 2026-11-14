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
#include "framework.h"
#include "fan_dev.h"
#include "mm_telem.h"
#include "osif.h"
#include "wdt_dev.h"
#include "fan_pid.h"
#include "thermal.h"
#include "softonoff.h"
#include "net_linux_paths.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* asic_miner spawn delay, s: none */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "  FAIL: "); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            fails++; \
        } else { \
            printf("  ok: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)
#define TST_MAX_PASS      260                /* supervisor passes a scenario may run */
#define TST_SLEEP_CAP     (TST_MAX_PASS * 4) /* osif_sleep_ms() calls before g_run is forced to 0 */
#define TST_TICK_MS       1000u              /* fake clock per ctl_tick() event, ms */
#define TST_PIN           (FAN_DUTY_ENUM_PCT > 0) /* 1 when the descriptor has an enum fan pin */
#define TST_SPAWN_PASS    0                       /* pass that spawns asic_miner */
#define TST_LIVE_PASS     (TST_SPAWN_PASS + 2)    /* first pass with asic telemetry live */
#define TST_CRASH_PASS    2                       /* pass on which asic_miner exits */
#define TST_TRIP_PASS     4                       /* pass that requests the overheat softoff */
#define TST_HOLD_PASS     ((int)(OVERHOT_HOLD_MS / TST_TICK_MS)) /* passes in the overheat hold */
#define TST_RESUME_PASS   (TST_TRIP_PASS + TST_HOLD_PASS)        /* pass the overheat hold ends */
#define TST_ON_PASS       3 /* pass that requests the API soft-on */
#define TST_SOFTOFF2_PASS 5 /* pass that requests the second API soft-off */
#define TST_CLOB_PASS     6 /* pass on which asic telemetry turns live */

#define TST_EV_MAX  2048    /* capacity of the event log, events */
#define TST_EV_PASS tst_cur /* pass counter recorded with each event */

#define TST_FAN_DUTY_EV /* log each fan duty set as a "duty" event */

/* Calls in the included sources, routed to the tst_* fakes below */
#define fork()                             tst_fork()
#define system(c)                          tst_system(c)
#define kill(p, s)                         tst_kill((p), (s))
#define waitpid(p, s, o)                   tst_waitpid((p), (s), (o))
#define stat(p, b)                         tst_stat((p), (b))
#define fan_pid_set_fixed(s, d)            tst_fan_fixed((s), (d))
#define ctl_tick(n)                        tst_ctl_tick(n)
#define mm_telem_get(o)                    tst_telem_get(o)
#define thermal_telem_check(a, b, c, d, e) tst_telem_check((a), (b), (c), (d), (e))

pid_t                        tst_fork(void);
int                          tst_system(const char *cmd);
int                          tst_kill(pid_t pid, int sig);
pid_t                        tst_waitpid(pid_t pid, int *status, int options);
int                          tst_stat(const char *path, struct stat *st);
static void                  tst_fan_fixed(fan_pid_state_t *st, int duty_pct);
static int                   tst_ctl_tick(uint64_t now_ms);
static void                  tst_telem_get(mm_telem_t *out);
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
#undef mm_telem_get
#undef ctl_tick
#undef fan_pid_set_fixed
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork

_Static_assert(FAN_DUTY_ENUM_PCT == 0 || FAN_DUTY_ENUM_PCT != FAN_DUTY_API_SOFTOFF_PCT,
        "the enum pin and the API soft-off pin are the same duty on this "
        "descriptor — the clobber assertion would be vacuous");

typedef void (*tst_script_fn)(int pass); /* per-pass scenario hook, pass from 0 */

static int      fails;
static int      tst_net_up; /* nonzero reports the network up with an IP */
static uint64_t tst_now;    /* fake monotonic clock, ms */
/* Passes run and wanted; osif_sleep_ms() calls; TST_SLEEP_CAP overrun flag */
static int           tst_pass, tst_want, tst_sleeps, tst_runaway;
static int           tst_cur = -1; /* pass recorded with each event; -1 before the first */
static int           tst_pass_ev0[TST_MAX_PASS + 2]; /* first event index of each pass */
static tst_script_fn tst_script;
/* mm_telem_get() poll_fresh, asic_live and in_work flags; tmax and tavg, degC */
static int tst_fresh, tst_asic_live, tst_in_work, tst_tmax;
/* mm_telem_get() calls outside and inside ctl_tick(); inside-ctl_tick() flag */
static int   tst_telem_gets, tst_tick_gets, tst_in_tick;
static pid_t tst_next_pid = 4000;        /* next fake fork() PID */
static pid_t tst_pid_core, tst_pid_asic; /* last spawned core_miner and asic_miner PIDs */
/*
 * One-shot: next waitpid() reaps asic_miner; one-shot: reap core_miner then
 * asic_miner; core_miner of that pair already reaped.
 */
static int tst_reap_asic, tst_reap_pair, tst_reap_pair_core_done;
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

static int ev_fix(int from, int val)
{
    for (int i = from; i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, "fix") && tst_ev[i].val == val)
            return i;
    return -1;
}
static int p0(int p)
{
    return tst_pass_ev0[p < TST_MAX_PASS ? p : TST_MAX_PASS - 1];
}
static int p1(int p)
{
    return p0(p + 1);
}

static int pre1(int p)
{
    for (int i = p0(p); i < p1(p) && i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, "tick0"))
            return i;
    return p0(p);
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
                "g_run — aborting\n");
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
    tst_cur = tst_pass;
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
static void tst_telem_get(mm_telem_t *out)
{
    if (tst_in_tick)
        tst_tick_gets++;
    else
        tst_telem_gets++;
    mm_telem_get(out);
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
    ev(pid == tst_pid_core ? "kill-core" : pid == tst_pid_asic ? "kill-asic" : "kill-?", sig);
    return 0;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    if (status)
        *status = 0;
    if (tst_reap_asic && pid == tst_pid_asic) {
        tst_reap_asic = 0;
        ev("reap-asic", 0);
        return tst_pid_asic;
    }
    if (tst_reap_pair) {
        if (!tst_reap_pair_core_done && pid == tst_pid_core) {
            tst_reap_pair_core_done = 1;
            ev("reap-core", 0);
            return tst_pid_core;
        }
        if (tst_reap_pair_core_done && pid == tst_pid_asic) {
            tst_reap_pair = 0;
            ev("reap-asic", 0);
            return tst_pid_asic;
        }
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
    ev("tick0", (int)(now_ms / TST_TICK_MS));
    tst_in_tick = 1;
    int r       = ctl_tick(now_ms);
    tst_in_tick = 0;
    ev("tick1", (int)(now_ms / TST_TICK_MS));
    return r;
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

static void scenario(int passes, tst_script_fn s, int asic_live0)
{
    tst_now      = 0;
    tst_pass     = 0;
    tst_cur      = -1;
    tst_want     = passes;
    tst_script   = s;
    tst_sleeps   = 0;
    tst_runaway  = 0;
    tst_ev_n     = 0;
    tst_pid_core = tst_pid_asic = 0;
    tst_duty_x10                = 0;
    tst_reap_asic = tst_reap_pair = tst_reap_pair_core_done = 0;
    tst_telem_gets                                          = 0;
    tst_tick_gets                                           = 0;
    tst_telem_reset                                         = 1;
    memset(tst_pass_ev0, 0, sizeof tst_pass_ev0);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_api_req           = 0;
    tst_fresh           = 1;
    tst_asic_live       = asic_live0;
    tst_in_work         = 0;
    tst_tmax            = 60;
    g_run               = 1;
    tst_net_raise();
    mm_miner_main_unused();
}

static void s_dip(int p)
{
    tst_asic_live = (p >= TST_LIVE_PASS);
}
static void s_never(int p)
{
    (void)p;
}
static void s_live(int p)
{
    (void)p;
    tst_asic_live = 1;
}

static void s_crash(int p)
{
    tst_asic_live = !(p >= TST_CRASH_PASS && p <= TST_CRASH_PASS + 2);
    if (p == TST_CRASH_PASS)
        tst_reap_asic = 1;
}

static void s_thermal(int p)
{
    tst_asic_live = (p >= TST_LIVE_PASS && p <= TST_TRIP_PASS);
    if (p == TST_TRIP_PASS)
        g_overheat_req = 1;
    if (p == TST_TRIP_PASS + 1)
        tst_reap_asic = 1;
}

static void s_clobber(int p)
{
    if (p == 1)
        g_api_req = -1;
    if (p == 2)
        tst_reap_asic = 1;
    if (p == TST_ON_PASS)
        g_api_req = 1;
    if (p == TST_SOFTOFF2_PASS)
        g_api_req = -1;
    if (p == TST_CLOB_PASS)
        tst_asic_live = 1;
}

int main(void)
{
    printf("descriptor %s: %d board(s), enum pin %d%% (%s), API soft-off pin %d%%, "
           "softoff pin %d%%, hold %u ms (%d passes)\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, FAN_DUTY_ENUM_PCT,
            TST_PIN ? "live" : "INERT — this board has no enum pin", FAN_DUTY_API_SOFTOFF_PCT,
            FAN_DUTY_SOFTOFF_PCT, OVERHOT_HOLD_MS, TST_HOLD_PASS);

    scenario(TST_LIVE_PASS + 3, s_dip, 0);
    CHECK(!tst_runaway, "the loop exits on a cleared g_run (%d sleeps, cap %d)", tst_sleeps,
            TST_SLEEP_CAP);
    {
        static const char *const up[] = { "rail-on", "spawn-core", "spawn-asic" };
        int                      sa   = ev_first(0, "spawn-asic");
        CHECK(ev_is(0, 3, up, NULL) && tst_ev[sa].pass == -1 && sa < p0(0),
                "bring-up: rail, core_miner, then asic_miner at start-up (act %d, "
                "before pass 0 starts at act %d), still THROUGH spawn_asic, which "
                "is what proves the fork discriminator every section below reads. "
                "So the phase is ARMED from pass %d (saw %s)",
                sa, p0(0), TST_SPAWN_PASS, ev_str(0, 5));
    }
    {
        static const char *const pin[]  = { "fix", "duty" };
        const int                pinv[] = { FAN_DUTY_ENUM_PCT, FAN_DUTY_ENUM_PCT * 10 };
        int                      e0 = p0(TST_SPAWN_PASS), e1 = pre1(TST_SPAWN_PASS);
        CHECK(TST_PIN ? (ev_is(e0, 2, pin, pinv) && e0 + 2 == e1) : (e0 == e1),
                TST_PIN ? "ENUM_HOLD_PIN actuates BOTH halves on the first pass at which "
                          "asic_miner EXISTS (pass %d, spawned at start-up) — the PID "
                          "override AND a HAL write, at FAN_DUTY_ENUM_PCT (%d%% -> %d "
                          "x10) — and nothing else runs before ctl_tick (saw %s)" :
                          "this fork has NO enum pin (pass %d, FAN_DUTY_ENUM_PCT %d), so "
                          "the block actuates nothing at all — %d act(s) before ctl_tick "
                          "(saw %s)",
                TST_SPAWN_PASS, FAN_DUTY_ENUM_PCT, TST_PIN ? FAN_DUTY_ENUM_PCT * 10 : e1 - e0,
                ev_str(e0, e1));
    }
    {
        int rel = ev_fix(0, -1);
        CHECK(TST_PIN ? (rel >= 0 && tst_ev[rel].pass == TST_LIVE_PASS &&
                                (rel + 1 >= tst_ev_n || strcmp(tst_ev[rel + 1].what, "duty"))) :
                        rel < 0,
                TST_PIN ? "ENUM_HOLD_RELEASE writes the PID OVERRIDE ONLY — no HAL write "
                          "follows it — so the fan PHYSICALLY stays at the enum duty "
                          "until the PID's next 2 s tick picks something else. The PIN "
                          "arm actuates both halves; the RELEASE arm does not (release "
                          "on pass %d, next act %s)" :
                          "with no pin there is nothing to release either: not one "
                          "fan_pid_set_fixed call in the whole run (first release at act "
                          "%d, %s)",
                TST_PIN ? tst_ev[rel].pass : rel,
                TST_PIN ? (rel + 1 < tst_ev_n ? tst_ev[rel + 1].what : "(end)") : "inert");
    }
    CHECK(ev_count(0, tst_ev_n, "fix") == (TST_PIN ? 2 : 0) && g_fan_st.fixed_duty == -1 &&
                    g_enum_hold.phase == ENUM_HOLD_PHASE_NONE,
            TST_PIN ? "...and the whole window costs exactly %d override writes — ONE "
                      "pin and ONE release, never one per iteration — ending with the "
                      "fan back on the PID (%d) and the phase at zero-init (%d), so a "
                      "later spawn opens a FRESH window rather than inheriting this one "
                      "(saw %d)" :
                      "...and the window costs %d override writes on this fork: the fan "
                      "is never taken off the PID at all (%d), and the phase spawn_asic "
                      "armed is forced straight back to zero-init (%d) on the first "
                      "enum_hold_step call (saw %d)",
            TST_PIN ? 2 : 0, g_fan_st.fixed_duty, (int)g_enum_hold.phase,
            ev_count(0, tst_ev_n, "fix"));

    scenario(TST_CRASH_PASS + 6, s_crash, 1);
    {
        int rp  = ev_first(0, "reap-asic");
        int sp  = ev_first(rp < 0 ? 0 : rp, "spawn-asic");
        int pin = TST_PIN ? ev_fix(sp < 0 ? 0 : sp, FAN_DUTY_ENUM_PCT) : -1;
        CHECK(rp >= 0 && sp > rp && tst_ev[sp].now == tst_ev[rp].now + RESTART_MS &&
                        (TST_PIN ? (pin > sp && tst_ev[pin].now == tst_ev[sp].now + TST_TICK_MS &&
                                           tst_ev[pin].pass == tst_ev[sp].pass + 1) :
                                   ev_fix(sp, FAN_DUTY_ENUM_PCT) < 0),
                TST_PIN ? "a CRASH-respawn is made by mm_spawn_retry %u ms after the "
                          "reap, later in the pass than this block, so the pin lands on "
                          "the next supervised pass, as it does after the start-up "
                          "spawn (pin %llu ms; reap %llu ms, spawn %llu ms, pin %llu ms)" :
                          "a CRASH-respawn still goes through spawn_asic and still arms "
                          "the phase, but enum_hold_step forces it straight back to "
                          "zero-init on a fork with no enum duty, so no pin ever follows "
                          "(respawn %u ms after the reap; pin time %llu; reap %llu ms, "
                          "spawn %llu ms, pin %llu ms)",
                RESTART_MS, (unsigned long long)(pin >= 0 ? tst_ev[pin].now : 0),
                (unsigned long long)(rp >= 0 ? tst_ev[rp].now : 0),
                (unsigned long long)(sp >= 0 ? tst_ev[sp].now : 0),
                (unsigned long long)(pin >= 0 ? tst_ev[pin].now : 0));
    }

    scenario(TST_RESUME_PASS + 3, s_thermal, 0);
    {
        int                      r0 = p0(TST_RESUME_PASS), r1 = pre1(TST_RESUME_PASS);
        static const char *const res5[]  = { "fix", "rail-on", "spawn-asic", "fix", "duty" };
        const int                res5v[] = { -1, 0, 0, FAN_DUTY_ENUM_PCT, FAN_DUTY_ENUM_PCT * 10 };
        static const char *const res3[]  = { "fix", "rail-on", "spawn-asic" };

        int ok = TST_PIN ?
                (ev_is(r0, 5, res5, NULL) && r0 + 5 == r1 && tst_ev[r0].val == res5v[0] &&
                        tst_ev[r0 + 3].val == res5v[3] && tst_ev[r0 + 4].val == res5v[4]) :
                (ev_is(r0, 3, res3, NULL) && r0 + 3 == r1 && tst_ev[r0].val == -1);
        CHECK(ok && ev_count(r0, r1, "spawn-core") == 0,
                TST_PIN ? "the thermal RESUME pass reads: release the fan to the PID "
                          "(fix=-1), rail up, respawn asic_miner — and NO spawn-core, "
                          "because the thermal soft-off never stopped core_miner — and then "
                          "THIS block "
                          "pins it again at %d%% two statements later, in the same pass, "
                          "because the respawn sits ABOVE ctl_tick. The release does not "
                          "survive its own iteration, and that is correct: the chain is "
                          "re-enumerating (saw %s)" :
                          "the thermal RESUME pass ends where the thermal block left it: "
                          "release to the PID (fix=-1), rail up, respawn asic — three "
                          "acts, no spawn-core because core_miner was never stopped, and "
                          "no pin because this fork has none (%d; saw %s)",
                TST_PIN ? FAN_DUTY_ENUM_PCT : 0, ev_str(r0, r1));
    }
    CHECK(g_fan_st.fixed_duty == (TST_PIN ? FAN_DUTY_ENUM_PCT : -1),
            TST_PIN ? "...so the EFFECTIVE override after a thermal resume is %d, NOT "
                      "the -1 the thermal branch wrote two statements earlier (saw %d)" :
                      "...so the EFFECTIVE override after a thermal resume is %d — the "
                      "-1 the thermal branch wrote, with nothing to overwrite it (saw "
                      "%d)",
            TST_PIN ? FAN_DUTY_ENUM_PCT : -1, g_fan_st.fixed_duty);

    scenario(12, s_live, 1);
    {
        int rel = ev_fix(0, -1);
        CHECK(TST_PIN ? (ev_count(0, tst_ev_n, "fix") == 1 && rel < 0 &&
                                g_enum_hold.phase == ENUM_HOLD_PHASE_STALE) :
                        (ev_count(0, tst_ev_n, "fix") == 0 &&
                                g_enum_hold.phase == ENUM_HOLD_PHASE_NONE),
                TST_PIN ? "a heartbeat that reads 1 from the first pass and NEVER dips "
                          "to 0 leaves the pin ON for the whole run: %d override(s), no "
                          "release (%d), phase stuck at STALE (%d). Full airflow with no "
                          "timeout is the safe direction — but note the pin is then held "
                          "on the strength of a "
                          "heartbeat that may belong to a child that no longer exists" :
                          "with no enum pin the guard cannot be exercised at all: %d "
                          "override(s) (release at %d), phase forced to zero-init (%d)",
                ev_count(0, tst_ev_n, "fix"), rel, (int)g_enum_hold.phase);
    }
    scenario(12, s_dip, 1);
    CHECK((ev_fix(0, -1) >= 0) == (TST_PIN ? 1 : 0),
            "...and supplying that ONE 0 is the only difference needed to release "
            "it: the same run with asic_live dropped for the first %d passes "
            "%s (release at act %d)",
            TST_LIVE_PASS, TST_PIN ? "DOES release" : "still has nothing to release",
            ev_fix(0, -1));

    scenario(20, s_never, 0);
    CHECK(g_fan_st.fixed_duty == (TST_PIN ? FAN_DUTY_ENUM_PCT : -1) &&
                    ev_count(0, tst_ev_n, "fix") == (TST_PIN ? 1 : 0),
            TST_PIN ? "a chain that never reports in holds the pin for the ENTIRE run "
                      "and is pinned exactly ONCE, not once per iteration (final "
                      "override %d after %d passes, %d fan_pid_set_fixed call(s))" :
                      "a chain that never reports in changes nothing on this fork: the "
                      "override stays at the released sentinel %d for all %d passes and "
                      "the block never writes it (%d call(s))",
            g_fan_st.fixed_duty, tst_pass, ev_count(0, tst_ev_n, "fix"));

    scenario(TST_SPAWN_PASS + 3, s_never, 0);
    {
        int t   = ev_first(p0(TST_SPAWN_PASS), "tick0");
        int pin = ev_first(p0(TST_SPAWN_PASS), "fix");
        CHECK(t >= 0 && (TST_PIN ? (pin >= 0 && pin < t) : pin < 0),
                TST_PIN ? "the enum block runs BEFORE ctl_tick (pin at act %d, tick "
                          "marker at %d), so the fan rule sees the override on the same "
                          "iteration it is armed instead of up to one fan cadence later" :
                          "there is no pin to order against the tick on this fork (pin "
                          "%d, tick marker at %d) — the ordering claim needs one",
                pin, t);
    }

    scenario(TST_CLOB_PASS + 2, s_clobber, 0);
    {
        int k0 = p0(TST_CLOB_PASS), k1 = pre1(TST_CLOB_PASS);
        int rel = -1;
        for (int i = k0; i < k1; i++)
            if (!strcmp(tst_ev[i].what, "fix") && tst_ev[i].val == -1)
                rel = i;
        CHECK(g_softonoff.api_softoff == 1 &&
                        (TST_PIN ? (rel >= 0 && g_fan_st.fixed_duty == -1) :
                                   (rel < 0 && g_fan_st.fixed_duty == FAN_DUTY_API_SOFTOFF_PCT)),
                TST_PIN ? "with an API soft-off HELD (api_softoff %d), a ENUM_HOLD_RELEASE still "
                          "fires from this block and overwrites the soft-off's API pin "
                          "with -1 (act %d), leaving the override at %d for the rest of "
                          "the soft-off — enum_hold_step knows nothing about either latch. "
                          "The fan rule DOES pass them to enum_hold_active as held_off, "
                          "but that only gates the fan RULE, not this block. The duty on the "
                          "wire does not move "
                          "today, because softonoff_fan_hold makes fan_rule return before it "
                          "can actuate; the blast radius is the PID's override field. "
                          "REPORTED, NOT FIXED — pinned so it stays visible if either of "
                          "those two facts changes (saw %s)" :
                          "this fork cannot reach the clobber: no release is emitted "
                          "while the soft-off holds (%d / act %d), so the soft-off's own %d%% "
                          "pin survives (saw %s)",
                TST_PIN ? g_softonoff.api_softoff : g_softonoff.api_softoff, TST_PIN ? rel : rel,
                TST_PIN ? g_fan_st.fixed_duty : FAN_DUTY_API_SOFTOFF_PCT, ev_str(k0, k1));
    }

    scenario(4, s_never, 0);
    CHECK(tst_telem_gets == 2 * tst_pass && tst_tick_gets > 0,
            "each supervised iteration takes ONE mm_telem_get snapshot in the "
            "loop body, shared by the power check, this enum block and the core "
            "wedge check, plus exactly one in mm_periph_push; the rules take "
            "theirs inside ctl_tick on their own cadence (%d gets outside the "
            "tick over %d passes, want %d; %d inside it)",
            tst_telem_gets, tst_pass, 2 * tst_pass, tst_tick_gets);

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
