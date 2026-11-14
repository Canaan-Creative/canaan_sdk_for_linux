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

#include "pool_config.h"
#include "core_internal.h"
#include "core_ident.h"

#include <signal.h>
#include <string.h>
#include <stdlib.h>

#include "mc_api_server.h"
#include "core_ascset.h"
#include "core_pool.h"
#include "core_ctrl.h"
#include "core_softonoff.h"
#include "core_wallpaper.h"
#include "mc_api_bootby.h"
#include "fault_report.h"

#define POOL_BACKOFF_BASE_MS 1000 /* first pool retry backoff, ms */
#define POOL_BACKOFF_MAX_MS  60000

_Static_assert((unsigned)STRATUM_RESOLVE_TOTAL_MS + (unsigned)STRATUM_CONNECT_TOTAL_MS +
                        (unsigned)STRATUM_SEND_TIMEOUT_S * 1000u +
                        (unsigned)STRATUM_RECV_TIMEOUT_S * 1000u +
                        (unsigned)STRATUM_SEND_TIMEOUT_S * 1000u + (unsigned)POOL_BACKOFF_MAX_MS <
                CORE_MAIN_STALL_MS,
        "a session that parses no line must reach the next "
        "g_main_beat_ms stamp inside CORE_MAIN_STALL_MS: resolve + "
        "connect + configure send + recv + session_drop send wait + "
        "backoff");
_Static_assert(2u * (unsigned)STRATUM_RECV_TIMEOUT_S * 1000u +
                        (unsigned)STRATUM_SEND_TIMEOUT_S * 1000u + (unsigned)POOL_BACKOFF_BASE_MS <
                CORE_MAIN_STALL_MS,
        "an established session must reach the next g_main_beat_ms "
        "stamp inside CORE_MAIN_STALL_MS: recv deadline reset by the "
        "last parsed event + one more recv after an unparsed line + "
        "session_drop send wait + backoff reset to its base");

osif_ipc_t *g_ipc_asic_core;

time_t   g_start;      /* stats start time, s */
uint32_t g_local_work; /* clean notifies plus 2 per nonce check, count */

pthread_mutex_t        g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
double                 g_diff_acc; /* accepted share difficulty sum */
double                 g_diff_rej; /* rejected share difficulty sum */
double                 g_diff1;    /* valid nonce difficulty sum */
struct core_ping       g_ping;
struct core_nonce_stat g_nst;
struct core_hashwin    g_hashwin;

submit_rec g_sub[SUBMIT_LOG];

asic_chip_telem g_chip;                              /* last asic telemetry */
time_t          g_chip_at;                           /* g_chip arrival time, s */
uint64_t        g_mw[HBOARD_COUNT][MINER_MAX_CHIPS]; /* valid nonces per chip, count */

mm_periph g_periph; /* last mm peripheral snapshot */

uint32_t            g_mm_state; /* mm work state, MC_WORK_* */
osif_ipc_t         *g_ipc_core_mm;
miner_version_state g_mm_version;

_Atomic uint64_t g_main_beat_ms; /* main loop heartbeat, ms */

struct core_stratum g_stratum = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .send = PTHREAD_MUTEX_INITIALIZER,
};

uint8_t  g_bootby_reason = MC_BOOTBY_POWRON; /* boot reason, MC_BOOTBY_* */
uint32_t g_bootby_sub;

pthread_mutex_t      g_pools_lock = PTHREAD_MUTEX_INITIALIZER;
struct core_pool_pub g_pool_pub[POOL_VALID_NUM];
_Static_assert(MC_API_POOL_URL_CAP == POOL_URL_MAX, "the API reads back the stored url whole");
int                   g_pool_pub_n;
int                   g_pool_active                  = -1; /* active pool index, -1 if none */
int                   g_pool_enabled[POOL_VALID_NUM] = { 1, 1, 1 };
int                   g_pool_prio[POOL_VALID_NUM] = { 0, 1, 2 }; /* pool priorities, 0 = highest */
struct core_pool_stat g_pool_st[POOL_VALID_NUM];
uint32_t              g_pool_failcnt;    /* connected pool sessions ended, count */
uint32_t              g_netfail_time[8]; /* network failure start, end pairs, s */
struct core_block     g_block = {
        .hash     = "000000000000000000000000000000000000",
        .net_diff = 18446744073709551616.0,
};
double   g_board_diff1[HBOARD_COUNT]; /* valid nonce difficulty per board */
time_t   g_dev_start;                 /* first valid nonce time, s */
uint32_t g_last_valid_work;           /* last valid nonce time, s */

job_tmpl g_ring[JOB_RING];
uint32_t g_job_seq  = 0;        /* newest job sequence number */
uint32_t g_dead_seq = 0;        /* jobs at or below this sequence number are retired */
char     g_user[POOL_USER_MAX]; /* session worker name */

uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

double now_mono_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

time_t now_s(void)
{
    return (time_t)(now_ms() / 1000);
}

static void ctrl_ack_cb(uint32_t type, const void *data, int len)
{
    if (type == MSG_MM_VERSION) {
        pthread_mutex_lock(&g_stats_lock);
        miner_version_receive(&g_mm_version, data, len, now_s());
        pthread_mutex_unlock(&g_stats_lock);
        return;
    }
    if (type == MSG_MM_BOOTBY && data && len == (int)sizeof(mc_mm_bootby)) {
        mc_mm_bootby bb;
        memcpy(&bb, data, sizeof bb);
        pthread_mutex_lock(&g_stats_lock);
        g_bootby_reason = (uint8_t)bb.reason;
        g_bootby_sub    = bb.sub;
        pthread_mutex_unlock(&g_stats_lock);
        return;
    }
    if (type == MSG_MM_STATUS && data && len == (int)sizeof(mc_mm_status)) {
        mc_mm_status status;
        memcpy(&status, data, sizeof status);
        if (status.state <= MC_WORK_FAULT) {
            pthread_mutex_lock(&g_stats_lock);
            g_mm_state = status.state;
            pthread_mutex_unlock(&g_stats_lock);
        }
        return;
    }
    if (type == MSG_CTRL_ACK && data && len == (int)sizeof(mc_ctrl_ack)) {
        mc_ctrl_ack a;

        memcpy(&a, data, sizeof a);
        osif_log(OSIF_LOG_DBG, "mm_miner acknowledged a control command (result %d)", a.result);
        core_ctrl_ack(data, len);
        return;
    }
    if (type == MSG_MM_PERIPH && data) {
        static fault_report_t len_fault; /* wrong-size MSG_MM_PERIPH fault report state */
        int                   bad_len = len != (int)sizeof(mm_periph);
        if (fault_report_step(&len_fault, (uint32_t)bad_len) != FAULT_REPORT_NONE) {
            if (bad_len)
                osif_log(OSIF_LOG_ERR,
                        "MSG_MM_PERIPH dropped, "
                        "%d bytes, %d expected",
                        len, (int)sizeof(mm_periph));
            else
                osif_log(OSIF_LOG_INFO,
                        "MSG_MM_PERIPH accepted "
                        "again (%d bytes)",
                        len);
        }
        if (bad_len)
            return;
        pthread_mutex_lock(&g_stats_lock);
        memcpy(&g_periph, data, sizeof g_periph);
        for (int b = 0; b < HBOARD_COUNT; b++)
            g_periph.hash_sn[b][HASH_SN_LEN] = '\0';
        g_periph.webpass[WEBPASS_HEX_LEN]  = '\0';
        g_periph.prod[PROD_NAME_CAP - 1]   = '\0';
        g_periph.model[MODEL_NAME_CAP - 1] = '\0';
        g_periph.tz[MM_TZ_NAME_CAP - 1]    = '\0';
        core_webpass_cache_set(g_periph.webpass);
        core_hwcfg_set(g_periph.prod, g_periph.model);
        core_setters_ledmode_set(g_periph.ledmode);
        core_setters_tz_set(g_periph.tz);
        pthread_mutex_unlock(&g_stats_lock);
    }
}

int main(int argc, char **argv)
{
    const char *conf = argc > 1 ? argv[1] : POOL_CONFIG_DEFAULT;
    signal(SIGPIPE, SIG_IGN);
    setenv("RES_OPTIONS", STRATUM_RESOLVE_RES_OPTIONS, 1);
    osif_log_op_sink_open();
    osif_log(OSIF_LOG_OP, "starting, version " CORE_MINER_VERSION);
    osif_log(OSIF_LOG_DBG,
            "resolver bounded by RES_OPTIONS=\"%s\" "
            "(worst case %d ms over at most %d nameservers)",
            STRATUM_RESOLVE_RES_OPTIONS, STRATUM_RESOLVE_TOTAL_MS, STRATUM_RESOLVE_NS_MAX);
    stratum_set_trace(pool_trace);
    g_start = now_s();

    pool_set pools;
    pool_init(&pools, POOL_BACKOFF_BASE_MS, POOL_BACKOFF_MAX_MS);
    int np = pool_config_load(&pools, conf);
    if (np > 0)
        publish_pools(&pools);

    mc_pipeline pipe;
    mc_pipeline_init(&pipe);

    if (osif_ipc_init(&g_ipc_asic_core, nonce_cb, MINER_IPC_ASIC_CORE_PATH, 1) != OSIF_OK)
        osif_log(OSIF_LOG_ERR,
                "cannot open the channel to asic_miner (%s); "
                "no work can reach the chips",
                MINER_IPC_ASIC_CORE_PATH);
    g_stratum.pipe = &pipe;

    {
        core_hwcfg_buf hw;

        core_hwcfg_copy(&hw);
        osif_log(OSIF_LOG_DBG,
                "hwcfg prod \"%s\" model \"%s\" "
                "(board default until the first MSG_MM_PERIPH)",
                hw.prod, hw.model);
    }

    if (osif_ipc_init(&g_ipc_core_mm, ctrl_ack_cb, MINER_IPC_CORE_MM_PATH, 0) != OSIF_OK)
        osif_log(OSIF_LOG_ERR,
                "cannot open the channel to mm_miner (%s); "
                "settings changes will not be applied",
                MINER_IPC_CORE_MM_PATH);

    pthread_t pump;
    int       err = pthread_create(&pump, NULL, pump_thread, NULL);

    if (err) {
        osif_log(OSIF_LOG_ERR, "cannot start the work thread (%s); exiting", strerror(err));
        return 1;
    }

    osif_log(OSIF_LOG_DBG, "%d ascset setter(s) registered",
            core_setters_register(MC_SYSTEMCFG_DEFAULT));
    if (core_pool_setter_register(conf, MC_SYSTEMCFG_DEFAULT) == 0)
        osif_log(OSIF_LOG_DBG, "setpool command registered");
    else
        osif_log(OSIF_LOG_ERR, "the setpool API command is unavailable");
    if (core_pool_cmds_register(conf) == 0)
        osif_log(OSIF_LOG_DBG,
                "pool commands registered "
                "(enable/disable/add/remove/switch/priority/zero)");
    else
        osif_log(OSIF_LOG_ERR,
                "some pool API commands are unavailable "
                "(enable/disable/add/remove/switch/priority/zero)");
    osif_log(OSIF_LOG_DBG, "%d soft-on/off command(s) registered", core_soft_setter_register());
    osif_log(OSIF_LOG_DBG, "%d wallpaper setter(s) registered", core_wallpaper_register());
    mc_api_server_start(api_snapshot, &pipe);

    if (np <= 0)
        osif_log(OSIF_LOG_WARN, "no mining pool is configured; mining waits until one is set");
    while (np <= 0) {
        osif_sleep_ms(10000);
        pool_init(&pools, POOL_BACKOFF_BASE_MS, POOL_BACKOFF_MAX_MS);
        np = pool_config_load(&pools, conf);
        if (np > 0)
            publish_pools(&pools);
    }
    osif_log(OSIF_LOG_INFO, "%d mining pool(s) configured", np);

    pthread_t probe;
    err = pthread_create(&probe, NULL, pool_probe_thread, NULL);
    if (err)
        osif_log(OSIF_LOG_ERR,
                "cannot start the pool probe (%s); a recovered "
                "pool is not switched back to automatically",
                strerror(err));
    else
        pthread_detach(probe);

    for (;;)
        pool_session_once(&pools, &pipe);
}
