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
#include "cali.h"
#include "fan_dev.h"
#include "mm_telem.h"
#include "osif.h"
#include "psu_ops.h"
#include "wdt_dev.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* no first asic_miner start delay, s */
#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TST_EV_MAX    256  /* capacity of tst_ev[], events; later events are dropped */
#define TST_SLEEP_CAP 4096 /* osif_sleep_ms() calls before a scenario counts as runaway */
#if defined(BOARD_NANO3S_H)
#define TST_OTHER      "x3" /* the other board, whose rail window must not overlap */
#define TST_OTHER_MIN  2400 /* its rail floor, mV */
#define TST_OTHER_MAX  3000 /* its rail ceiling, mV */
#define TST_OTHER_SEED 2700 /* a rail request inside its window, mV */
#elif defined(BOARD_X3_H)
#define TST_OTHER      "nano3s" /* the other board, whose rail window must not overlap */
#define TST_OTHER_MIN  3200     /* its rail floor, mV */
#define TST_OTHER_MAX  4000     /* its rail ceiling, mV */
#define TST_OTHER_SEED 3450     /* a rail request inside its window, mV */
#else
#error "no other-board rail window for this board"
#endif
#define TST_UNIT_DIV 1000 /* this board's rail counts per volt (mV counts) */

int     tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
            cali_param *out, uint8_t *aged);
uint8_t tst_workmode_get(const cali_wm_store *s);
void    tst_log(osif_loglvl_t lvl, const char *fmt, ...);
pid_t   tst_fork(void);
int     tst_system(const char *cmd);
int     tst_kill(pid_t pid, int sig);
pid_t   tst_waitpid(pid_t pid, int *status, int options);
int     tst_stat(const char *path, struct stat *st);

/* cali_resolve() returning tst_volt and tst_aged */
#define cali_resolve(p, c, m, l, o, a) tst_cali_resolve((p), (c), (m), (l), (o), (a))
#define cali_workmode_get(p)           tst_workmode_get(p)        /* returns tst_mode */
#define osif_log(...)                  tst_log(__VA_ARGS__)       /* keeps the last line */
#define fork()                         tst_fork()                 /* returns a fresh fake pid */
#define system(c)                      tst_system(c)              /* no-op, returns 0 */
#define kill(p, s)                     tst_kill((p), (s))         /* no-op, returns 0 */
#define waitpid(p, s, o)               tst_waitpid((p), (s), (o)) /* scripted reap */
#define stat(p, b)                     tst_stat((p), (b))         /* every path missing */

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
#include "mm_miner_tu.h"
#undef stat
#undef waitpid
#undef kill
#undef system
#undef fork
#undef osif_log
#undef cali_workmode_get
#undef cali_resolve

extern uint16_t tst_rail_mv; /* last clamped mm_vf_set_rail() value, rail counts */

static uint16_t             tst_volt;       /* volt cali_resolve() returns, rail counts */
static uint8_t              tst_mode = 2;   /* work mode cali_workmode_get() returns */
static uint8_t              tst_aged;       /* aged flag cali_resolve() returns, 1 = calibrated */
static const char          *tst_seen_ini;   /* path last passed to cali_resolve() */
static const cali_wm_store *tst_seen_wm;    /* store last passed to cali_workmode_get() */
static uint16_t             tst_seen_cnt;   /* ASIC count last passed to cali_resolve() */
static uint8_t              tst_seen_mode;  /* work mode last passed to cali_resolve() */
static int8_t               tst_seen_level; /* level last passed to cali_resolve() */
static int                  tst_resolves;   /* cali_resolve() calls */
static char                 tst_ev[TST_EV_MAX][16]; /* recorded events, e.g. "rail-on", "fan50" */
static int                  tst_ev_n;
static char                 tst_last_log[512];
static osif_loglvl_t        tst_last_lvl;
/* Log lines, and startup hashboard power-on failure reports among them */
static int      tst_lines, tst_on_reports;
static char     tst_on_line[512];
static uint64_t tst_now; /* fake monotonic clock, ms */
/* passes run, passes wanted, osif_sleep_ms() calls, 1 = sleep cap exceeded */
static int      tst_pass, tst_want, tst_sleeps, tst_runaway;
static int      tst_overheat_at  = -1;       /* pass that raises an overheat request, -1 = none */
static int      tst_fan_fault_at = -1;       /* pass that raises a fan-fault request, -1 = none */
static int      tst_duty_x10;                /* last fan duty set, 0.1 % */
static int      tst_off_fails, tst_on_fails; /* 1 = rail off / rail on is unconfirmed */
static pid_t    tst_next_pid = 4000;         /* pid the next successful fork returns */
static pid_t    tst_pid_core, tst_pid_asic;  /* last forked core_miner/asic_miner pid */
static uint16_t tst_on_counts; /* value last passed to board_power_rail_on(), rail counts */
static int      tst_off_recover_pass = -1;    /* pass from which rail off succeeds, -1 = never */
static int      tst_reboot_in_prepare;        /* 1 = the pout-limit read sets g_rebooting */
static int      tst_reap_core, tst_reap_asic; /* 1 = next waitpid() reaps that child */
static int      tst_unknown_n;                /* mm_vf_rail_unknown() calls */
static int      tst_unknown_offs;             /* rail-offs seen by the last mm_vf_rail_unknown() */

uint8_t tst_workmode_get(const cali_wm_store *s)
{
    tst_seen_wm = s;
    return tst_mode;
}

int tst_cali_resolve(const char *path, uint16_t asic_cnt, uint8_t mode, int8_t level,
        cali_param *out, uint8_t *aged)
{
    tst_resolves++;
    tst_seen_ini   = path;
    tst_seen_cnt   = asic_cnt;
    tst_seen_mode  = mode;
    tst_seen_level = level;
    if (out) {
        memset(out, 0, sizeof *out);
        out->volt     = tst_volt;
        out->max_pout = 62;
        out->temp     = 90;
    }
    if (aged)
        *aged = tst_aged;
    return 0;
}

static void ev(const char *fmt, ...)
{
    va_list ap;
    if (tst_ev_n >= TST_EV_MAX)
        return;
    va_start(ap, fmt);
    vsnprintf(tst_ev[tst_ev_n++], sizeof tst_ev[0], fmt, ap);
    va_end(ap);
}
static const char *ev_join(char *out, size_t cap)
{
    out[0] = '\0';
    for (int k = 0; k < tst_ev_n; k++) {
        if (out[0])
            strncat(out, ",", cap - strlen(out) - 1);
        strncat(out, tst_ev[k], cap - strlen(out) - 1);
    }
    return out;
}

void tst_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tst_last_log, sizeof tst_last_log, fmt, ap);
    va_end(ap);
    tst_last_lvl = lvl;
    tst_lines++;
    if (strstr(tst_last_log, "hashboard power-on failed at startup")) {
        tst_on_reports++;
        memcpy(tst_on_line, tst_last_log, sizeof tst_on_line);
    }
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
    if (tst_pass == tst_overheat_at)
        g_overheat_req = 1;
    if (tst_pass == tst_fan_fault_at)
        g_fan_fault_req = 1;
    if (++tst_pass >= tst_want)
        g_run = 0;
}

void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
    out->ps_vout_mv = POWER_VALID_VOLT_MIN;
    out->tmax       = 60;
    out->tavg       = 55;
    if (CAP_HB_MCU)
        out->hb_count = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++)
        out->hb_outlet[b] = out->hb_inlet[b] = 60;
}

int tst_children_stop(const pid_t *pids, int n, unsigned t, unsigned k)
{
    (void)pids;
    (void)n;
    (void)t;
    (void)k;
    ev("join");
    return 0;
}

int fan_dev_init(void)
{
    return 0;
}
void fan_dev_set_duty_x10(int duty_x10)
{
    tst_duty_x10 = duty_x10;
    ev("fan%d", duty_x10 / 10);
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
    return WDT_TIMEOUT_S;
}
int wdt_dev_feed_period_ms(void)
{
    return WDT_FEED_PERIOD_MS_MAX;
}
void wdt_dev_disarm(void)
{
    ev("disarm");
}
void wdt_dev_feed(void)
{
}

pid_t tst_fork(void)
{
    int   is_asic = (g_enum_hold.phase == ENUM_HOLD_PHASE_ARMED);
    pid_t p       = tst_next_pid++;
    if (is_asic)
        tst_pid_asic = p;
    else
        tst_pid_core = p;
    ev("spawn");
    return p;
}
int tst_system(const char *cmd)
{
    (void)cmd;
    return 0;
}

int board_power_rail_on(uint16_t counts)
{
    tst_on_counts = counts;
    ev("rail-on");
    return tst_on_fails ? HAL_PSU_RAIL_E_UNCONFIRMED : HAL_PSU_RAIL_OK;
}
int board_power_rail_off(void)
{
    ev("rail-off");
    if (tst_off_recover_pass >= 0 && tst_pass >= tst_off_recover_pass)
        return HAL_PSU_RAIL_OK;
    return tst_off_fails ? HAL_PSU_RAIL_E_UNCONFIRMED : HAL_PSU_RAIL_OK;
}
const char *hal_psu_rail_strerror(int rc)
{
    (void)rc;
    return "test";
}
static int tst_pout_limit(uint16_t *watts)
{
    *watts = 140;
    if (tst_reboot_in_prepare)
        atomic_store(&g_rebooting, 1);
    return 0;
}
const hal_psu_ops_t *hal_psu(void)
{
    static const hal_psu_ops_t psu = {
        .pout_limit = tst_pout_limit,
        .vout_fault = tst_psu_vout_fault,
        .flags      = TST_PSU_FLAGS,
    };
    return &psu;
}
int tst_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    return 0;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    if (status)
        *status = 0;
    if (pid > 0 && pid == tst_pid_core && tst_reap_core) {
        tst_reap_core = 0;
        return pid;
    }
    if (pid > 0 && pid == tst_pid_asic && tst_reap_asic) {
        tst_reap_asic = 0;
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

static void fan_telem_reset(void)
{
    softonoff_state_t held = { .api_softoff = 1 };
    enum_hold_t       eh   = { 0 };
    mm_fan_ctx_t      fc   = { .softonoff = &held, .enum_hold = &eh };

    atomic_store(&g_fan_dev_ok, 1);
    fan_rule(&fc, 0);
}

static void scenario(int passes, int overheat_at, int api_req, int off_fails, int on_fails)
{
    fan_telem_reset();
    tst_now         = 0;
    tst_pass        = 0;
    tst_want        = passes;
    tst_overheat_at = overheat_at;
    tst_sleeps      = 0;
    tst_runaway     = 0;
    tst_ev_n        = 0;
    tst_duty_x10    = 0;
    tst_off_fails   = off_fails;
    tst_on_fails    = on_fails;
    tst_lines       = 0;
    tst_on_reports  = 0;
    tst_reap_core   = 0;
    tst_reap_asic   = 0;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_fault_off         = 0;
    g_api_req           = api_req;
    g_run               = 1;
    mm_miner_main_unused();
}

static int ev_count(const char *what)
{
    int n = 0;
    for (int k = 0; k < tst_ev_n; k++)
        if (!strcmp(tst_ev[k], what))
            n++;
    return n;
}

void mm_vf_rail_unknown(void)
{
    tst_unknown_n++;
    tst_unknown_offs = ev_count("rail-off");
}

static long stage2(void)
{
    cali_param cp;
    uint8_t    aged = 0;
    uint8_t    mode = tst_workmode_get(&g_wm_store);
    tst_cali_resolve(CALI_INI_PATH, MAX_ASIC_ON_SINGLE_HASH, mode, 0, &cp, &aged);
    mm_vf_set_rail(cp.volt);
    uint16_t got = tst_rail_mv;
    tst_log(OSIF_LOG_INFO, "init rail at mode %u: %u (%s), ramping", (unsigned)mode, got,
            aged ? "calibrated" : "table");
    return got;
}

static long ask(uint32_t want)
{
    tst_volt = (uint16_t)want;
    return stage2();
}

static const char *volts(long count)
{
    static char b[4][32];
    static int  i;
    char       *s = b[i = (i + 1) & 3];
    if (TST_UNIT_DIV == 100)
        snprintf(s, 32, "%ld.%02ld V", count / 100, count % 100);
    else
        snprintf(s, 32, "%ld.%03ld V", count / 1000, count % 1000);
    return s;
}

int main(void)
{
    printf("descriptor %s: rail clamp [%d..%d] = %s..", BOARDCONF_SECTION, VOL_MIN, VOL_MAX,
            volts(VOL_MIN));
    printf("%s, tool default VOL_ENUM %d, %d board(s)\n", volts(VOL_MAX), VOL_ENUM,
            (int)HBOARD_COUNT);

    CHECK(VOL_MIN < VOL_MAX && VOL_MIN > 0,
            "the window is non-empty and ordered [%d..%d] (%s..%s) — an inverted "
            "pair would make the two clamps fight and the second one win",
            VOL_MIN, VOL_MAX, volts(VOL_MIN), volts(VOL_MAX));

    CHECK(VOL_ENUM >= VOL_MIN && VOL_ENUM <= VOL_MAX,
            "the tool's own default VOL_ENUM %d (%s) is inside mm's clamp, so the "
            "two agree about this board's safe window",
            VOL_ENUM, volts(VOL_ENUM));

    tst_volt = VOL_ENUM;
    CHECK(rail_enable_counts() == VOL_ENUM,
            "mm energizes at VOL_ENUM %d (%s), inside [%d..%d] (got %u)", VOL_ENUM, volts(VOL_ENUM),
            VOL_MIN, VOL_MAX, (unsigned)rail_enable_counts());

    {
        long bad_lo = -1, bad_hi = -1, bad_in = -1;
        long got_lo = -1, got_hi = -1, got_in = -1;
        int  nlo = 0, nhi = 0, nin = 0;
        for (uint32_t v = 0; v <= 0xffffu; v++) {
            long g    = ask(v);
            long want = (v < (uint32_t)VOL_MIN) ? VOL_MIN :
                    (v > (uint32_t)VOL_MAX)     ? VOL_MAX :
                                                  (long)v;
            if (g == want)
                continue;
            if (v < (uint32_t)VOL_MIN) {
                if (!nlo++) {
                    bad_lo = v;
                    got_lo = g;
                }
            } else if (v > (uint32_t)VOL_MAX) {
                if (!nhi++) {
                    bad_hi = v;
                    got_hi = g;
                }
            } else {
                if (!nin++) {
                    bad_in = v;
                    got_in = g;
                }
            }
        }
        CHECK(nlo == 0,
                "all %d requests below the floor clamp UP to %d (%s) — "
                "%d wrong, first %ld -> %ld",
                VOL_MIN, VOL_MIN, volts(VOL_MIN), nlo, bad_lo, got_lo);
        CHECK(nhi == 0,
                "all %d requests above the ceiling clamp DOWN to %d (%s) "
                "— this is the over-volt arm; %d wrong, first %ld -> %ld",
                0xffff - VOL_MAX, VOL_MAX, volts(VOL_MAX), nhi, bad_hi, got_hi);
        CHECK(nin == 0,
                "all %d in-band requests [%d..%d] pass through "
                "UNCHANGED — the clamp bounds, it does not quantise or re-seed; "
                "%d wrong, first %ld -> %ld",
                VOL_MAX - VOL_MIN + 1, VOL_MIN, VOL_MAX, nin, bad_in, got_in);
    }

    CHECK(ask(0) == VOL_MIN,
            "a ZERO request — what an all-zero or truncated cali record leaves in "
            "cp.volt — comes up at the floor %d (%s), not at 0",
            VOL_MIN, volts(VOL_MIN));

    CHECK(ask(VOL_MIN) == VOL_MIN && ask(VOL_MIN - 1) == VOL_MIN,
            "the floor %d (%s) is itself commandable and %d is the first request "
            "clamped up — the window is closed at the bottom",
            VOL_MIN, volts(VOL_MIN), VOL_MIN - 1);

    CHECK(ask(VOL_MAX) == VOL_MAX && ask(VOL_MAX + 1) == VOL_MAX,
            "the ceiling %d (%s) is itself commandable and %d is the first request "
            "clamped down — the window is closed at the top, and this is the edge "
            "that decides how hard the ASICs can be driven",
            VOL_MAX, volts(VOL_MAX), VOL_MAX + 1);

    CHECK(VOL_MAX < TST_OTHER_MIN || VOL_MIN > TST_OTHER_MAX,
            "this board's window [%d..%d] and %s's [%d..%d] do NOT overlap, so a "
            "shared literal cannot pass unnoticed on either board",
            VOL_MIN, VOL_MAX, TST_OTHER, TST_OTHER_MIN, TST_OTHER_MAX);

    {
        long g    = ask(TST_OTHER_SEED);
        long want = TST_OTHER_SEED < VOL_MIN ? VOL_MIN : VOL_MAX;
        CHECK(g == want && g != TST_OTHER_SEED,
                "%s's own rail seed %d is REFUSED here — commanded %ld (%s), not "
                "%d (got %ld)",
                TST_OTHER, TST_OTHER_SEED, want, volts(want), TST_OTHER_SEED, g);
    }

    for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
        char want[64];
        tst_mode     = m;
        tst_resolves = 0;
        tst_volt     = (uint16_t)(VOL_MIN + m);
        long g       = stage2();
        snprintf(want, sizeof want, "mode %u: %d (", (unsigned)m, VOL_MIN + m);
        CHECK(tst_seen_mode == m && g == VOL_MIN + m && tst_resolves == 1 &&
                        strstr(tst_last_log, want) != NULL,
                "the SELECTED work mode reaches cali_resolve, its voltage is the "
                "one commanded, and the log says so — mode %u resolved %d and "
                "commanded %ld in exactly %d resolve(s), logging \"%s\"",
                (unsigned)m, VOL_MIN + m, g, tst_resolves, want);
    }
    tst_mode = 2;

    CHECK(tst_seen_cnt == MAX_ASIC_ON_SINGLE_HASH,
            "cali_resolve is asked for THIS board's whole chain — %u ASICs across "
            "%d board(s), the descriptor's MAX_ASIC_ON_SINGLE_HASH — since a count "
            "with no factory row falls back to this product's own row-0 mode 0 "
            "(cali_default_param)",
            tst_seen_cnt, (int)HBOARD_COUNT);

    CHECK(tst_seen_level == 0,
            "level 0 is asked for, the only one that exists (CALI_LEVEL_COUNT %d)",
            CALI_LEVEL_COUNT);

    CHECK(tst_seen_ini && !strcmp(tst_seen_ini, CALI_INI_PATH) && tst_seen_wm == &g_wm_store &&
                    g_wm_store.get == mm_cfgstore_get,
            "the persisted cali record is read from %s and the persisted mode "
            "selection from [syscfg] work_mode in the %s RAM copy",
            CALI_INI_PATH, MC_SYSTEMCFG_DEFAULT);

    {
        char want[64];
        tst_aged = 1;
        long g   = ask(TST_OTHER_SEED);
        snprintf(want, sizeof want, ": %ld (", g);
        CHECK(strstr(tst_last_log, want) != NULL && strstr(tst_last_log, "init rail") != NULL &&
                        tst_last_lvl == OSIF_LOG_INFO,
                "the INFO line reports the COMMANDED count %ld, not the requested "
                "%d — it is logged after the clamp (saw \"%s\")",
                g, TST_OTHER_SEED, tst_last_log);

        snprintf(want, sizeof want, "%d", TST_OTHER_SEED);
        CHECK(strstr(tst_last_log, want) == NULL,
                "...and the REFUSED request %d never appears in that line "
                "(saw \"%s\")",
                TST_OTHER_SEED, tst_last_log);

        CHECK(strstr(tst_last_log, " mV") == NULL && strstr(tst_last_log, " V") == NULL,
                "...and it prints the raw count with no unit (saw \"%s\")", tst_last_log);

        CHECK(strstr(tst_last_log, "calibrated") != NULL,
                "...and whether the voltage came from a PERSISTED calibration or "
                "the factory table (saw \"%s\")",
                tst_last_log);
        tst_aged = 0;
        ask(VOL_ENUM);
        CHECK(strstr(tst_last_log, "table") != NULL, "...both ways round (saw \"%s\")",
                tst_last_log);
    }

    {
        char clean[2048], failed[2048];

        scenario(4, -1, 0, 0, 0);
        scenario(4, -1, 0, 0, 0);
        ev_join(clean, sizeof clean);
        int offs_clean  = ev_count("rail-off");
        int lines_clean = tst_lines;
        CHECK(!tst_runaway && offs_clean == 1, "one `--off` on the stop path (saw %d off(s): %s)",
                offs_clean, clean);

        scenario(4, -1, 0, 1, 0);
        ev_join(failed, sizeof failed);
        CHECK(tst_lines == lines_clean,
                "a STOP-PATH rail-off that did not take adds no supervisor line — "
                "the PSU HAL reports its own failure (saw %d line(s), clean %d)",
                tst_lines, lines_clean);
        CHECK(ev_count("rail-off") == offs_clean && !strcmp(clean, failed),
                "...and changes nothing else: the failing run's acts are identical "
                "to the clean run's — no retry, no second `--off`, nothing "
                "reordered (clean %s | failed %s)",
                clean, failed);

        scenario(6, 2, 0, 0, 0);
        scenario(6, 2, 0, 0, 0);
        ev_join(clean, sizeof clean);
        offs_clean  = ev_count("rail-off");
        lines_clean = tst_lines;
        CHECK(offs_clean == 2,
                "the thermal soft-off cuts the rail (2 `--off` with the stop "
                "path): %s",
                clean);

        scenario(6, 2, 0, 1, 0);
        ev_join(failed, sizeof failed);
        CHECK(tst_lines == lines_clean,
                "the thermal soft-off adds no supervisor line for a rail-off that did "
                "not take (%d line(s), clean %d, %d failed off(s))",
                tst_lines, lines_clean, ev_count("rail-off"));
        CHECK(ev_count("rail-off") > offs_clean && tst_duty_x10 == FAN_DUTY_MAX_PCT * 10,
                "unconfirmed thermal shutdown retries with maximum cooling: %s", failed);

        scenario(4, -1, -1, 0, 0);
        scenario(4, -1, -1, 0, 0);
        ev_join(clean, sizeof clean);
        offs_clean  = ev_count("rail-off");
        lines_clean = tst_lines;
        CHECK(g_softonoff.api_softoff && offs_clean == 2,
                "the API soft-off cuts the rail (api_softoff %d, 2 `--off` with the stop "
                "path): %s",
                g_softonoff.api_softoff, clean);

        scenario(4, -1, -1, 1, 0);
        ev_join(failed, sizeof failed);
        CHECK(tst_lines == lines_clean,
                "the API soft-off adds no supervisor line for a rail-off that did "
                "not take (%d line(s), clean %d)",
                tst_lines, lines_clean);
        CHECK(ev_count("rail-off") > offs_clean && tst_duty_x10 == FAN_DUTY_MAX_PCT * 10,
                "unconfirmed API shutdown retries with maximum cooling: %s", failed);

        tst_on_counts = 0;
        scenario(4, -1, 0, 0, 0);
        CHECK(tst_on_counts == VOL_ENUM,
                "...and on a real supervisor run the count that reaches the HAL "
                "is that same VOL_ENUM %d — the in-process call carries it, "
                "nothing re-parses it (saw %u)",
                VOL_ENUM, (unsigned)tst_on_counts);

        scenario(4, -1, 0, 0, 1);
        CHECK(tst_on_reports == 1,
                "a failed start-up rail enable latches the startup power error and "
                "says so once (%d)",
                tst_on_reports);
        CHECK(strstr(tst_on_line, "— power fault latched") != NULL,
                "the start-up power-on failure line names the board's outcome: %s", tst_on_line);
        g_startup_errors = ERR_MM_POWER | ERR_MM_FAN;
        tst_on_fails     = 1;
        CHECK(rail_power_on() != HAL_PSU_RAIL_OK && (g_startup_errors & ERR_MM_POWER),
                "failed power retry retains startup POWER error");
        tst_on_fails = 0;
        CHECK(rail_power_on() == HAL_PSU_RAIL_OK && g_startup_errors == ERR_MM_FAN,
                "successful power retry clears only startup POWER error");
        g_startup_errors |= ERR_MM_POWER;
        g_fault_off             = 1;
        g_softoff_rsn           = MC_SOFTOFF_POWER_FAULT;
        g_softonoff.api_softoff = 1;
        g_softonoff.start_wait  = 0;
        pid_t retry_asic        = -1;
        mm_api_act(softonoff_api_step(&g_softonoff, 1), 1, &retry_asic);
        mm_status_push();
        CHECK(!g_fault_off && retry_asic > 0 && !(g_live_ecmm & ERR_MM_POWER) &&
                        (g_live_ecmm & ERR_MM_FAN),
                "successful API retry clears live POWER while retaining unrelated errors");
        tst_off_recover_pass = 4;
        scenario(8, 2, 0, 1, 0);
        CHECK(!mm_rail_off_unconfirmed() && tst_duty_x10 == FAN_DUTY_SOFTOFF_PCT * 10,
                "confirmed disable recovery releases maximum cooling to thermal duty");
        CHECK(ev_count("rail-off") > 2 && ev_count("rail-off") < 8,
                "disable retries stop after confirmation");
        tst_off_recover_pass = -1;

        pid_t stopped_asic        = -1;
        g_softonoff.thermal_until = 0;
        g_softonoff.api_softoff   = 1;
        tst_off_fails             = 1;
        (void)rail_power_off();
        mm_thermal_act(SOFTONOFF_THERMAL_TO_SOFTOFF, &stopped_asic);
        CHECK(mm_rail_off_unconfirmed() && tst_duty_x10 == FAN_DUTY_MAX_PCT * 10,
                "thermal-to-API hold cannot lower cooling before disable is confirmed");

        g_fault_off     = 0;
        g_fan_fault_req = 1;
        mm_fan_fault_act(&stopped_asic);
        CHECK(g_softoff_rsn == MC_SOFTOFF_FAN_ERR && tst_duty_x10 == FAN_DUTY_MAX_PCT * 10,
                "fan-fault shutdown also keeps maximum cooling on unconfirmed disable");
        tst_off_fails        = 0;
        tst_fan_fault_at     = 2;
        tst_off_recover_pass = 4;
        scenario(8, -1, 0, 1, 0);
        CHECK(!mm_rail_off_unconfirmed() && g_softoff_rsn == MC_SOFTOFF_FAN_ERR &&
                        tst_duty_x10 == FAN_DUTY_SOFTOFF_PCT * 10,
                "confirmed disable after fan fault restores main's fan-fault cooling");
        tst_fan_fault_at     = -1;
        tst_off_recover_pass = -1;

        tst_ev_n              = 0;
        tst_reboot_in_prepare = 1;
        int rrc               = rail_power_on();
        tst_reboot_in_prepare = 0;
        CHECK(rrc == HAL_PSU_RAIL_E_REFUSED && ev_count("rail-on") == 0,
                "a reboot that starts during the power adapter negotiation keeps the "
                "rail off (rc %d, %d rail-on)",
                rrc, ev_count("rail-on"));
        atomic_store(&g_rebooting, 0);

        pid_t refused_asic    = -1;
        g_fault_off           = 0;
        g_softoff_rsn         = MC_SOFTOFF_NONE;
        tst_reboot_in_prepare = 1;
        mm_soft_start(&refused_asic, SOFTONOFF_API_ON);
        tst_reboot_in_prepare = 0;
        CHECK(!g_fault_off && g_softoff_rsn != MC_SOFTOFF_POWER_FAULT && refused_asic == -1,
                "a soft-on whose power-on a starting reboot refuses latches no power "
                "fault and starts no asic_miner (latch %d, reason %d)",
                g_fault_off, g_softoff_rsn);
        atomic_store(&g_rebooting, 0);

        tst_ev_n = 0;
        atomic_store(&g_rebooting, 1);
        int brc = rail_power_on();
        CHECK(brc == HAL_PSU_RAIL_E_REFUSED && ev_count("rail-on") == 0,
                "a power-on requested while a reboot is in progress is refused and never "
                "reaches the rail (rc %d, %d rail-on)",
                brc, ev_count("rail-on"));
        atomic_store(&g_rebooting, 0);
    }

    {
        tst_ev_n      = 0;
        tst_unknown_n = 0;
        tst_off_fails = 0;
        tst_on_fails  = 0;
        (void)rail_power_off();
        CHECK(tst_unknown_n == 1 && tst_unknown_offs == 1,
                "a rail-off forgets the V/F rail shadow after the rail is cut (%d call(s), "
                "%d rail-off(s) before it)",
                tst_unknown_n, tst_unknown_offs);
        (void)rail_power_off_for_reboot();
        CHECK(tst_unknown_n == 2 && tst_unknown_offs == 2,
                "...and so does the reboot rail-off (%d call(s), %d rail-off(s) before it)",
                tst_unknown_n, tst_unknown_offs);
        tst_ev_n      = 0;
        tst_unknown_n = 0;
        int orc       = rail_power_on();
        CHECK(orc == HAL_PSU_RAIL_OK && ev_count("rail-on") == 1 && tst_unknown_n == 1,
                "...and so does a power-on, which enables the rail at VOL_ENUM (rc %d, "
                "%d rail-on, %d call(s))",
                orc, ev_count("rail-on"), tst_unknown_n);
    }

    {
        fan_telem_reset();
        g_softonoff   = (softonoff_state_t){ 0 };
        tst_off_fails = 0;
        mm_fault_stop(MC_SOFTOFF_FAN_ERR, FAN_DUTY_SOFTOFF_PCT, -1);
        fan_rule(&g_fan_ctx, 1000);
        fan_rule(&g_fan_ctx, 1000 + TELEM_STALE_FAILSAFE_MS + 1000);
        CHECK(g_fault_off && tst_duty_x10 == FAN_DUTY_SOFTOFF_PCT * 10,
                "fan-fault soft-off keeps the board duty %d%% past the %u s telemetry "
                "fail-safe (duty %d%%)",
                FAN_DUTY_SOFTOFF_PCT, TELEM_STALE_FAILSAFE_MS / 1000u, tst_duty_x10 / 10);
        g_fault_off = 0;
    }

    return tst_done();
}
