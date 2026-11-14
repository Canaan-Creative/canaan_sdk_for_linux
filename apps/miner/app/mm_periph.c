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
#include "cali.h"
#include "eeprom_dev.h"
#include "fan_dev.h"
#include "mc_api_bootby.h"
#include "mc_errcode.h"
#include "miner_ipc.h"
#include "mm_cfgstore.h"
#include "mm_cfgwr.h"
#include "mm_fan.h"
#include "mm_led.h"
#include "mm_net.h"
#include "mm_telem.h"
#include "mm_tz.h"
#include "mm_vf.h"
#include "net_state.h"
#include "otp_dev.h"
#include "process_version.h"
#include "osif.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SUPPORT_BLE
#include "wifi_mgr.h"
#endif /* SUPPORT_BLE */
#include "mm_display.h"
#ifdef SUPPORT_LCD
#include "ui_screen.h"
#include "ui_types.h"
#endif /* SUPPORT_LCD */

/* pool job gap that sets ERR_MM_POOL_INACTIVE, and wait with an IP for a first job, s */
#define POOL_INACTIVE_S 120u

/* pool job gap that reboots, s */
#define POOL_REBOOT_S 360u

static _Atomic uint32_t g_live_ecmm; /* current errors, ERR_MM_* bit mask */
static _Atomic int      g_pool_inactive;
static _Atomic int g_pool_unreached;  /* configured pool, IP, no first job within POOL_INACTIVE_S */
static int         g_no_job_waiting;  /* IP up, waiting for a first pool job */
static uint64_t    g_no_job_since_ms; /* when that wait started, ms */
static char        g_hash_sn[HBOARD_COUNT][HASH_SN_LEN + 1];
static pthread_mutex_t g_hash_sn_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         g_dna[MM_DNA_LEN]; /* device ID */

static pthread_mutex_t g_filtermesh_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_filtermesh_time; /* filter run time, minutes */

void mm_hash_sn_set(unsigned hash, const char *sn)
{
    pthread_mutex_lock(&g_hash_sn_lock);
    memcpy(g_hash_sn[hash], sn, HASH_SN_LEN);
    pthread_mutex_unlock(&g_hash_sn_lock);
}

void mm_hash_sn_load(void)
{
    if (eeprom_init() != 0) {
        osif_log(OSIF_LOG_WARN,
                "hashboard serial numbers could not be read — "
                "left blank");
        return;
    }
    osif_sleep_ms(10);
    for (int i = 0; i < HBOARD_COUNT; i++) {
        char sn[HASH_SN_LEN];
        if (eeprom_read(SN_OFST, (uint8_t *)sn, HASH_SN_LEN, (uint8_t)i) != 0) {
            osif_log(OSIF_LOG_WARN, "hashboard %d serial number could not be read", i);
        } else {
            pthread_mutex_lock(&g_hash_sn_lock);
            memcpy(g_hash_sn[i], sn, HASH_SN_LEN);
            pthread_mutex_unlock(&g_hash_sn_lock);
        }
        osif_sleep_ms(10);
    }
}

void mm_dna_load(void)
{
    int rc = mm_dna_read(g_dna, sizeof g_dna);

    if (rc == -ENOENT) {
        osif_log(OSIF_LOG_WARN, "Device ID not readable (open dna device error)");
    } else if (rc != 0) {
        osif_log(OSIF_LOG_WARN, "Device ID not readable (read dna error)");
        memset(g_dna, 0, sizeof g_dna);
    }
}

static int mm_asic_idle_by_design(void)
{
    return (softonoff_children_stopped(&g_softonoff, mm_power_fault_stop()) &
                   SOFTONOFF_CHILD_ASIC) != 0;
}

void mm_periph_push(void)
{
    mm_periph    p = { .valid = 1 };
    mm_led_cfg_t led;
    mm_led_snapshot(&led, &p.runeffect);
    p.ledmode    = led.ledmode;
    p.led_effect = led.effect;
    p.led_bright = led.bright;
    p.led_temper = led.temper;
    p.led_rgb    = led.rgb;
    int fp       = fan_dev_get_duty_pct();
    p.fan_pct    = (uint8_t)(fp > 0 ? fp : 0);
    for (size_t f = 0; f < g_board.fan_count; f++) {
        int rpm      = fan_dev_get_rpm((int)f);
        p.fan_rpm[f] = (uint16_t)(rpm > 0 ? rpm : 0);
    }
    mm_telem_t tel;
    mm_telem_get(&tel);
    if (tel.poll_fresh) {
        p.hb_count = (uint8_t)tel.hb_count;
        for (int b = 0; b < tel.hb_count; b++) {
            p.hb_outlet[b] = (int16_t)tel.hb_outlet[b];
            p.hb_inlet[b]  = (int16_t)tel.hb_inlet[b];
        }
    }
    int asic_idle_by_design = mm_asic_idle_by_design();
    int hash_fault_latched  = (g_fault_off && g_softoff_rsn == MC_SOFTOFF_HASH_FAULT);
    for (size_t b = 0; b < g_board.hboard_count; b++) {
        int enum_failed =
                (tel.asic_live && (int)b < tel.asic_boards && tel.asic_chips_board[b] <= 0) ||
                hash_fault_latched;
        p.hu_errcode[b] = enum_failed ? ERR_HU_ENUM : 0u;
    }
    int asic_link = osif_ipc_connected(g_ipc_asic_mm);
    p.mm_errcode  = (asic_idle_by_design || asic_link) ? 0u : ERR_MM_INTERNUCLEAR;

    mm_vf_snapshot(&p);
    {
        uint8_t    mode = mm_workmode_running();
        cali_param cp;
        uint8_t    aged = 0;
        cali_resolve(CALI_INI_PATH, g_board.max_asic, mode, 0, &cp, &aged);
        p.cali_max_pout     = cp.max_pout;
        p.cali_temp         = cp.temp;
        p.cali_volt         = cp.volt;
        p.cali_pll_start    = cp.pll_start;
        p.cali_pll_interval = cp.pll_interval;
        p.cali_aged         = aged;
        p.workmode          = mode;
        p.maxmode           = mm_workmode_max();

        unsigned caliall = 0;
        for (uint8_t m = 0; m < CALI_MODE_MAX; m++) {
            char       raw[CALI_STR_MAX];
            cali_param mcp;
            if ((CALI_MODE_BOARD_MASK & (1u << m)) &&
                    cali_get(CALI_INI_PATH, m, 0, raw, sizeof raw) == 0 &&
                    cali_parse(raw, &mcp) == 0)
                caliall |= 1u << m;
        }
        if (p.aging_finished)
            caliall |= 1u << mode;
        p.caliall = (uint8_t)caliall;
    }
    p.softoff_rsn = g_softoff_rsn;
    pthread_mutex_lock(&g_hash_sn_lock);
    memcpy(p.hash_sn, g_hash_sn, sizeof p.hash_sn);
    pthread_mutex_unlock(&g_hash_sn_lock);
    memcpy(p.dna, g_dna, sizeof p.dna);
    mm_cfgwr_webpass_get(p.webpass, sizeof p.webpass);
    mm_cfgwr_hwcfg_get(p.prod, sizeof p.prod, p.model, sizeof p.model);
    p.activation = (uint8_t)mm_cfgwr_activation_get();
    mm_tz_get(p.tz, sizeof p.tz);
#ifdef SUPPORT_BLE
    {
        char ssid[WIFI_SSID_LEN_MAX] = { 0 };
        int  rssi                    = 0;
        wifi_update_ssid_info(ssid, &rssi);
        snprintf(p.ssid, sizeof p.ssid, "%s", ssid);
        p.rssi        = (int16_t)rssi;
        p.net_devtype = mm_net_devtype();
    }
#endif /* SUPPORT_BLE */
    p.filtermesh = filtermesh_minutes();
    mm_display_state display;
    mm_display_snapshot(&display);
    p.lcd_onoff     = display.onoff;
    p.lcd_show      = display.show;
    p.lcd_bright    = (uint8_t)display.bright;
    p.lcd_bright_hi = (uint8_t)(display.bright >> 8);
    p.volume        = display.volume;

    p.mm_errcode |= atomic_load(&g_live_ecmm);
    for (int board = 0; board < HBOARD_COUNT; board++)
        if (p.hu_errcode[board])
            p.mm_errcode |= ERR_MM_SLAVE;

    osif_ipc_send(g_ipc_core_mm, MSG_MM_PERIPH, &p, (int)sizeof p);
    static const char version[] = MM_MINER_VERSION;
    osif_ipc_send(g_ipc_core_mm, MSG_MM_VERSION, version, sizeof version);
    mc_mm_bootby bb = { g_bootby_reason, g_bootby_sub };
    osif_ipc_send(g_ipc_core_mm, MSG_MM_BOOTBY, &bb, (int)sizeof bb);
}

static int fault_stopped(uint8_t rsn)
{
    return g_fault_off && g_softoff_rsn == rsn;
}

#ifdef SUPPORT_LCD
static void ui_latch(int *shown, int active, uint16_t id)
{
    if (active == *shown)
        return;
    ui_show_update(active ? UI_ACT_ADD : UI_ACT_DELETE, id);
    *shown = active;
}

static void status_popups(uint32_t errors)
{
    static int overhot_shown, fan_shown, power_shown, filter_shown;
    static int hash_shown, stall_shown, aging_shown, power_fault_shown, pool_shown;
    ui_latch(&overhot_shown, g_softonoff.thermal_until != 0, UI_OVERHOT);
    ui_latch(&fan_shown, (g_startup_errors & ERR_MM_FAN) || !atomic_load(&g_fan_dev_ok),
            UI_FAN_ERR);
    ui_latch(&power_shown, g_power_notice, UI_POWER_ERR);
    ui_latch(&filter_shown, filtermesh_minutes() >= FILTERMESH_TIME_MAX, UI_FILTER_DIRTY);
    ui_latch(&hash_shown, fault_stopped(MC_SOFTOFF_HASH_FAULT), UI_HASH_ERR);
    ui_latch(&stall_shown, fault_stopped(MC_SOFTOFF_FAN_ERR), UI_FAN_STALL);
    ui_latch(&aging_shown, fault_stopped(MC_SOFTOFF_CALI_FAILED), UI_AGING_ERR);
    /* a power fault latched at startup already shows the adapter screen */
    ui_latch(&power_fault_shown, fault_stopped(MC_SOFTOFF_POWER_FAULT) && !g_power_notice,
            UI_POWER_FAULT);
    ui_latch(&pool_shown, (errors & ERR_MM_POOL_INACTIVE) != 0 || atomic_load(&g_pool_unreached),
            UI_CHECK_POOL);
}
#else  /* SUPPORT_LCD */
static void status_popups(uint32_t errors)
{
    (void)errors;
}
#endif /* SUPPORT_LCD */

void mm_status_push(void)
{
    char     ip[INET_ADDRSTRLEN];
    uint32_t errors = g_startup_errors;
    if (!mm_net_present())
        errors |= ERR_MM_LAN_DAMAGED;
    if (net_state_ip(ip, sizeof ip) != 0)
        errors |= ERR_MM_IPLOST;
    if (fault_stopped(MC_SOFTOFF_POWER_FAULT))
        errors |= ERR_MM_POWER;
    if (fault_stopped(MC_SOFTOFF_FAN_ERR))
        errors |= ERR_MM_FAN;
    if (fault_stopped(MC_SOFTOFF_CALI_FAILED))
        errors |= ERR_MM_AGING_FAILED;
    if (mm_rail_off_unconfirmed())
        errors |= ERR_MM_POWER;
    if (g_softonoff.thermal_until)
        errors |= ERR_MM_OVERHOT;
    if (!atomic_load(&g_fan_dev_ok))
        errors |= ERR_MM_FAN;
    if (g_model_mismatch)
        errors |= ERR_MM_HW_UNMATCH;
    if (atomic_load(&g_pool_inactive))
        errors |= ERR_MM_POOL_INACTIVE;
    atomic_store(&g_live_ecmm, errors);
    mc_mm_status status = { .state = g_softonoff.thermal_until || g_softonoff.api_softoff ||
                        g_fault_off ?
                MC_WORK_IDLE :
                MC_WORK_INIT };
    osif_ipc_send(g_ipc_core_mm, MSG_MM_STATUS, &status, sizeof status);
    status_popups(errors);
}

static void pool_unreached_update(int waiting, uint64_t now_ms)
{
    if (!waiting) {
        g_no_job_waiting = 0;
        atomic_store(&g_pool_unreached, 0);
        return;
    }
    if (!g_no_job_waiting) {
        g_no_job_waiting  = 1;
        g_no_job_since_ms = now_ms;
    }
    atomic_store(&g_pool_unreached,
            now_ms - g_no_job_since_ms > POOL_INACTIVE_S * 1000u && mm_cfgwr_pool_configured());
}

void pool_idle_rule(void *ctx, uint64_t now_ms)
{
    char       ip[INET_ADDRSTRLEN];
    mm_telem_t tel;
    int        has_ip;

    (void)ctx;
    mm_telem_get(&tel);
    has_ip = net_state_ip(ip, sizeof ip) == 0;
    pool_unreached_update(tel.job_gap_s == 0 && has_ip, now_ms);
    if (tel.job_gap_s <= POOL_INACTIVE_S) {
        atomic_store(&g_pool_inactive, 0);
        return;
    }
    if (has_ip)
        atomic_store(&g_pool_inactive, 1);
    if (tel.job_gap_s <= POOL_REBOOT_S || atomic_load(&g_rebooting))
        return;
    osif_log(OSIF_LOG_WARN, "no new job > 6 min, %s ip", has_ip ? "has" : "none");
    (void)mm_system_reboot(MC_BOOTBY_POOL_INACTIVE, 0);
}

void filtermesh_load(void)
{
    char value[16];
    pthread_mutex_lock(&g_filtermesh_lock);
    g_filtermesh_time = 0;
    if (mm_cfgstore_get("misc", "filtermesh", value, sizeof value) == 0) {
        char *end;
        errno                 = 0;
        unsigned long minutes = strtoul(value, &end, 10);
        if (*value >= '0' && *value <= '9' && !*end && !errno && minutes <= UINT32_MAX)
            g_filtermesh_time = (uint32_t)minutes;
    }
    pthread_mutex_unlock(&g_filtermesh_lock);
}

uint32_t filtermesh_minutes(void)
{
    pthread_mutex_lock(&g_filtermesh_lock);
    uint32_t minutes = g_filtermesh_time;
    pthread_mutex_unlock(&g_filtermesh_lock);
    return minutes;
}

void filtermesh_clean(void)
{
    pthread_mutex_lock(&g_filtermesh_lock);
    g_filtermesh_time = 0;
    (void)mm_cfgstore_set("misc", "filtermesh", "0");
    pthread_mutex_unlock(&g_filtermesh_lock);
}

void filtermesh_rule(void *ctx, uint64_t now_ms)
{
    char buf[16];
    (void)ctx;
    (void)now_ms;
    pthread_mutex_lock(&g_filtermesh_lock);
    if (g_filtermesh_time < UINT32_MAX)
        g_filtermesh_time++;
    if (g_filtermesh_time % 60 == 0) {
        snprintf(buf, sizeof buf, "%u", g_filtermesh_time);
        (void)mm_cfgstore_set("misc", "filtermesh", buf);
    }
    pthread_mutex_unlock(&g_filtermesh_lock);
}

int filtermesh_keep(void)
{
    char buf[16];
    pthread_mutex_lock(&g_filtermesh_lock);
    snprintf(buf, sizeof buf, "%u", g_filtermesh_time);
    int rc = mm_cfgstore_put("misc", "filtermesh", buf);
    pthread_mutex_unlock(&g_filtermesh_lock);
    return rc;
}
