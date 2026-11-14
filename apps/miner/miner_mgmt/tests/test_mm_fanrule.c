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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "boardconf.h"
#include "fan_dev.h"
#include "mm_telem.h"
#include "net_linux_paths.h"
#include "../../tests/include/tst_check.h"

#undef MM_SYS_NET_DIR
#define MM_SYS_NET_DIR "/nonexistent/"

#define TEST_HB_COUNT   (CAP_HB_MCU ? HBOARD_COUNT : 0) /* hb_count reported in telemetry */
#define TEST_OUTLET_ROW 3                               /* outlet temperatures in a telem() row */

static pid_t tst_fork(void);
static int   tst_kill(pid_t pid, int sig);
static pid_t tst_waitpid(pid_t pid, int *st, int opt);
#if NET_WIRED_ALT
static int tst_system(const char *cmd);
#endif /* NET_WIRED_ALT */

/* Process calls in the included sources, routed to the failing fakes below */
#define fork()           tst_fork()
#define kill(p, s)       tst_kill((p), (s))
#define waitpid(p, s, o) tst_waitpid((p), (s), (o))
#if NET_WIRED_ALT
#define system(c) tst_system((c))
#endif /* NET_WIRED_ALT */

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
#include "mm_logrot.c"
#include "mm_miner_tu.h"
#undef system
#undef waitpid
#undef kill
#undef fork

static mm_telem_t tst_tel;          /* snapshot mm_telem_get() returns */
static int        tst_duty_x10;     /* last commanded fan duty, 0.1 %; -1 none */
static int        tst_duty_n;       /* fan_dev_set_duty_x10() call count */
static int        tst_init_rc = -1; /* fan_dev_init() result */
static int        tst_init_n;       /* fan_dev_init() call count */
static int        tst_proc_calls;   /* fork/kill/waitpid/system call count */
/* "fan controller still unavailable" and "available again" log lines */
static int tst_warn_n, tst_resumed_n;
static int cmd_mark, probe_mark; /* tst_duty_n and tst_init_n at the last read */

void mm_telem_get(mm_telem_t *out)
{
    *out = tst_tel;
}

int fan_dev_init(void)
{
    tst_init_n++;
    return tst_init_rc;
}
void fan_dev_set_duty_x10(int duty_x10)
{
    tst_duty_x10 = duty_x10;
    tst_duty_n++;
}
int fan_dev_last_duty_x10(void)
{
    return tst_duty_x10;
}
int fan_dev_get_duty_pct(void)
{
    return tst_duty_x10 / 10;
}

static pid_t tst_fork(void)
{
    tst_proc_calls++;
    return -1;
}
static int tst_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    tst_proc_calls++;
    return -1;
}
static pid_t tst_waitpid(pid_t pid, int *st, int opt)
{
    (void)pid;
    (void)st;
    (void)opt;
    tst_proc_calls++;
    return -1;
}
#if NET_WIRED_ALT
static int tst_system(const char *cmd)
{
    (void)cmd;
    tst_proc_calls++;
    return -1;
}
#endif /* NET_WIRED_ALT */

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    char    b[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (lvl == OSIF_LOG_WARN && strstr(b, "fan controller still unavailable"))
        tst_warn_n++;
    if (lvl == OSIF_LOG_INFO && strstr(b, "fan controller available again"))
        tst_resumed_n++;
}

static int cmds(void)
{
    int d    = tst_duty_n - cmd_mark;
    cmd_mark = tst_duty_n;
    return d;
}
static int probes(void)
{
    int d      = tst_init_n - probe_mark;
    probe_mark = tst_init_n;
    return d;
}

static void tick(uint64_t now_ms)
{
    fan_rule(&g_fan_ctx, now_ms);
}

static void telem(int fresh, int asic_live, int tavg, int tmax, const int o[TEST_OUTLET_ROW])
{
    memset(&tst_tel, 0, sizeof tst_tel);
    tst_tel.poll_fresh = fresh;
    tst_tel.asic_live  = asic_live;
    tst_tel.tavg       = tavg;
    tst_tel.tmax       = tmax;
    tst_tel.hb_count   = TEST_HB_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++) {
        int v                = (o && b < TEST_OUTLET_ROW) ? o[b] : ASIC_TEMP_NONE;
        tst_tel.hb_outlet[b] = tst_tel.hb_inlet[b] = v;
    }
}

static void arm_pid(void)
{
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty   = -1;
    g_fan_st.instart_done = 1;
    tst_duty_x10          = -1;
}

int main(void)
{
    int d;

    printf("descriptor %s: %d board(s), hb_count %d, PID band %d..%d, "
           "duties init %d / max %d / api-softoff %d / enum %d\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, TEST_HB_COUNT, FAN_PID_TEMP_MIN, FAN_PID_TEMP_MAX,
            FAN_DUTY_INIT_PCT, FAN_DUTY_MAX_PCT, FAN_DUTY_API_SOFTOFF_PCT, FAN_DUTY_ENUM_PCT);

    telem(0, 0, ASIC_TEMP_NONE, ASIC_TEMP_NONE, NULL);

    tick(0);
    CHECK(probes() == 0 && cmds() == 0,
            "no fan pwm: the first tick neither re-probes nor commands");

    tick(60000);
    d = probes();
    CHECK(d == 1 && cmds() == 0,
            "...it re-probes at ~60 s (%d probe(s)), and a still-dead pwm commands "
            "nothing",
            d);
    CHECK(tst_warn_n == 1, "...the first failed re-probe logs the unavailable warning (%d)",
            tst_warn_n);

    tick(150001);
    d = probes();
    CHECK(d == 1 && cmds() == 0,
            "...and a dead pwm still commands nothing %u s in, where a live one "
            "would already be at fail-safe (%d probe(s))",
            TELEM_STALE_FAILSAFE_MS / 1000u, d);
    CHECK(tst_warn_n == 2, "...a later failed re-probe logs the warning again (%d)", tst_warn_n);

    tst_init_rc = 0;
    tick(220000);
    CHECK(probes() == 1 && atomic_load(&g_fan_dev_ok),
            "a pwm that comes back on a retry is adopted (no restart needed)");
    CHECK(tst_resumed_n == 1 && tst_warn_n == 2,
            "recovery logs the resumed line once (%d) and no further warning (%d)", tst_resumed_n,
            tst_warn_n);
    CHECK(cmds() == 0,
            "no usable reading inside the grace HOLDs: an absent temperature must "
            "never be allowed to LOWER the fan");

    tick(220000 + TELEM_STALE_FAILSAFE_MS + 1);
    d = cmds();
    CHECK(d == 1 && tst_duty_x10 == FAN_DUTY_MAX_PCT * 10,
            "unusable past the %u s grace commands FAN_DUTY_MAX_PCT (%d%%), because "
            "the thermal soft-off goes telem-blind at the same moment (%d command(s), %d x10)",
            TELEM_STALE_FAILSAFE_MS / 1000u, FAN_DUTY_MAX_PCT, d, tst_duty_x10);

    softonoff_api_step(&g_softonoff, 0);
    fan_pid_set_fixed(&g_fan_st, FAN_DUTY_API_SOFTOFF_PCT);
    tick(400000);
    CHECK(cmds() == 0,
            "a soft-off miner commands nothing even with the telemetry long gone — "
            "the %d%% the soft-off pinned stands",
            FAN_DUTY_API_SOFTOFF_PCT);

    softonoff_api_step(&g_softonoff, 1);
    tick(400001);
    CHECK(cmds() == 0,
            "...and the soft-off RE-ARMED the grace, so the tick after the release "
            "HOLDs instead of firing the fail-safe on a stamp %u s old",
            TELEM_STALE_FAILSAFE_MS / 1000u);

    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    arm_pid();
    telem(1, 1, 30, 101, (const int[TEST_OUTLET_ROW]){ 60, 60, 60 });
    tick(410000);
    d = cmds();
    if (TEST_HB_COUNT > 0) {
        CHECK(d == 1 && tst_duty_x10 == FAN_DUTY_MAX_PCT * 10,
                "TMax 101 is the input, not TAvg 30: above the band, so %d%% (%d "
                "command(s), %d x10) — a TAvg-first order would command %d%% here",
                FAN_DUTY_MAX_PCT, d, tst_duty_x10, FAN_DUTY_INIT_PCT);
    } else {
        CHECK(d == 1 && tst_duty_x10 == FAN_DUTY_INIT_PCT * 10,
                "no outlet row: TAvg 30 is the input and TMax 101 is IGNORED, so the "
                "below-band %d%% (%d command(s), %d x10) — the n<=0 arm keeps the TMax "
                "preference off this board, which is what \"nano3s is untouched\" means",
                FAN_DUTY_INIT_PCT, d, tst_duty_x10);
    }

    arm_pid();
    telem(1, 1, ASIC_TEMP_NONE, ASIC_TEMP_NONE, (const int[TEST_OUTLET_ROW]){ 101, 32, 31 });
    tick(412000);
    d = cmds();
    if (TEST_HB_COUNT > 0) {
        CHECK(d == 1 && tst_duty_x10 == FAN_DUTY_MAX_PCT * 10,
                "with no chip reading the OUTLET row is the input (hottest 101 -> "
                "%d%%), so the row and hb_count %d both reach the picker (%d command(s), "
                "%d x10)",
                FAN_DUTY_MAX_PCT, TEST_HB_COUNT, d, tst_duty_x10);
    } else {
        CHECK(d == 0,
                "with no chip reading and no outlet row there is no input at all: the "
                "rule HOLDs rather than inventing one");
    }

    if (TEST_HB_COUNT > 0) {
        arm_pid();
        telem(1, 1, ASIC_TEMP_NONE, 30, (const int[TEST_OUTLET_ROW]){ 101, 32, 31 });
        tick(414000);
        d = cmds();
        CHECK(d == 1 && tst_duty_x10 == FAN_DUTY_INIT_PCT * 10,
                "a usable chip max 30 beats a hotter outlet 101 (below-band %d%%, %d "
                "command(s), %d x10) — and the PID ran at all, so the validity gate "
                "reads the PICKED value, not t.tavg, which is the sentinel here",
                FAN_DUTY_INIT_PCT, d, tst_duty_x10);
    }

    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    arm_pid();
    enum_hold_rearm(&g_enum_hold);
    enum_hold_step(&g_enum_hold, 0, FAN_DUTY_ENUM_PCT);
    telem(0, 0, ASIC_TEMP_NONE, ASIC_TEMP_NONE, NULL);
    tick(416000);
    d = cmds();
    if (FAN_DUTY_ENUM_PCT > 0) {
        CHECK(d == 1 && tst_duty_x10 == FAN_DUTY_ENUM_PCT * 10,
                "enumerating with no telemetry commands FAN_DUTY_ENUM_PCT (%d%%) from "
                "the first tick, not %u s later when the fail-safe escalates (%d "
                "command(s), %d x10)",
                FAN_DUTY_ENUM_PCT, TELEM_STALE_FAILSAFE_MS / 1000u, d, tst_duty_x10);

        g_fan_st.fixed_duty = 40;
        tick(418000);
        d = cmds();
        CHECK(d == 1 && tst_duty_x10 == 400,
                "...and a user fixed duty REPLACES the pin for the window "
                "(%d command(s), %d x10)",
                d, tst_duty_x10);
    } else {
        CHECK(d == 0,
                "no enum pin on this product (FAN_DUTY_ENUM_PCT 0): an unusable "
                "reading inside the grace still just HOLDs");
    }

    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    arm_pid();
    telem(1, 1, 60, 60, (const int[TEST_OUTLET_ROW]){ 60, 60, 60 });
    tick(500000);
    cmds();

    softonoff_action pa = softonoff_thermal_step(&g_softonoff, 1, 500000, OVERHOT_HOLD_MS);
    CHECK(pa == SOFTONOFF_THERMAL_OFF && g_softonoff.thermal_until != 0,
            "the thermal soft-off trips and the hold is armed (verdict %d)", (int)pa);
    fan_pid_set_fixed(&g_fan_st, FAN_DUTY_SOFTOFF_PCT);
    telem(0, 0, ASIC_TEMP_NONE, ASIC_TEMP_NONE, NULL);

    tick(500000 + TELEM_STALE_FAILSAFE_MS + 1);
    CHECK(cmds() == 0,
            "an over-hot board commands nothing %u s into the hold, with the "
            "telemetry gone because the thermal soft-off stopped the child itself — the %d%% the "
            "trip pinned stands. Before this the fail-safe commanded "
            "FAN_DUTY_MAX_PCT (%d%%) here, overriding the safety pin for the rest "
            "of the %u ms hold",
            TELEM_STALE_FAILSAFE_MS / 1000u, FAN_DUTY_SOFTOFF_PCT, FAN_DUTY_MAX_PCT,
            OVERHOT_HOLD_MS);

    tick(500000 + OVERHOT_HOLD_MS - 1000);
    CHECK(cmds() == 0, "...and still nothing on the last tick before the hold expires");

    pa = softonoff_thermal_step(&g_softonoff, 0, 500000 + OVERHOT_HOLD_MS, OVERHOT_HOLD_MS);
    CHECK(pa == SOFTONOFF_THERMAL_RESUME && g_softonoff.thermal_until == 0,
            "the hold expires with no soft-off underneath (verdict %d)", (int)pa);
    tick(500000 + OVERHOT_HOLD_MS + 1);
    CHECK(cmds() == 0,
            "...and the hold RE-ARMED the grace, so the tick after the resume "
            "HOLDs instead of firing the fail-safe on a stamp %u s old — same "
            "property as after an API soft-on",
            TELEM_STALE_FAILSAFE_MS / 1000u);

    g_fan_target_temp   = FAN_TARGET_TEMP;
    g_fan_target_manual = 0;
    mm_fan_target_set_manual(FAN_PID_TEMP_MAX);
    CHECK(g_fan_target_temp == FAN_PID_TEMP_MAX && g_fan_target_manual == 1,
            "mm_fan_target_set_manual(%d) writes the target and arms the manual latch "
            "(target %u, latch %d)",
            FAN_PID_TEMP_MAX, g_fan_target_temp, g_fan_target_manual);
    mm_fan_target_set_manual(FAN_PID_TEMP_MIN);
    CHECK(g_fan_target_temp == FAN_PID_TEMP_MIN && g_fan_target_manual == 1,
            "...a second call replaces the target and keeps the latch (target %u, latch %d)",
            g_fan_target_temp, g_fan_target_manual);

    CHECK(tst_proc_calls == 0,
            "the fan rule forks, spawns, signals and reaps nothing "
            "(%d process call(s))",
            tst_proc_calls);

    return tst_done();
}
