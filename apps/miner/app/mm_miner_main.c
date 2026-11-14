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

#include "framework.h"
#include "thermal.h"
#include "softonoff.h"
#include "enum_hold.h"
#include "fan_health.h"
#include "fan_dev.h"
#include "mm_telem.h"
#include "process_version.h"
#include "boardconf.h"
#include "board_desc.h"
#include "osif.h"

#include "miner_ipc.h"
#include "mc_errcode.h"
#include "mm_ctrl.h"
#include "mm_internal.h"
#include "hashmcu.h"
#include "mm_logrot.h"
#include "mm_model.h"
#include "mm_spawn.h"
#include "mm_led.h"
#include "mm_net.h"
#include "net_discover.h"
#include "mm_ota.h"
#include "mm_cfgwr.h"
#include "mm_cfgstore.h"
#include "mm_factory.h"
#include "mm_efuse.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/wait.h>
#ifdef SUPPORT_BLE
#include "mm_ble.h"
#include "wifi_mgr.h"
#endif /* SUPPORT_BLE */
#include "mm_ui_glue.h"
#include "wdt_dev.h"
#include "psu_ops.h"
#include "miner_chan.h"
#include "mm_vf.h"
#include "mm_fan.h"
#include "mm_tz.h"
#include "mc_api_bootby.h"
#include "net_hwaddr.h"

thermal_params_t g_thermal = {
    .over_temp        = PVT_TEMP_OVER_HR_MAX, /* trips when chip TMax is above it, degC */
    .outlet_over_temp = HASHOUT_OVER_HR_MAX,  /* hottest hashboard outlet limit, degC */
    /* consecutive 1 s samples with the outlet above its limit before a trip, 0 disables */
    .outlet_over_count = HASHOUT_OVERHOT_CNT_MAX,
};
volatile sig_atomic_t g_run = 1;
volatile sig_atomic_t g_sig = 0; /* signal that stopped the main loop, 0 none */
_Atomic int           g_ota_reboot;
_Atomic int           g_rebooting;     /* non-zero once reboot/power-off started, never cleared */
_Atomic int           g_reboot_failed; /* reboot() returned flag, consumed by the main loop */
_Atomic int           g_wdt_beat; /* main loop heartbeat, set each pass, cleared by mm_wdt_thread */
_Atomic int      g_wdt_released;  /* set after mm_shutdown() disarmed the dog; ends mm_wdt_thread */
_Atomic uint64_t g_core_spawn_at; /* last core_miner spawn time, ms, 0 never spawned */
_Atomic uint64_t g_asic_spawn_at; /* last asic_miner spawn time, ms, 0 never spawned */

/* core_miner wedge timer base: last live report, spawn or wedge kill, ms */
uint64_t          g_core_live_at;
osif_ipc_t       *g_ipc_core_mm;
osif_ipc_t       *g_ipc_asic_mm;
_Atomic uint8_t   g_softoff_rsn; /* softoff reason, MC_SOFTOFF_* */
_Atomic int       g_fault_off;
softonoff_state_t g_softonoff;
enum_hold_t       g_enum_hold;

static mm_fan_ctx_t g_fan_ctx = { .softonoff = &g_softonoff,
    .enum_hold                               = &g_enum_hold,
    .fault_off                               = &g_fault_off };
int                 g_asic_ever_live; /* asic_miner live since last spawn flag */
unsigned g_asic_exec_fails; /* consecutive asic_miner exec failures, 0..ASIC_EXEC_RETRY_MAX */
uint32_t g_bootby_reason = MC_BOOTBY_POWRON; /* boot reason, MC_BOOTBY_* */
uint32_t g_bootby_sub;
int      g_model_mismatch; /* boot env HW/SW differ from this firmware flag */
uint32_t g_startup_errors; /* startup errors, ERR_MM_* bit mask */
int      g_power_notice;   /* start-up power fault LCD popup flag, never cleared */

/*
 * fan_health_rule context: .err_count consecutive 2 s samples with a driven fan
 * reading below FAN_STALL_RPM trip the fan fault softoff, 0 disables the rule
 */
static fan_health_params_t g_fan_health = { .err_count = FAN_BELOW_STALL_CNT };

static void mm_asic_cb(uint32_t type, const void *data, int len)
{
    if (type == MSG_ASIC_TELEM)
        mm_telem_ingest(data, len, osif_ipc_conn_seq(g_ipc_asic_mm));
    if (type == MSG_ASIC_EFUSE)
        mm_efuse_ingest(data, len);
}

void mm_cfg_flush(void)
{
    if (mm_cfgstore_flush() < 0 && !mm_cfgstore_frozen())
        osif_log(OSIF_LOG_ERR, "settings NOT persisted; they are lost on reboot");
}

static void cfg_flush_rule(void *ctx, uint64_t now_ms)
{
    (void)ctx;
    (void)now_ms;
    mm_cfg_flush();
}

static void mm_ctrl_cb(uint32_t type, const void *data, int len)
{
    if (type == MSG_CORE_STATS) {
        mm_telem_ingest_core(data, len);
        return;
    }
    if (type == MSG_CTRL_CMD)
        mm_ctrl_cmd(data, len);
}

static void mm_rules_register(void)
{
    static ctl_rule_t rules[] = {
        { .name = "thermal", .cadence_ms = 1000, .evaluate = thermal_rule, .ctx = &g_thermal },
        { .name = "fan_pid", .cadence_ms = 2000, .evaluate = fan_rule, .ctx = &g_fan_ctx },
        { .name             = "fan_health",
                .cadence_ms = 2000,
                .evaluate   = fan_health_rule,
                .ctx        = &g_fan_health },
        { .name = "freq_ramp", .cadence_ms = 1000, .evaluate = freq_ramp_rule, .ctx = NULL },
        { .name = "logrot", .cadence_ms = 60000, .evaluate = logrot_rule, .ctx = NULL },
        { .name = "soft_sched", .cadence_ms = 1000, .evaluate = soft_sched_rule, .ctx = NULL },
        { .name = "filtermesh", .cadence_ms = 60000, .evaluate = filtermesh_rule, .ctx = NULL },
        { .name = "cfgflush", .cadence_ms = 30000, .evaluate = cfg_flush_rule, .ctx = NULL },
        { .name = "efuse", .cadence_ms = 1000, .evaluate = mm_efuse_rule, .ctx = NULL },
        { .name = "ip_show", .cadence_ms = 1000, .evaluate = mm_net_ip_show_rule, .ctx = NULL },
        { .name = "pool_idle", .cadence_ms = 30000, .evaluate = pool_idle_rule, .ctx = NULL },
#if NET_WIRED_ALT
        { .name = "net_wired", .cadence_ms = 2000, .evaluate = mm_net_wired_rule, .ctx = NULL },
#endif /* NET_WIRED_ALT */
#if defined(SUPPORT_BLE) && IMPL_NET_IS_WIFI
        { .name             = "net_wifi_repair",
                .cadence_ms = 2000,
                .evaluate   = wifi_link_repair_tick,
                .ctx        = NULL },
        { .name = "net_wifi_rssi", .cadence_ms = 5000, .evaluate = wifi_rssi_refresh, .ctx = NULL },
#endif /* defined(SUPPORT_BLE) && IMPL_NET_IS_WIFI */
    };
    _Static_assert(sizeof rules / sizeof rules[0] <= CTL_MAX_RULES,
            "more control rules than CTL_MAX_RULES");

    ctl_reset();
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++)
        ctl_register(&rules[i]);
}

static void mm_channels_open(void)
{
    static const miner_chan_t chans[] = {
        { &g_ipc_core_mm, MINER_IPC_CORE_MM_PATH, 1, mm_ctrl_cb, "ctrl", 0 },
        { &g_ipc_asic_mm, MINER_IPC_ASIC_MM_PATH, 1, mm_asic_cb, "asic-ctrl", 0 },
    };
    if (miner_chan_open(chans, (int)(sizeof chans / sizeof chans[0])) != 0 || !g_ipc_core_mm)
        osif_log(OSIF_LOG_ERR,
                "control channel not open — API settings and "
                "device commands will not take effect");
}

static void mm_board_desc_seed(void)
{
    const board_desc_t seed = {
        .hboard_count  = HBOARD_COUNT,
        .max_asic      = MAX_ASIC_ON_SINGLE_HASH,
        .fan_count     = FAN_COUNT,
        .freq_interval = FREQ_INTERVAL,
    };
    board_desc_init(&seed);
}

static void mm_loop_wait(uint32_t ms)
{
    uint64_t deadline = osif_now_ms() + ms;
    uint64_t now;

    while ((now = osif_now_ms()) < deadline)
        if (mm_ui_btn_wait((uint32_t)(deadline - now)))
            mm_ui_btn_drain();
}

static void mm_model_report(void)
{
    char env_hw[32], env_sw[32];
    int  model_status = mm_model_check(HW_TYPE, SW_TYPE, env_hw, env_sw, sizeof env_hw);
    g_model_mismatch  = model_status > 0;
    if (model_status < 0)
        osif_log(OSIF_LOG_OP,
                "board model not checked: HW/SW not readable from "
                "the boot environment (HW='%s' SW='%s')",
                env_hw, env_sw);
    else if (g_model_mismatch)
        osif_log(OSIF_LOG_OP,
                "board model mismatch: boot environment says "
                "HW='%s' SW='%s', this firmware is for HW='%s' SW='%s'",
                env_hw, env_sw, HW_TYPE, SW_TYPE);
}

static void mm_bootby_init(void)
{
    wdt_dev_init();
    int      wdt_bs = wdt_dev_bootstatus();
    uint8_t  br;
    uint32_t bs;
    mm_bootby_recover(wdt_bs, &br, &bs);
    g_bootby_reason = br;
    g_bootby_sub    = bs;
    osif_log(OSIF_LOG_OP, "[BOOT] Bootby:0x%02X.0x%08X (%s)", br, bs, mc_bootby_string(br));
}

static void mm_child_exited(pid_t d, int status, pid_t *core, pid_t *asic)
{
    int is_core = d == *core, is_asic = d == *asic;

    if (is_core)
        *core = -1;
    if (is_asic)
        *asic = -1;
    if (softonoff_intentional_exit(&g_softonoff, is_asic) || (is_asic && g_fault_off))
        return;
    const char *who         = is_core ? CORE_BIN : ASIC_BIN;
    int         not_started = WIFEXITED(status) && WEXITSTATUS(status) == CHILD_EXEC_FAILED;
    int         exec_retry  = not_started && (!is_asic || g_asic_exec_fails < ASIC_EXEC_RETRY_MAX);
    if (is_asic && !not_started)
        g_asic_exec_fails = 0;
    if (is_asic && exec_retry)
        g_asic_exec_fails++;
    if (exec_retry)
        osif_log(OSIF_LOG_ERR,
                "%s (pid %d) could not be started "
                "(program missing or not executable); retrying in %u s",
                who, (int)d, RESTART_MS / 1000u);
    else if (WIFSIGNALED(status))
        osif_log(OSIF_LOG_ERR,
                "%s (pid %d) was killed by signal %d; "
                "restarting in %u s",
                who, (int)d, WTERMSIG(status), RESTART_MS / 1000u);
    else if (!not_started)
        osif_log(OSIF_LOG_WARN,
                "%s (pid %d) exited with code %d; "
                "restarting in %u s",
                who, (int)d, WEXITSTATUS(status), RESTART_MS / 1000u);
    if (is_asic && !mm_asic_termed(d) && (not_started ? !exec_retry : !g_asic_ever_live)) {
        if (not_started)
            osif_log(OSIF_LOG_ERR,
                    "HASH FAULT: %s (pid %d) could not "
                    "be started after %u retries — mining stopped, "
                    "hashboard power off",
                    ASIC_BIN, (int)d, ASIC_EXEC_RETRY_MAX);
        else
            osif_log(OSIF_LOG_ERR,
                    "HASH FAULT: %s exited before its "
                    "chips came up — mining stopped, hashboard power "
                    "off",
                    ASIC_BIN);
        mm_fault_stop(MC_SOFTOFF_HASH_FAULT, FAN_DUTY_HASH_FAULT_PCT, *asic);
        return;
    }
    mm_spawn_later(is_core, osif_now_ms() + RESTART_MS);
}

static void mm_rail_off_retry(void)
{
    if (!mm_rail_off_unconfirmed())
        return;
    /* clang-format off */
    int duty =
            (g_softonoff.thermal_until || (g_fault_off && g_softoff_rsn == MC_SOFTOFF_FAN_ERR)) ?
            FAN_DUTY_SOFTOFF_PCT :
            (g_fault_off && g_softoff_rsn == MC_SOFTOFF_CALI_FAILED) ? FAN_DUTY_MIN_PCT :
            g_fault_off ? 0 :
            FAN_DUTY_API_SOFTOFF_PCT;
    /* clang-format on */
    mm_rail_off_fan(duty);
}

static void mm_core_wedge_check(uint64_t now, int core_live, pid_t core)
{
    if (core_live)
        g_core_live_at = now;
    if (core > 0 && (int64_t)(now - g_core_live_at) >= (int64_t)WDT_CORE_WEDGE_MS) {
        osif_log(OSIF_LOG_ERR,
                "%s stopped responding for %u s "
                "— killing and restarting it",
                CORE_BIN, WDT_CORE_WEDGE_MS / 1000u);
        kill(core, SIGKILL);
        g_core_live_at = now;
    }
}

int main(void)
{
    uint64_t start_ms = osif_now_ms();

    osif_log_op_sink_open();
    mm_wdt_sig_install();
    osif_log(OSIF_LOG_OP, "mm_miner " MM_MINER_VERSION " starting");
    mm_factory_boot_seed();
    mm_factory_boot_fill();
    mm_tz_restore();
    mm_cfgwr_hwcfg_set(PROD_NAME, MODEL);
    mm_cfgwr_seed(MC_SYSTEMCFG_DEFAULT);
    if (mm_cfgwr_start(NULL, NULL) != 0) {
        osif_log(OSIF_LOG_ERR, "configuration service failed to start — mm_miner exits");
        return 1;
    }
    char mac[18];
    mc_api_identity_mac(mac);
    osif_log(OSIF_LOG_INFO, "Mac %.2s:%.2s:%.2s:%.2s:%.2s:%.2s", mac, mac + 2, mac + 4, mac + 6,
            mac + 8, mac + 10);
    mm_model_report();

    mm_board_desc_seed();

    int prc = rail_power_on();
    if (prc != 0) {
        g_startup_errors |= ERR_MM_POWER;
        g_power_notice = 1;
        int latched    = mm_rail_on_fault_latch();
        osif_log(OSIF_LOG_ERR,
                "hashboard power-on failed at startup (%s, code %d) "
                "— %s",
                hal_psu_rail_strerror(prc), prc,
                latched ? "power fault latched" : "mining not stopped");
    }

    mm_hashboard_bringup();

    mm_hash_sn_load();

    mm_dna_load();

    if (fan_dev_init() != 0)
        g_startup_errors |= ERR_MM_FAN;
    else
        atomic_store(&g_fan_dev_ok, 1);

    mm_fan_target_apply(mm_workmode_running());

    filtermesh_load();
    osif_log(OSIF_LOG_DBG, "filter running time %u min (replace at %u min)", filtermesh_minutes(),
            (unsigned)FILTERMESH_TIME_MAX);

    mm_rules_register();

    mm_bootby_init();

    mm_led_start();

    mm_ui_start();

    mm_channels_open();
    mm_vf_start();

    pid_t core = spawn_core();
    pid_t asic = -1;
    mm_start_gate_arm(start_ms, &asic);

    char dev_name[16];
    mm_dev_name(dev_name, sizeof dev_name);
#ifdef SUPPORT_BLE
    net_provision_start(dev_name);
#endif /* SUPPORT_BLE */
    net_discover_start(dev_name);

    (void)mm_ota_start();

    pthread_t wdt_tid;
    int       wdt_thread_ok = pthread_create(&wdt_tid, NULL, mm_wdt_thread, NULL) == 0;
    if (!wdt_thread_ok) {
        /* Released rather than held unfed, which would boot-loop the board; the
         * counter keeps running and the kernel ping worker feeds it. No boot
         * reason is persisted: no watchdog reboot follows this branch. */
        osif_log(OSIF_LOG_ERR,
                "WDT thread could not start — WDT "
                "released (timeout %d s) and NOT supervising this boot",
                wdt_dev_timeout_s());
        wdt_dev_disarm();
    }

    while (g_run) {
        int   status = 0;
        pid_t d      = core > 0 ? waitpid(core, &status, WNOHANG) : 0;
        if (d <= 0 && asic > 0)
            d = waitpid(asic, &status, WNOHANG);
        if (d > 0) {
            mm_child_exited(d, status, &core, &asic);
            continue;
        }

        mm_telem_poll();
        uint64_t   now = osif_now_ms();
        mm_telem_t tel;

        mm_telem_get(&tel);
        if (asic > 0 && tel.asic_live) {
            g_asic_ever_live  = 1;
            g_asic_exec_fails = 0;
        }

        mm_asic_term_tick(now, asic);

        mm_power_watch(&tel, asic);

        softonoff_action pa = mm_overheat_step(now);
        mm_thermal_act(pa, &asic);
        mm_reboot_failed_act();

        {
            enum_hold_action ea = enum_hold_step(&g_enum_hold, tel.asic_live, FAN_DUTY_ENUM_PCT);
            mm_enum_act(ea);
        }

        mm_rail_off_retry();
        ctl_tick(now);

        mm_fan_fault_act(&asic);
        mm_aging_fail_act(&asic);

        int api_req = mm_api_req_take();
        if (api_req) {
            int              want_on = api_req > 0;
            softonoff_action ap      = softonoff_api_step(&g_softonoff, want_on);
            mm_api_act(ap, want_on, &asic);
        }

        mm_start_gate_release(now, &asic);

        mm_spawn_retry(now, &core, &asic);

        mm_status_push();
        mm_periph_push();
        mm_core_wedge_check(now, tel.core_live, core);
        atomic_store(&g_wdt_beat, 1);
        mm_ui_tick();
        mm_loop_wait(1000);
    }

    mm_shutdown(core, asic);
    atomic_store(&g_wdt_released, 1);
    if (wdt_thread_ok)
        pthread_join(wdt_tid, NULL);
    return 0;
}
