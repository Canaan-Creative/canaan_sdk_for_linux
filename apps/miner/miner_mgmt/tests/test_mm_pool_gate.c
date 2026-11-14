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

#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_MAX_PASS 64 /* longest scenario, passes */
/* osif_sleep_ms() calls before a scenario counts as runaway */
#define TST_SLEEP_CAP (TST_MAX_PASS * 8)
#define TST_TICK_MS   1000u /* main-loop pass length, ms */

#define TST_RESTART_PASSES ((int)(RESTART_MS / TST_TICK_MS)) /* passes in one restart delay */

#define TST_FAN_DUTY_EV /* log each fan duty set as a "duty" event */

pid_t                        tst_fork(void);
int                          tst_system(const char *cmd);
int                          tst_kill(pid_t pid, int sig);
pid_t                        tst_waitpid(pid_t pid, int *status, int options);
int                          tst_stat(const char *path, struct stat *st);
int                          tst_access(const char *path, int mode);
static void                  tst_fan_fixed(fan_pid_state_t *st, int duty_pct);
static int                   tst_ctl_tick(uint64_t now_ms);
static void                  tst_log(osif_loglvl_t lvl, const char *fmt, ...);
static thermal_telem_verdict tst_telem_check(thermal_telem_state_t *st, int asic_live,
        int temp_valid, uint64_t now_ms, uint32_t grace_ms);

#define fork()                  tst_fork()                 /* returns a fresh fake pid */
#define system(c)               tst_system(c)              /* no-op, returns 0 */
#define kill(p, s)              tst_kill((p), (s))         /* records a kill event */
#define waitpid(p, s, o)        tst_waitpid((p), (s), (o)) /* scripted reap */
#define stat(p, b)              tst_stat((p), (b))         /* every path missing */
#define access(p, m)            tst_access((p), (m))       /* network devices as scripted */
#define fan_pid_set_fixed(s, d) tst_fan_fixed((s), (d))    /* records a "fix" event */
#define ctl_tick(n)             tst_ctl_tick(n)            /* records a "tick" event, value in s */
#define osif_log(...)           tst_log(__VA_ARGS__)       /* keeps the last line */
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
#undef ctl_tick
#undef fan_pid_set_fixed
#undef access
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork

_Static_assert(ASIC_START_DELAY_S + 6 + TST_RESTART_PASSES <= TST_MAX_PASS,
        "the start-up delay scenarios must fit in TST_MAX_PASS passes");

/* Per-pass scenario step, called with the 0-based pass number */
typedef void (*tst_script_fn)(int pass);

static uint64_t tst_now; /* fake monotonic clock, ms */
/* passes run, passes wanted, osif_sleep_ms() calls, 1 = sleep cap exceeded */
static int           tst_pass, tst_want, tst_sleeps, tst_runaway;
static int           tst_pass_ev0[TST_MAX_PASS + 2]; /* tst_ev_n at the start of each pass */
static int           tst_net_up;                     /* 1 = network up */
static int           tst_no_ip;                      /* 1 = net_state_ip() reports no address */
static int           tst_netdev; /* 1 = every network device under MM_SYS_NET_DIR exists */
static tst_script_fn tst_script;
/* mm_telem_get() values: poll_fresh, asic_live, core_live, in_work flags; tmax, degC */
static int      tst_fresh, tst_asic_live, tst_core_live, tst_in_work, tst_tmax;
static int      tst_feeds;           /* wdt_dev_feed() calls */
static uint32_t tst_job_gap_s;       /* mm_telem_get() job_gap_s, s since the last pool job */
static int      tst_pool_configured; /* mm_cfgwr_pool_configured() result */
static int      tst_pool_reads;      /* mm_cfgwr_pool_configured() calls */
/* children_stop() return value; 1 = children_stop() was called */
static int           tst_join_rc, tst_in_tail;
static pid_t         tst_next_pid = 4000;          /* pid the next fork returns */
static pid_t         tst_pid_core, tst_pid_asic;   /* last forked core_miner/asic_miner pid */
static int           tst_reap_asic, tst_reap_core; /* 1 = next waitpid() reaps that child */
static char          tst_last_log[128];
static osif_loglvl_t tst_last_lvl;
static int           tst_telem_reset;    /* 1 = clear the thermal telem state on the next check */
extern int           stub_reboot_n;      /* mm_system_reboot() calls */
extern uint8_t       stub_reboot_reason; /* last mm_system_reboot() reason, MC_BOOTBY_* */

#include "tst_events.h"
#include "tst_fan_dev.h"

int net_state_get(void)
{
    return tst_net_up ? 2 : 0;
}
int mm_cfgwr_pool_configured(void)
{
    tst_pool_reads++;
    return tst_pool_configured;
}
int tst_access(const char *path, int mode)
{
    if (!strncmp(path, MM_SYS_NET_DIR, strlen(MM_SYS_NET_DIR)))
        return tst_netdev ? 0 : -1;
    return access(path, mode);
}
int net_state_ip(char *ip, size_t cap)
{
    if (tst_no_ip || !ip || !cap)
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
    out->job_gap_s  = tst_job_gap_s;
    out->poll_fresh = tst_fresh;
    out->ps_vout_mv = POWER_VALID_VOLT_MIN;
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
    tst_feeds++;
    ev("feed", 0);
}

int tst_children_stop(const pid_t *pids, int n, unsigned term_ms, unsigned kill_ms)
{
    (void)pids;
    (void)n;
    (void)term_ms;
    (void)kill_ms;
    tst_in_tail = 1;
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
    va_start(ap, fmt);
    vsnprintf(tst_last_log, sizeof tst_last_log, fmt, ap);
    va_end(ap);
    tst_last_lvl = lvl;
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
    tst_in_tail                   = 0;
    tst_join_rc                   = 0;
    tst_telem_reset               = 1;
    memset(tst_pass_ev0, 0, sizeof tst_pass_ev0);
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_api_req           = 0;
    tst_net_up          = 0;
    g_fault_off         = 0;
    atomic_store(&g_asic_spawn_at, 0);
    tst_fresh     = 1;
    tst_asic_live = 1;
    tst_core_live = 0;
    tst_in_work   = 1;
    tst_tmax      = 60;
    g_run         = 1;
    mm_miner_main_unused();
}

static void s_never_up(int p)
{
    (void)p;
}
static void tst_net_raise(void)
{
    tst_net_up = 1;
}

static void s_respawn(int p)
{
    if (p == 1)
        tst_net_raise();
    if (p == ASIC_START_DELAY_S + 3)
        tst_reap_asic = 1;
}
static void s_wait_api_off(int p)
{
    if (p == 1)
        g_softonoff.api_softoff = 1;
}
static void s_wait_fault(int p)
{
    if (p == 1)
        g_fault_off = 1;
}
static void s_wait_off_on(int p)
{
    if (p == 1)
        g_api_req = -1;
    if (p == 2)
        g_api_req = 1;
}
static void s_wait_thermal(int p)
{
    if (p == 1)
        g_overheat_req = 1;
}

static void test_pool_idle(void)
{
    tst_last_log[0] = '\0';
    tst_job_gap_s   = 0;
    pool_idle_rule(NULL, 0);
    mm_status_push();
    CHECK(!(g_live_ecmm & ERR_MM_POOL_INACTIVE) && stub_reboot_n == 0,
            "before the first pool job nothing is flagged or rebooted");

    tst_job_gap_s = 121;
    pool_idle_rule(NULL, 0);
    mm_status_push();
    CHECK((g_live_ecmm & ERR_MM_POOL_INACTIVE) && stub_reboot_n == 0,
            "a job gap over 120 s with an IP sets ERR_MM_POOL_INACTIVE");

    tst_job_gap_s = 120;
    pool_idle_rule(NULL, 0);
    mm_status_push();
    CHECK(!(g_live_ecmm & ERR_MM_POOL_INACTIVE), "a job gap of 120 s or less clears it");

    tst_no_ip     = 1;
    tst_job_gap_s = 200;
    pool_idle_rule(NULL, 0);
    mm_status_push();
    CHECK(!(g_live_ecmm & ERR_MM_POOL_INACTIVE) && stub_reboot_n == 0,
            "without an IP the gap sets no error bit");

    tst_job_gap_s = 361;
    pool_idle_rule(NULL, 0);
    CHECK(stub_reboot_n == 1 && stub_reboot_reason == MC_BOOTBY_POOL_INACTIVE &&
                    tst_last_lvl == OSIF_LOG_WARN &&
                    !strcmp(tst_last_log, "no new job > 6 min, none ip"),
            "a job gap over 360 s logs and reboots with MC_BOOTBY_POOL_INACTIVE "
            "(%d reboot(s), reason 0x%02x, \"%s\")",
            stub_reboot_n, stub_reboot_reason, tst_last_log);

    tst_no_ip = 0;
    atomic_store(&g_rebooting, 1);
    pool_idle_rule(NULL, 0);
    CHECK(stub_reboot_n == 1, "no second reboot while one is in progress");
    atomic_store(&g_rebooting, 0);
    pool_idle_rule(NULL, 0);
    CHECK(stub_reboot_n == 2 && !strcmp(tst_last_log, "no new job > 6 min, has ip"),
            "the log names an IP when there is one (\"%s\")", tst_last_log);

    tst_job_gap_s = 0;
    pool_idle_rule(NULL, 0);
    stub_reboot_n = 0;
}

static void test_net_bits(void)
{
    const uint32_t net_bits = ERR_MM_LAN_DAMAGED | ERR_MM_IPLOST;

    tst_netdev = 0;
    tst_no_ip  = 1;
    mm_status_push();
    CHECK((g_live_ecmm & net_bits) == net_bits,
            "no network device and no IP set ERR_MM_LAN_DAMAGED and ERR_MM_IPLOST");
    CHECK(!g_fault_off && g_softoff_rsn != MC_SOFTOFF_LAN_DAMAGED,
            "a missing network device does not soft-off");

    tst_netdev = 1;
    mm_status_push();
    CHECK((g_live_ecmm & net_bits) == ERR_MM_IPLOST,
            "a network device without an IP sets only ERR_MM_IPLOST");

    tst_no_ip = 0;
    mm_status_push();
    CHECK(!(g_live_ecmm & net_bits), "a network device with an IP clears both");

    tst_netdev = 0;
    tst_no_ip  = 1;
    mm_status_push();
    CHECK((g_live_ecmm & net_bits) == net_bits, "a lost network device sets both again");
    tst_no_ip = 0;
}

static void test_pool_unreached(void)
{
    tst_pool_configured = 1;
    tst_no_ip           = 0;
    tst_job_gap_s       = 5;
    pool_idle_rule(NULL, 1000);
    tst_job_gap_s  = 0;
    tst_pool_reads = 0;
    pool_idle_rule(NULL, 10000);
    pool_idle_rule(NULL, 130000);
    CHECK(!atomic_load(&g_pool_unreached) && tst_pool_reads == 0,
            "no first pool job for 120 s is not yet flagged, and pools.conf is not read");
    pool_idle_rule(NULL, 130001);
    CHECK(atomic_load(&g_pool_unreached),
            "a configured pool with no first job 120 s after the IP is flagged");
    mm_status_push();
    CHECK(!(g_live_ecmm & ERR_MM_POOL_INACTIVE) && stub_reboot_n == 0,
            "...on the screen only: no ECMM bit, no reboot");

    tst_pool_configured = 0;
    pool_idle_rule(NULL, 160000);
    CHECK(!atomic_load(&g_pool_unreached), "no configured pool is not flagged");
    tst_pool_configured = 1;

    tst_job_gap_s = 3;
    pool_idle_rule(NULL, 190000);
    CHECK(!atomic_load(&g_pool_unreached), "the first pool job clears the flag");

    tst_job_gap_s = 0;
    tst_no_ip     = 1;
    pool_idle_rule(NULL, 200000);
    tst_no_ip = 0;
    pool_idle_rule(NULL, 300000);
    pool_idle_rule(NULL, 420000);
    CHECK(!atomic_load(&g_pool_unreached), "the wait restarts when the IP returns");
    pool_idle_rule(NULL, 420001);
    CHECK(atomic_load(&g_pool_unreached), "...and flags 120 s after it");

    tst_job_gap_s = 0;
    tst_no_ip     = 1;
    pool_idle_rule(NULL, 430000);
    CHECK(!atomic_load(&g_pool_unreached), "losing the IP clears the flag");
    tst_no_ip           = 0;
    tst_pool_configured = 0;
}

static int tst_gate_first_unfed(int ticks)
{
    wdt_flags_t    f;
    wdt_grace_t    grace[WDT_TID_TOTAL];
    uint64_t       healthy_at = tst_now;
    fault_report_t rep;

    wdt_flags_reset(&f);
    memset(grace, 0, sizeof grace);
    memset(&rep, 0, sizeof rep);
    tst_asic_live = 0;
    tst_core_live = 1;
    for (int i = 0; i < ticks; i++) {
        int fed = tst_feeds;

        tst_now += TST_TICK_MS;
        atomic_store(&g_wdt_beat, 1);
        mm_wdt_tick(&f, grace, &healthy_at, &rep, tst_now);
        if (tst_feeds == fed)
            return i;
    }
    return -1;
}

static void test_start_delay(void)
{
    int ticks = (int)(2u * WDT_STARVE_MS / TST_TICK_MS);
    int a, u;

    scenario(ASIC_START_DELAY_S, s_never_up);
    CHECK(!tst_runaway && ev_count(0, tst_ev_n, "spawn-asic") == 0 && g_softonoff.start_wait == 1 &&
                    (softonoff_children_stopped(&g_softonoff, g_fault_off) & SOFTONOFF_CHILD_ASIC),
            "for the whole %d s start-up delay no asic_miner is spawned and "
            "start_wait stays set, so the ASIC leg counts as stopped (spawns %d, "
            "latch %d)",
            ASIC_START_DELAY_S, ev_count(0, tst_ev_n, "spawn-asic"), g_softonoff.start_wait);
    CHECK(atomic_load(&g_asic_spawn_at) == 0,
            "...and asic_miner has never been spawned, so no spawn grace covers "
            "the ASIC leg");

    u = tst_gate_first_unfed(ticks);
    CHECK(u < 0,
            "with the state the delay leaves (start_wait set, asic_miner never "
            "spawned, no asic telemetry, core_miner live) the feed gate feeds for "
            "all %d ticks, past WDT_STARVE_MS %u ms (first unfed %d)",
            ticks, WDT_STARVE_MS, u);

    g_softonoff.start_wait = 0;
    u                      = tst_gate_first_unfed(ticks);
    CHECK(u >= 0,
            "...and the exemption is start_wait alone: cleared, with asic_miner "
            "still never spawned, the gate stops feeding (first unfed %d)",
            u);

    scenario(ASIC_START_DELAY_S + 3, s_never_up);
    a = ev_first(0, "spawn-asic");
    CHECK(ev_count(0, tst_ev_n, "spawn-asic") == 1 && a >= 0 &&
                    tst_ev[a].now >= (uint64_t)ASIC_START_DELAY_S * 1000u &&
                    tst_ev[a].now < (uint64_t)ASIC_START_DELAY_S * 1000u + TST_TICK_MS &&
                    g_softonoff.start_wait == 0,
            "without a pool, asic_miner is spawned once, on the first pass at or after the "
            "%d s delay (spawns %d, at %llu ms, latch %d)",
            ASIC_START_DELAY_S, ev_count(0, tst_ev_n, "spawn-asic"),
            a >= 0 ? (unsigned long long)tst_ev[a].now : 0ull, g_softonoff.start_wait);

    scenario(ASIC_START_DELAY_S + 6 + TST_RESTART_PASSES, s_respawn);
    {
        int r = ev_first(0, "reap-asic");
        int b = r >= 0 ? ev_first(r, "spawn-asic") : -1;
        CHECK(ev_count(0, tst_ev_n, "spawn-asic") == 2 && r >= 0 && b >= 0 &&
                        tst_ev[b].now - tst_ev[r].now >= RESTART_MS &&
                        tst_ev[b].now - tst_ev[r].now < RESTART_MS + TST_TICK_MS &&
                        g_softonoff.start_wait == 0,
                "an asic_miner exit after the first spawn is respawned RESTART_MS "
                "%u ms later, not after another start-up delay (spawns %d, "
                "respawn %lld ms after the exit, latch %d)",
                RESTART_MS, ev_count(0, tst_ev_n, "spawn-asic"),
                r >= 0 && b >= 0 ? (long long)(tst_ev[b].now - tst_ev[r].now) : -1ll,
                g_softonoff.start_wait);
    }
}

static void test_start_delay_holds(void)
{
    int a;

    scenario(ASIC_START_DELAY_S + 3, s_wait_api_off);
    CHECK(ev_count(0, tst_ev_n, "spawn-asic") == 0 && g_softonoff.start_wait == 0,
            "the start delay ending under a latched API soft-off consumes start_wait but "
            "spawns NOTHING (spawns %d, latch %d)",
            ev_count(0, tst_ev_n, "spawn-asic"), g_softonoff.start_wait);

    scenario(ASIC_START_DELAY_S + 3, s_wait_fault);
    CHECK(ev_count(0, tst_ev_n, "spawn-asic") == 0 && g_softonoff.start_wait == 0,
            "the start delay ending under a POWER_FAULT off consumes start_wait but spawns "
            "NOTHING (spawns %d, latch %d)",
            ev_count(0, tst_ev_n, "spawn-asic"), g_softonoff.start_wait);

    scenario(ASIC_START_DELAY_S + 3, s_wait_thermal);
    CHECK(g_softonoff.thermal_until && !g_softonoff.start_wait &&
                    ev_count(0, tst_ev_n, "spawn-asic") == 0,
            "the start delay ending during a thermal hold cannot spawn ASIC (spawns %d, "
            "latch %d)",
            ev_count(0, tst_ev_n, "spawn-asic"), g_softonoff.start_wait);

    scenario(ASIC_START_DELAY_S - 2, s_wait_off_on);
    CHECK(!g_softonoff.api_softoff && g_softonoff.start_wait &&
                    ev_count(0, tst_ev_n, "spawn-asic") == 0,
            "off/on during the start delay records ON without spawning ASIC");

    scenario(ASIC_START_DELAY_S + 3, s_wait_off_on);
    a = ev_first(0, "spawn-asic");
    CHECK(a >= 0 && tst_ev[a].now >= (uint64_t)ASIC_START_DELAY_S * 1000u &&
                    tst_ev[a].now < (uint64_t)ASIC_START_DELAY_S * 1000u + TST_TICK_MS &&
                    ev_count(0, tst_ev_n, "spawn-asic") == 1 && !g_softonoff.start_wait,
            "deferred ON starts exactly once when the start delay ends (spawns %d, at %llu ms)",
            ev_count(0, tst_ev_n, "spawn-asic"), a >= 0 ? (unsigned long long)tst_ev[a].now : 0ull);
}

int main(void)
{
    printf("descriptor %s: %d board(s), fail-safe max %d%%, ASIC_START_DELAY_S %d\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, FAN_DUTY_MAX_PCT, ASIC_START_DELAY_S);

    test_pool_idle();
    test_net_bits();
    test_pool_unreached();
    test_start_delay();
    test_start_delay_holds();

    scenario(ASIC_START_DELAY_S + 3, s_never_up);
    CHECK(!tst_runaway, "the loop exits on a cleared g_run (%d sleeps)", tst_sleeps);
    {
        static const char *const up[] = { "rail-on", "spawn-core" };
        CHECK(ev_is(0, 2, up, NULL) && ev_count(0, tst_ev_n, "spawn-asic") == 1,
                "cold start with no network and no pool: rail up, core_miner "
                "spawned at start-up, and asic_miner spawned once after the "
                "start-up delay (saw %s)",
                ev_str(0, 5));
    }

    return tst_done();
}
