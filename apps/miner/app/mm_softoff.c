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

#include "mm_internal.h"
#include "board_desc.h"
#include "fan_dev.h"
#include "fan_health.h"
#include "miner_ipc.h"
#include "mm_ctrl.h"
#include "mm_fan.h"
#include "mm_spawn.h"
#include "mm_telem.h"
#include "mm_vf.h"
#include "psu_ops.h"
#include "softonoff_sched.h"
#include "osif.h"

#include <pthread.h>
#include <time.h>

#define COVERAGE_LAP_BUDGET_MS 600000u /* per-hashboard share of the coverage settle time, ms */

/* temperature coverage settle time, ms */
#define COVERAGE_RIPEN_MS ((HBOARD_COUNT) * COVERAGE_LAP_BUDGET_MS)

static thermal_softoff_state_t  g_thermal_st;
static thermal_coverage_state_t g_thermal_cov;
static soft_sched_t             g_soft;
static pthread_mutex_t          g_soft_lock = PTHREAD_MUTEX_INITIALIZER;
static int                      g_fan_fault_req;
static int                      g_fan_fault_mask; /* stopped fans, bit per fan */
static int                      g_fan_fault_duty; /* fan duty at the fan fault, % */
static fan_health_state_t       g_fan_health_st;
static int               g_overheat_req; /* over-temperature softoff request, set by thermal_rule */
static _Atomic int       g_api_req;      /* soft on/off request: 1 on, -1 off, 0 none */
static const char *const g_softoff_why[] = {
    [MC_SOFTOFF_NONE]          = "none",
    [MC_SOFTOFF_POWER_FAULT]   = "power fault",
    [MC_SOFTOFF_HASH_FAULT]    = "hash board fault",
    [MC_SOFTOFF_ASIC_OVERHOT]  = "over-temperature",
    [MC_SOFTOFF_API]           = "API or schedule request",
    [MC_SOFTOFF_FAN_ERR]       = "fan fault",
    [MC_SOFTOFF_LAN_DAMAGED]   = "LAN damaged",
    [MC_SOFTOFF_LOW_HASHRATE]  = "low hashrate",
    [MC_SOFTOFF_HASHMCU_ERR]   = "hash board MCU error",
    [MC_SOFTOFF_HW_ERR]        = "hardware error",
    [MC_SOFTOFF_START_OVERHOT] = "over-temperature at start",
    [MC_SOFTOFF_CALI_FAILED]   = "aging failed",
};
static const char *const g_softon_why[] = {
    [SOFTONOFF_THERMAL_RESUME] = "over-temperature cool-down finished",
    [SOFTONOFF_API_ON]         = "API or schedule request",
};

int mm_asic_stopped(void)
{
    return (softonoff_children_stopped(&g_softonoff, g_fault_off) & SOFTONOFF_CHILD_ASIC) != 0;
}

void thermal_rule(void *ctx, uint64_t now_ms)
{
    thermal_params_t *pp = ctx;
    mm_telem_t        t;
    mm_telem_get(&t);

    switch (thermal_coverage_check(&g_thermal_cov, t.temp_valid_chips, t.asic_chips, t.tavg, now_ms,
            COVERAGE_RIPEN_MS)) {
    case THERMAL_COVERAGE_SHORT:
        osif_log(OSIF_LOG_WARN,
                "only %d of %d chips report a temperature "
                "(%d silent) — the over-temperature check uses "
                "the chips that report",
                t.temp_valid_chips, t.asic_chips, t.asic_chips - t.temp_valid_chips);
        break;
    case THERMAL_COVERAGE_FULL:
        osif_log(OSIF_LOG_INFO, "all %d chips report a temperature again", t.asic_chips);
        break;
    default:
        break;
    }

    if (thermal_softoff_check(pp, &g_thermal_st, t.tmax, t.poll_fresh ? t.hb_outlet : NULL,
                t.poll_fresh ? t.hb_count : 0, t.in_work))
        g_overheat_req = 1;
}

softonoff_action mm_overheat_step(uint64_t now_ms)
{
    softonoff_action pa =
            softonoff_thermal_step(&g_softonoff, g_overheat_req, now_ms, OVERHOT_HOLD_MS);
    if (pa != SOFTONOFF_NONE)
        g_overheat_req = 0;
    return pa;
}

static mc_set_result mm_soft_now_refused(int is_on)
{
    uint64_t until = g_softonoff.thermal_until;
    uint64_t now   = osif_now_ms();

    switch (softonoff_api_check(&g_softonoff, g_fault_off, is_on)) {
    case SOFTONOFF_API_IGNORED:
        mm_ctrl_refuse(MC_CTRL_WHY_COOLDOWN,
                until > now ? (int64_t)((until - now + 999u) / 1000u) : 0);
        osif_log(OSIF_LOG_OP,
                "API soft-on refused — over-temperature "
                "cool-down in progress");
        return MC_SET_ERR_STATE;
    case SOFTONOFF_API_FAULT_HELD:
        mm_ctrl_refuse(MC_CTRL_WHY_FAULT, g_softoff_rsn);
        osif_log(OSIF_LOG_OP,
                "API soft-on refused — mining stopped by %s; "
                "a soft-off then soft-on releases it",
                g_softoff_why[g_softoff_rsn]);
        return MC_SET_ERR_STATE;
    default:
        return MC_SET_OK;
    }
}

mc_set_result mm_apply_soft(uint16_t op, const mc_ctrl_cmd *c)
{
    int     is_on = op == CTRL_OP_SOFTON;
    int64_t now_s;

    if (c->v.soft.immediate) {
        mc_set_result rc = mm_soft_now_refused(is_on);

        if (rc != MC_SET_OK)
            return rc;
        g_api_req = is_on ? 1 : -1;
        osif_log(OSIF_LOG_INFO, "soft-%s requested by API, applying now", is_on ? "on" : "off");
        return MC_SET_OK;
    }

    if (c->v.soft.valid > 1) {
        osif_log(OSIF_LOG_WARN, "scheduled %s rejected: valid %u is not 0 or 1", ctrl_cmd_name(op),
                (unsigned)c->v.soft.valid);
        return MC_SET_ERR_ARG;
    }

    now_s = (int64_t)time(NULL);
    if (c->v.soft.valid && c->v.soft.ts < now_s - SOFT_SCHED_WINDOW_S) {
        mm_ctrl_refuse(MC_CTRL_WHY_PAST, now_s);
        osif_log(OSIF_LOG_WARN, "scheduled %s rejected: time %lld is before now (%lld)",
                ctrl_cmd_name(op), (long long)c->v.soft.ts, (long long)now_s);
        return MC_SET_ERR_ARG;
    }

    pthread_mutex_lock(&g_soft_lock);
    soft_sched_arm(&g_soft, is_on, c->v.soft.valid, c->v.soft.ts);
    pthread_mutex_unlock(&g_soft_lock);
    osif_log(OSIF_LOG_INFO, "scheduled %s %s (time %lld)", ctrl_cmd_name(op),
            c->v.soft.valid ? "set" : "cleared", (long long)c->v.soft.ts);
    return MC_SET_OK;
}

void soft_sched_rule(void *ctx, uint64_t now_ms)
{
    (void)ctx;
    (void)now_ms;

    pthread_mutex_lock(&g_soft_lock);
    soft_sched_action a = soft_sched_due(&g_soft, (int64_t)time(NULL), !g_softonoff.api_softoff);
    pthread_mutex_unlock(&g_soft_lock);

    if (a == SOFT_SCHED_ON && g_softonoff.thermal_until) {
        osif_log(OSIF_LOG_OP,
                "scheduled soft-on skipped — over-temperature "
                "cool-down in progress");
        return;
    }

    if (a != SOFT_SCHED_NONE)
        osif_log(OSIF_LOG_INFO, "scheduled soft-%s due — applying now",
                a == SOFT_SCHED_ON ? "on" : "off");
    if (a == SOFT_SCHED_ON)
        g_api_req = 1;
    else if (a == SOFT_SCHED_OFF)
        g_api_req = -1;
}

int mm_api_req_take(void)
{
    return atomic_exchange(&g_api_req, 0);
}

void mm_rail_off_fan(int duty)
{
    int rc = rail_power_off();
    mm_fan_set_fixed(mm_rail_off_duty(rc, duty));
}

void mm_fault_stop(uint8_t rsn, int duty, pid_t asic)
{
    g_softoff_rsn = rsn;
    g_fault_off   = 1;
    mm_asic_term(asic);
    mm_rail_off_fan(duty);
    osif_log(OSIF_LOG_OP, "mining stopped (soft-off, reason: %s)", g_softoff_why[rsn]);
}

static int mm_softoff_fan_duty(int duty)
{
    return mm_rail_off_unconfirmed() ? FAN_DUTY_MAX_PCT : duty;
}

static void mm_soft_stop(pid_t *asic, int duty, uint8_t rsn)
{
    mm_asic_term(*asic);
    mm_rail_off_fan(duty);
    osif_log(OSIF_LOG_OP, "mining stopped (soft-off, reason: %s)", g_softoff_why[rsn]);
}

static void mm_soft_start(pid_t *asic, softonoff_action pa)
{
    if (atomic_load(&g_rebooting))
        return;
    g_asic_exec_fails = 0;
    mm_fan_pid_fixed(-1);
    int rc = rail_power_on();
    if (rc == HAL_PSU_RAIL_E_REFUSED)
        return;
    if (rc != HAL_PSU_RAIL_OK && mm_rail_on_fault_latch()) {
        osif_log(OSIF_LOG_WARN,
                "POWER FAULT: hashboard power-on failed (%s, "
                "code %d) — mining stays stopped",
                hal_psu_rail_strerror(rc), rc);
        g_softonoff.api_softoff = 1;
        mm_fan_set_fixed(mm_softoff_fan_duty(0));
        osif_log(OSIF_LOG_OP, "mining stopped (soft-off, reason: %s)",
                g_softoff_why[MC_SOFTOFF_POWER_FAULT]);
        return;
    }
    if (*asic <= 0 && !mm_asic_stopped())
        *asic = spawn_asic();
    osif_log(OSIF_LOG_OP, "mining restarted (soft-on, reason: %s)", g_softon_why[pa]);
}

void mm_thermal_act(softonoff_action pa, pid_t *asic)
{
    switch (pa) {
    case SOFTONOFF_THERMAL_OFF:
        if (!g_fault_off)
            g_softoff_rsn = MC_SOFTOFF_ASIC_OVERHOT;
        osif_log(OSIF_LOG_OP,
                "OVER HOT: chips above %d C%s — mining stopped "
                "for %u s to cool down",
                g_thermal.over_temp,
                g_thermal.outlet_over_count > 0 ? " or hashboard outlet above its limit" : "",
                OVERHOT_HOLD_MS / 1000u);
        mm_soft_stop(asic, FAN_DUTY_SOFTOFF_PCT, MC_SOFTOFF_ASIC_OVERHOT);
        return;
    case SOFTONOFF_THERMAL_TO_SOFTOFF:
        if (g_softoff_rsn == MC_SOFTOFF_ASIC_OVERHOT)
            g_softoff_rsn = MC_SOFTOFF_API;
        osif_log(OSIF_LOG_OP,
                "cool-down finished — mining stays stopped "
                "because of the API soft-off");
        mm_fan_set_fixed(mm_softoff_fan_duty(FAN_DUTY_API_SOFTOFF_PCT));
        return;
    case SOFTONOFF_THERMAL_RESUME:
        if (g_fault_off) {
            osif_log(OSIF_LOG_OP,
                    "cool-down finished — mining stays stopped "
                    "because a fault is still latched");
            return;
        }
        if (g_softoff_rsn == MC_SOFTOFF_ASIC_OVERHOT)
            g_softoff_rsn = MC_SOFTOFF_NONE;
        osif_log(OSIF_LOG_OP, "cool-down finished — mining restarts");
        mm_soft_start(asic, pa);
        return;
    default:
        return;
    }
}

void mm_reboot_failed_act(void)
{
    if (!atomic_exchange(&g_reboot_failed, 0))
        return;
    osif_log(OSIF_LOG_OP, "reboot failed — mining stays stopped");
}

void mm_enum_act(enum_hold_action ea)
{
    if (ea == ENUM_HOLD_PIN) {
        osif_log(OSIF_LOG_INFO,
                "hashboards starting — fan held at %d%% "
                "until the chips report",
                FAN_DUTY_ENUM_PCT);
        mm_fan_set_fixed(FAN_DUTY_ENUM_PCT);
    } else if (ea == ENUM_HOLD_RELEASE) {
        osif_log(OSIF_LOG_INFO,
                "chips reporting — fan back to "
                "automatic control");
        mm_fan_pid_fixed(-1);
    }
}

void mm_api_act(softonoff_action ap, int want_on, pid_t *asic)
{
    switch (ap) {
    case SOFTONOFF_API_IGNORED:
        osif_log(OSIF_LOG_OP,
                "API soft-%s ignored — over-temperature "
                "cool-down in progress",
                want_on ? "on" : "off");
        return;
    case SOFTONOFF_API_OFF_HELD:
        osif_log(OSIF_LOG_OP,
                "soft-off by API recorded — mining stays stopped "
                "after the over-temperature cool-down");
        return;
    case SOFTONOFF_API_OFF:
        osif_log(OSIF_LOG_OP,
                "soft-off by API — mining stopped, hashboard "
                "power off; pool connection and API stay up");
        if (!g_fault_off)
            g_softoff_rsn = MC_SOFTOFF_API;
        mm_soft_stop(asic, FAN_DUTY_API_SOFTOFF_PCT, MC_SOFTOFF_API);
        return;
    case SOFTONOFF_API_ON:
        g_fault_off   = 0;
        g_softoff_rsn = MC_SOFTOFF_NONE;
        mm_vf_aging_rearm();
        osif_log(OSIF_LOG_OP, "soft-on by API — mining %s",
                atomic_load(&g_rebooting) ? "stays stopped" : "restarts");
        mm_soft_start(asic, ap);
        return;
    default:
        return;
    }
}

void mm_aging_fail_act(pid_t *asic)
{
    if (!mm_vf_take_aging_failed() || g_fault_off)
        return;

    osif_log(OSIF_LOG_OP,
            "AGING FAILED: no usable frequency found — mining "
            "stopped, hashboard power off");
    mm_fault_stop(MC_SOFTOFF_CALI_FAILED, FAN_DUTY_MIN_PCT, *asic);
}

void fan_health_rule(void *ctx, uint64_t now_ms)
{
    fan_health_params_t *pp = ctx;
    (void)now_ms;

    if (pp->err_count == 0)
        return;
    if (!atomic_load(&g_fan_dev_ok) || g_fault_off)
        return;

    int rpm[FAN_COUNT];
    int n = (int)g_board.fan_count;
    if (n > FAN_COUNT)
        n = FAN_COUNT;
    for (int f = 0; f < n; f++)
        rpm[f] = fan_dev_get_rpm(f);

    int duty = (fan_dev_last_duty_x10() + 9) / 10;

    mm_telem_t t;
    mm_telem_get(&t);

    fan_health_verdict v =
            fan_health_check(pp, &g_fan_health_st, rpm, n, duty, t.poll_fresh && t.in_work);
    if (v == FAN_HEALTH_TRIP) {
        g_fan_fault_mask = fan_health_stopped_mask(rpm, n);
        g_fan_fault_duty = duty;
        g_fan_fault_req  = 1;
    }
}

void mm_fan_fault_act(pid_t *asic)
{
    if (!g_fan_fault_req)
        return;
    g_fan_fault_req = 0;
    if (g_fault_off)
        return;

    osif_log(OSIF_LOG_OP,
            "FAN FAULT: fan mask 0x%x read below %d RPM on %d "
            "samples in a row at %d%% duty — mining stopped, hashboard power "
            "off",
            (unsigned)g_fan_fault_mask, FAN_STALL_RPM, FAN_BELOW_STALL_CNT, g_fan_fault_duty);
    mm_fault_stop(MC_SOFTOFF_FAN_ERR, FAN_DUTY_SOFTOFF_PCT, *asic);
}
