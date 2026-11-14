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
#include "mc_api_bootby.h"
#include "mm_efuse.h"
#include "mm_led.h"
#include "mm_spawn.h"
#include "mm_telem.h"
#include "mm_vf.h"
#include "wdt_dev.h"
#include "mm_cfgstore.h"
#include "osif.h"
#include "mm_ui_glue.h"

#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#define APPROOT       "/mntapp/release"
#define CORE_DIR      APPROOT "/linux/app"
#define ASIC_DIR      APPROOT "/asic"
#define CHILD_TERM_MS 3000u /* wait after SIGTERM before SIGKILL, ms */
#define CHILD_KILL_MS 500u  /* wait after SIGKILL, ms */

static uint64_t g_core_retry_at; /* core_miner respawn time, ms */
static uint64_t g_asic_retry_at; /* asic_miner respawn time, ms */
static pid_t    g_asic_term_pid; /* asic_miner pid being stopped, 0 none */
static uint64_t g_asic_term_at;  /* first SIGTERM, then SIGKILL time, ms */
static int      g_asic_term_killed;
static uint64_t g_start_gate_at; /* earliest first asic_miner spawn time, ms */

static pid_t spawn_child(const char *dir, const char *bin)
{
    char  rel[64];
    pid_t parent = getpid();
    pid_t pid;

    snprintf(rel, sizeof rel, "./%s", bin);
    pid = fork();
    if (pid < 0) {
        osif_log(OSIF_LOG_ERR, "could not start %s (fork failed)", bin);
        return -1;
    }
    if (pid == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
            _exit(CHILD_EXEC_FAILED);
        if (chdir(dir) == 0)
            execl(rel, bin, (char *)NULL);
        _exit(CHILD_EXEC_FAILED);
    }
    osif_log(OSIF_LOG_INFO, "started %s (pid %d)", bin, (int)pid);
    return pid;
}

void mm_asic_term(pid_t asic)
{
    if (asic <= 0)
        return;
    kill(asic, SIGTERM);
    if (asic == g_asic_term_pid)
        return;
    g_asic_term_pid    = asic;
    g_asic_term_at     = osif_now_ms();
    g_asic_term_killed = 0;
}

int mm_asic_termed(pid_t pid)
{
    return pid > 0 && pid == g_asic_term_pid;
}

void mm_asic_term_tick(uint64_t now, pid_t asic)
{
    if (!g_asic_term_pid)
        return;
    if (asic != g_asic_term_pid) {
        g_asic_term_pid = 0;
        return;
    }
    if (!g_asic_term_killed) {
        if ((int64_t)(now - g_asic_term_at) < (int64_t)CHILD_TERM_MS)
            return;
        osif_log(OSIF_LOG_WARN,
                "%s (pid %d) did not stop within %u s — "
                "killing it",
                ASIC_BIN, (int)asic, CHILD_TERM_MS / 1000u);
        kill(asic, SIGKILL);
        g_asic_term_killed = 1;
        g_asic_term_at     = now;
        return;
    }
    if ((int64_t)(now - g_asic_term_at) < (int64_t)CHILD_KILL_MS)
        return;
    osif_log(OSIF_LOG_ERR,
            "%s (pid %d) still not gone after being "
            "killed",
            ASIC_BIN, (int)asic);
    g_asic_term_pid = 0;
}

pid_t spawn_core(void)
{
    pid_t    pid = spawn_child(CORE_DIR, CORE_BIN);
    uint64_t now = osif_now_ms();

    atomic_store(&g_core_spawn_at, now);
    g_core_live_at  = now;
    g_core_retry_at = pid > 0 ? 0 : now + RESTART_MS;
    if (pid <= 0)
        osif_log(OSIF_LOG_ERR, "%s could not be started — retrying in %u s", CORE_BIN,
                RESTART_MS / 1000u);
    return pid;
}

pid_t spawn_asic(void)
{
    if (atomic_load(&g_rebooting)) {
        g_asic_retry_at = 0;
        osif_log(OSIF_LOG_INFO, "%s not started — reboot in progress", ASIC_BIN);
        return -1;
    }
    g_ramp_freq      = 0;
    g_ramp_announced = 0;
    mm_vf_set_ramp_target(0, 0);
    mm_telem_asic_reset(osif_ipc_conn_seq(g_ipc_asic_mm) + 1u);
    mm_vf_asic_spawned();
    g_asic_ever_live = 0;
    enum_hold_rearm(&g_enum_hold);
    mm_efuse_asic_spawned();
    g_asic_term_pid = 0;

    pid_t    pid = spawn_child(ASIC_DIR, ASIC_BIN);
    uint64_t now = osif_now_ms();

    atomic_store(&g_asic_spawn_at, now);
    g_asic_retry_at = pid > 0 ? 0 : now + RESTART_MS;
    if (pid <= 0)
        osif_log(OSIF_LOG_ERR, "%s could not be started — retrying in %u s", ASIC_BIN,
                RESTART_MS / 1000u);
    return pid;
}

void mm_start_gate_arm(uint64_t start_ms, pid_t *asic)
{
    g_start_gate_at        = start_ms + (uint64_t)ASIC_START_DELAY_S * 1000u;
    g_softonoff.start_wait = 1;
    osif_log(OSIF_LOG_INFO, "waiting %u s after start-up before starting %s",
            (unsigned)ASIC_START_DELAY_S, ASIC_BIN);
    mm_start_gate_release(osif_now_ms(), asic);
}

void mm_start_gate_release(uint64_t now, pid_t *asic)
{
    if (!g_softonoff.start_wait || (int64_t)(now - g_start_gate_at) < 0)
        return;
    g_softonoff.start_wait = 0;
    if (*asic <= 0 && !mm_asic_stopped()) {
        osif_log(OSIF_LOG_INFO, "start-up wait over — starting %s", ASIC_BIN);
        *asic = spawn_asic();
    }
}

void mm_spawn_later(int is_core, uint64_t at)
{
    if (is_core)
        g_core_retry_at = at;
    else
        g_asic_retry_at = at;
}

void mm_spawn_retry(uint64_t now, pid_t *core, pid_t *asic)
{
    if (g_core_retry_at && (int64_t)(now - g_core_retry_at) >= 0) {
        if (*core > 0)
            g_core_retry_at = 0;
        else
            *core = spawn_core();
    }
    if (g_asic_retry_at && (int64_t)(now - g_asic_retry_at) >= 0) {
        if (*asic > 0 || mm_asic_stopped())
            g_asic_retry_at = 0;
        else
            *asic = spawn_asic();
    }
}

static void shutdown_save(void)
{
    mm_cfg_flush();
    sync();
}

void mm_shutdown(pid_t core, pid_t asic)
{
    osif_log(OSIF_LOG_INFO,
            "shutting down — stopping child processes and "
            "hashboard power");
    if (g_sig && !atomic_load(&g_ota_reboot) && !mm_ui_poweroff_requested())
        (void)mm_bootby_stage(MC_BOOTBY_SYS, (uint32_t)g_sig);
    shutdown_save();
    mm_vf_stop_request();
    {
        pid_t kids[2] = { core, asic };
        (void)mm_children_stop(kids, 2, CHILD_TERM_MS, CHILD_KILL_MS);
    }
    mm_vf_stop();
    (void)rail_power_off();
    osif_ipc_deinit(g_ipc_core_mm);
    g_ipc_core_mm = NULL;
    osif_ipc_deinit(g_ipc_asic_mm);
    g_ipc_asic_mm = NULL;
    shutdown_save();
    mm_led_stop();
    wdt_dev_disarm();
    mm_ui_stop();
    osif_log(OSIF_LOG_INFO, "stopped");
}
