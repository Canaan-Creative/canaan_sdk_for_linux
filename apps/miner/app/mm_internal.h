/* SPDX-License-Identifier: MIT */
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

#ifndef MM_INTERNAL_H
#define MM_INTERNAL_H

#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

#include "boardconf.h"
#include "ctrl_cmds.h"
#include "enum_hold.h"
#include "mc_set_result.h"
#include "mm_power.h"
#include "mm_system.h"
#include "mm_telem.h"
#include "mm_wdt_sig.h"
#include "osif.h"
#include "softonoff.h"
#include "thermal.h"

#define CORE_BIN            "core_miner"
#define ASIC_BIN            "asic_miner"
#define RESTART_MS          3000u /* child restart delay, ms */
#define CHILD_EXEC_FAILED   127   /* child exit code when exec fails */
#define ASIC_EXEC_RETRY_MAX 3u    /* asic_miner exec failures retried, count */
#define WDT_STARVE_MS       90000u
#define WDT_CORE_WEDGE_MS   60000u
/* V/F tick gap after which the supervisor leg stops reporting, ms */
#define WDT_VF_STALL_MS     60000u
#define OVERHOT_HOLD_MS     180000u /* over-temperature softoff hold, ms */
#define FILTERMESH_TIME_MAX 120000u /* filter run time that raises the filter warning, minutes */

extern thermal_params_t g_thermal;
extern _Atomic int      g_ota_reboot;
extern _Atomic int      g_rebooting;
extern _Atomic int      g_reboot_failed;
extern _Atomic int      g_wdt_beat;      /* main loop heartbeat flag */
extern _Atomic int      g_wdt_released;  /* dog disarmed on shutdown flag */
extern _Atomic uint64_t g_core_spawn_at; /* last core_miner spawn time, ms */
extern _Atomic uint64_t g_asic_spawn_at; /* last asic_miner spawn time, ms */
extern uint64_t    g_core_live_at; /* wedge timer base: last live report, spawn or wedge kill, ms */
extern osif_ipc_t *g_ipc_core_mm;
extern osif_ipc_t *g_ipc_asic_mm;
extern _Atomic uint8_t   g_softoff_rsn; /* softoff reason, MC_SOFTOFF_* */
extern _Atomic int       g_fault_off;
extern softonoff_state_t g_softonoff;
extern enum_hold_t       g_enum_hold;
extern int               g_asic_ever_live; /* asic_miner live since last spawn flag */
extern unsigned          g_asic_exec_fails;
extern uint32_t          g_bootby_reason; /* boot reason, MC_BOOTBY_* */
extern uint32_t          g_bootby_sub;
extern int               g_model_mismatch;
extern uint32_t          g_startup_errors; /* startup errors, ERR_MM_* bit mask */
extern int               g_power_notice;   /* power fault popup flag */
extern uint16_t          g_ramp_freq;      /* current ramp frequency, MHz */
extern uint8_t           g_ramp_announced; /* ramp target reached flag */

void             mm_cfg_flush(void);
void             mm_hash_sn_set(unsigned hash, const char *sn);
void             mm_hash_sn_load(void);
void             mm_dna_load(void);
void             mm_periph_push(void);
void             mm_status_push(void);
void             pool_idle_rule(void *ctx, uint64_t now_ms);
void             filtermesh_load(void);
uint32_t         filtermesh_minutes(void);
void             filtermesh_clean(void);
void             filtermesh_rule(void *ctx, uint64_t now_ms);
int              filtermesh_keep(void);
int              mm_asic_stopped(void);
void             thermal_rule(void *ctx, uint64_t now_ms);
softonoff_action mm_overheat_step(uint64_t now_ms);
mc_set_result    mm_apply_soft(uint16_t op, const mc_ctrl_cmd *c);
void             soft_sched_rule(void *ctx, uint64_t now_ms);
int              mm_api_req_take(void);
void             mm_thermal_act(softonoff_action pa, pid_t *asic);
void             mm_enum_act(enum_hold_action ea);
void             mm_api_act(softonoff_action ap, int want_on, pid_t *asic);
void             mm_reboot_failed_act(void);
void             fan_health_rule(void *ctx, uint64_t now_ms);
void             mm_fan_fault_act(pid_t *asic);
void             mm_aging_fail_act(pid_t *asic);
void             mm_rail_off_fan(int duty);
void             mm_fault_stop(uint8_t rsn, int duty, pid_t asic);
uint8_t          mm_workmode_running(void);
uint8_t          mm_workmode_max(void);
uint16_t         mm_power_budget(void);
mc_set_result    mm_apply_workmode(uint16_t op, const mc_ctrl_cmd *c);
int              rail_power_off(void);
int              rail_power_off_for_reboot(void);
int              rail_power_on(void);
int              mm_rail_off_unconfirmed(void);
int              mm_rail_off_duty(int rc, int duty);
int              mm_power_vout_bad(int vout_mv, int pout_w, int vout12);
int              mm_rail_on_fault_latch(void);
int              mm_power_fault_stop(void);
void             mm_power_watch(const mm_telem_t *tel, pid_t asic);
void             freq_ramp_rule(void *ctx, uint64_t now_ms);
void            *mm_wdt_thread(void *arg);

#endif /* MM_INTERNAL_H */
