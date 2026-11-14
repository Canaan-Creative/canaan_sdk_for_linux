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

#include "hal.h"
#include "a3197s_ops.h"
#include "a3197s_pll_seq.h"
#include "a3197s_decode.h"
#include "uart_pro.h"
#include "driver_avalon.h"
#include "avalon_gen.h"
#include "toast_work.h"
#include "miner_ipc.h"
#include "process_version.h"
#include "miner_chan.h"
#include "nonce_ring.h"
#include "osif.h"

#include "boardconf.h"
#include "asic_temp_pub.h"
#include "asic_internal.h"
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#define NONCE_RING_CAP    32
#define ENUM_RETRY_MAX    30
#define TZSET_TELEM_TICKS 3u /* tzset() period, in TELEM_PERIOD_MS ticks (3 s) */
#define NEXT_BOARD() \
    do { \
        ch = (uint8_t)((ch + 1) % HBOARD_COUNT); \
        osif_sleep_ms(SCAN_CADENCE_MS); \
    } while (0)

_Static_assert(sizeof(asic_chip_telem) <= OSIF_IPC_MAX, "asic_chip_telem > OSIF_IPC_MAX");
_Static_assert(offsetof(asic_chip_telem, tm) == 0, "asic_telem must lead asic_chip_telem");
_Static_assert(sizeof(mc_asic_job) <= OSIF_IPC_MAX, "mc_asic_job > OSIF_IPC_MAX");

static void ipc_cb(uint32_t type, const void *data, int len);

volatile sig_atomic_t g_run = 1;
osif_ipc_t           *g_ipc_asic_core;
osif_ipc_t           *g_ipc_asic_mm;
nonce_ring_t          g_nring; /* nonces waiting to be sent */
volatile sig_atomic_t g_efuse_req;
volatile uint8_t      g_bin_grade; /* chip bin grade from efuse */
struct asic_board     g_bd[HBOARD_COUNT];

/* serializes data handed from IPC callbacks to the main loop */
static osif_lock_t *g_ipc_lock;

static mc_asic_job      g_job; /* latest job from core_miner */
static _Atomic uint32_t g_job_cnt;

static uint32_t    g_work_nonce2;     /* next extranonce2 to send */
static uint32_t    g_last_job_nonce2; /* last extranonce2 of the previous job */
static mc_asic_job g_cur_job;         /* job that work is built from */
static uint32_t    g_last_job_cnt;    /* the g_job_cnt at the last g_cur_job update */

static uint16_t              g_pll_freq[ASIC_PLL_POINTS]; /* PLL points from mm_miner, MHz */
static volatile sig_atomic_t g_pll_at_target;

static avalon_nonce_out g_nring_slots[NONCE_RING_CAP]; /* g_nring storage */

static uint8_t g_overflow_logged;

static const miner_chan_t g_chans[] = {
    { &g_ipc_asic_core, MINER_IPC_ASIC_CORE_PATH, 0, ipc_cb, "datapath", 1 },
    { &g_ipc_asic_mm, MINER_IPC_ASIC_MM_PATH, 0, ipc_cb, "asic-ctrl", 0 },
};

static void on_sig(int s)
{
    (void)s;
    g_run = 0;
}

static void ipc_lock(void)
{
    osif_lock_acquire(g_ipc_lock);
}

static void ipc_unlock(void)
{
    osif_lock_release(g_ipc_lock);
}

static void asic_send_nonce(void)
{
    void *it;
    while ((it = nonce_ring_peek(&g_nring)) != NULL) {
        if (osif_ipc_send(g_ipc_asic_core, MSG_ASIC_NONCE, it, (int)sizeof(avalon_nonce_out)) !=
                OSIF_OK)
            return;
        nonce_ring_consume(&g_nring);
        g_overflow_logged = 0;
    }
}

static void asic_job_clamp(mc_asic_job *j)
{
    j->version[sizeof j->version - 1]   = 0;
    j->prevhash[sizeof j->prevhash - 1] = 0;
    j->ntime[sizeof j->ntime - 1]       = 0;
    j->nbits[sizeof j->nbits - 1]       = 0;
    j->xnonce1[sizeof j->xnonce1 - 1]   = 0;
    j->mask[sizeof j->mask - 1]         = 0;
    j->coinb1[sizeof j->coinb1 - 1]     = 0;
    j->coinb2[sizeof j->coinb2 - 1]     = 0;
    j->merkle[sizeof j->merkle - 1]     = 0;
}

static void ipc_cb(uint32_t type, const void *data, int len)
{
    static int badlen = -1;

    if (type == MSG_ASIC_JOB) {
        if (data && len != (int)sizeof(mc_asic_job)) {
            if (len != badlen) {
                badlen = len;
                osif_log(OSIF_LOG_ERR,
                        "job from core_miner dropped: %d bytes, "
                        "expected %d (MSG_ASIC_JOB); core_miner and asic_miner "
                        "may come from different builds",
                        len, (int)sizeof(mc_asic_job));
            }
        } else if (data) {
            mc_asic_job j;
            badlen = -1;
            memcpy(&j, data, sizeof j);
            asic_job_clamp(&j);
            ipc_lock();
            int moved = !g_job_cnt || j.target_h != g_job.target_h || j.target_l != g_job.target_l;
            g_job     = j;
            g_job_cnt++;
            if (moved)
                for (int i = 0; i < HBOARD_COUNT; i++)
                    g_bd[i].target_pending = 1;
            ipc_unlock();
        }
        return;
    }
    if (type == MSG_ASIC_VF) {
        if (data && len == (int)sizeof(mc_asic_vf)) {
            mc_asic_vf v;
            memcpy(&v, data, sizeof v);
            if (v.freq[0] != 0) {
                ipc_lock();
                for (int i = 0; i < ASIC_PLL_POINTS; i++)
                    g_pll_freq[i] = v.freq[i];
                if (v.at_target)
                    g_pll_at_target = 1;
                for (int i = 0; i < HBOARD_COUNT; i++)
                    g_bd[i].pll_pending = 1;
                ipc_unlock();
            }
        }
        return;
    }
    if (type == MSG_MM_EFUSE) {
        if (data && len == (int)sizeof(mm_efuse_state)) {
            mm_efuse_state st;
            memcpy(&st, data, sizeof st);
            if (st.present)
                g_bin_grade = st.bin_grade;
            else
                g_efuse_req = 1;
        }
        return;
    }
}

static int set_asic_freq(uint8_t ch)
{
    if (g_bd[ch].pll_pending) {
        ipc_lock();
        g_bd[ch].pll_pending = 0;
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            g_bd[ch].pll_pts[i] = g_pll_freq[i];
        ipc_unlock();
    }
    if (!memcmp(g_bd[ch].cmd_pts, g_bd[ch].pll_pts, sizeof g_bd[ch].cmd_pts))
        return 0;
    memcpy(g_bd[ch].cmd_pts, g_bd[ch].pll_pts, sizeof g_bd[ch].cmd_pts);
    a3197s_set_freq(g_bd[ch].pll_pts);
    hal_asic()->set_pll(ch, A3197S_CHIP_BROADCAST, g_bd[ch].ri_sta);
    g_bd[ch].work_freq = (uint16_t)correct_freq(g_bd[ch].pll_pts[0]);
    return 1;
}

static int asic_bringup(uint8_t ch)
{
    g_bd[ch].local_work = 0;
    asic_telem_board_reset(ch);
    g_bd[ch].ri_sta = 0;
    g_bd[ch].stage  = BS_RI;
    g_bd[ch].chip   = 0;
    uart_init(ch, UART_DEFAULT_BAUD_RATE);

    a3197s_enter_work_mode(ch);
    osif_sleep_ms(1);
    a3197s_enter_config_mode(ch);
    osif_sleep_ms(1);

    uint32_t n = 0;
    for (int try = 0; try < ENUM_RETRY_MAX && g_run; try++) {
        n = a3197s_enumerate(ch);
        osif_log(OSIF_LOG_INFO, "enumerate -> %u chip(s) (try %d)", n, try);
        if (n > 0 && n <= MINER_MAX_CHIPS)
            break;
        n = 0;
        a3197s_enter_work_mode(ch);
        osif_sleep_ms(500);
        a3197s_enter_config_mode(ch);
        osif_sleep_ms(500);
    }
    if (n == 0)
        return -1;

    a3197s_config_asic(ch);

    uart_chip_select(ch, A3197S_CHIP_BROADCAST);
    uart_write_baud_cmd(ch, UART_HIGH_BAUD_RATE);
    osif_sleep_ms(30);
    uart_config_baud(ch, UART_HIGH_BAUD_RATE);
    osif_sleep_ms(30);
    uart_clear_rxfifo(ch);

    uart_chip_select(ch, (uint16_t)(n - 1));
    uint32_t st = 0;
    a3197s_read_reg(ch, A3197S_UART_STATUS, &st, 1);
    osif_log(OSIF_LOG_INFO,
            "board %u: %u chips found, chip link running at "
            "%u baud, freq ratio %u (0x%08x)",
            (unsigned)ch, n, (unsigned)UART_HIGH_BAUD_RATE, (st & 0x7f800u) >> 11, st);
    uart_chip_select(ch, A3197S_CHIP_BROADCAST);

    return (int)n;
}

static void asic_deinit(void)
{
    miner_chan_close(g_chans, (int)(sizeof g_chans / sizeof g_chans[0]));
    osif_log(OSIF_LOG_INFO, "stopped");
}

static int asic_set_target(uint8_t ch)
{
    if (!g_bd[ch].target_pending)
        return 0;
    ipc_lock();
    g_bd[ch].target_pending = 0;
    uint32_t th = g_job.target_h, tl = g_job.target_l;
    ipc_unlock();
    uart_chip_select(ch, A3197S_CHIP_BROADCAST);
    if (avalon_set_target(ch, th, tl) != HAL_OK) {
        ipc_lock();
        g_bd[ch].target_pending = 1;
        ipc_unlock();
        return 0;
    }
    return 1;
}

static void asic_preset_work(uint8_t ch)
{
    uart_chip_select(ch, A3197S_CHIP_BROADCAST);
    a3197s_mx2_reset(ch);
    a3197s_config_coreregs(ch);
    uart_chip_select(ch, g_bd[ch].chip);
}

static int asic_build_work(const mc_asic_job *j, uint32_t n2, uint8_t ch, uint8_t chip,
        struct toast_work *w)
{
    avalon_job_in jobin;
    if (mc_gen_chip_work(j, n2, &jobin) != 0)
        return -1;
    jobin.user_note[0] = n2;
    jobin.user_note[1] = j->seq;
    jobin.user_note[2] = __builtin_bswap32(((uint32_t)ch << 16) | (uint32_t)chip);
    jobin.user_note[3] = 0;
    avalon_build_work(&jobin, w);
    return 0;
}

static void asic_send_default_work(uint8_t ch)
{
    if (g_bd[ch].chips_n <= 0)
        return;

    mc_asic_job tpl;
    mc_default_job(&tpl);

    struct toast_work w;

    if (asic_build_work(&tpl, tpl.n2_base, ch, 0, &w) != 0) {
        osif_log(OSIF_LOG_ERR,
                "default work build failed — board %u "
                "will not hash until a pool job arrives",
                (unsigned)ch);
        return;
    }

    uart_chip_select(ch, A3197S_CHIP_BROADCAST);
    avalon_set_target(ch, tpl.target_h, tpl.target_l);
    memcpy(g_bd[ch].cur_ver, w.version, sizeof g_bd[ch].cur_ver);
    a3197s_write_version(ch, g_bd[ch].cur_ver);
    avalon_send_work(ch, &w);
    g_bd[ch].local_work++;
    uart_chip_select(ch, g_bd[ch].chip);
    osif_log(OSIF_LOG_DBG, "board %u swept the default work", (unsigned)ch);
}

static int asic_job_same(const mc_asic_job *a, const mc_asic_job *b)
{
    return a->seq == b->seq && a->xnonce2_size == b->xnonce2_size &&
            !strcmp(a->version, b->version) && !strcmp(a->prevhash, b->prevhash) &&
            !strcmp(a->ntime, b->ntime) && !strcmp(a->nbits, b->nbits) &&
            !strcmp(a->xnonce1, b->xnonce1) && !strcmp(a->mask, b->mask) &&
            !strcmp(a->coinb1, b->coinb1) && !strcmp(a->coinb2, b->coinb2) &&
            !strcmp(a->merkle, b->merkle);
}

static void asic_send_work(uint8_t ch)
{
    if (g_last_job_cnt != g_job_cnt) {
        mc_asic_job j;
        ipc_lock();
        j              = g_job;
        g_last_job_cnt = g_job_cnt;
        ipc_unlock();
        if (!asic_job_same(&j, &g_cur_job)) {
            g_last_job_nonce2 = g_work_nonce2 - 1u;
            g_work_nonce2     = j.n2_base;
        }
        g_cur_job = j;
    }

    struct toast_work w;
    if (asic_build_work(&g_cur_job, g_work_nonce2, ch, g_bd[ch].chip, &w) != 0)
        return;
    if (memcmp(g_bd[ch].cur_ver, w.version, sizeof g_bd[ch].cur_ver) != 0) {
        memcpy(g_bd[ch].cur_ver, w.version, sizeof g_bd[ch].cur_ver);
        uart_chip_select(ch, A3197S_CHIP_BROADCAST);
        a3197s_write_version(ch, g_bd[ch].cur_ver);
    }
    uart_chip_select(ch, g_bd[ch].chip);
    avalon_send_work(ch, &w);
    g_bd[ch].local_work++;
    g_work_nonce2++;
}

static void asic_read_nonce(uint8_t ch)
{
    avalon_nonce_out nb[TOAST_NONCE_COUNT];
    uart_chip_select(ch, g_bd[ch].chip);
    int got = avalon_collect_nonces(ch, nb, TOAST_NONCE_COUNT);
    for (int i = 0; i < got; i++) {
        nb[i].last_job_nonce2 = g_last_job_nonce2;
        if (nonce_ring_push(&g_nring, &nb[i]) != 0 && !g_overflow_logged) {
            g_overflow_logged = 1;
            osif_log(OSIF_LOG_ERR,
                    "nonce buffer full: %u nonces discarded "
                    "because core_miner did not collect them in time",
                    g_nring.discarded);
        }
    }
}

static void start_asic_work(uint8_t ch)
{
    a3197s_enable_nonce_return(ch);
    osif_log(OSIF_LOG_INFO, "read nonce begin");
}

int main(void)
{
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    osif_log_op_sink_open();
    osif_log(OSIF_LOG_INFO, "starting, version " ASIC_MINER_VERSION " (A3197S chips)");
    if (osif_lock_create(&g_ipc_lock) != OSIF_OK) {
        osif_log(OSIF_LOG_ERR, "cannot create the IPC lock; asic_miner exits");
        return 1;
    }
    nonce_ring_init(&g_nring, g_nring_slots, sizeof g_nring_slots[0], NONCE_RING_CAP);
    miner_chan_open(g_chans, (int)(sizeof g_chans / sizeof g_chans[0]));

    asic_telem_init();
    mc_default_job(&g_cur_job);
    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        g_bd[bd].work_freq = INIT_FREQ;
        for (int i = 0; i < ASIC_PLL_POINTS; i++)
            g_bd[bd].pll_pts[i] = INIT_FREQ;
    }
    uint32_t telem_at = (uint32_t)osif_now_ms();
    uint32_t telem_n  = 0;

    osif_sleep_ms(1000);

    int up = 0;
    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        int n = asic_bringup((uint8_t)bd);
        if (!g_run)
            break;
        if (n < 0) {
            osif_log(OSIF_LOG_ERR,
                    "board %d: no valid chip count (1..%d) after %d scans; "
                    "the board stays off",
                    bd, MINER_MAX_CHIPS, ENUM_RETRY_MAX);
            g_bd[bd].chips_n = 0;
            continue;
        }
        g_bd[bd].chips_n = n;
        up++;
    }
    if (!g_run) {
        asic_deinit();
        return 0;
    }
    if (up == 0) {
        osif_log(OSIF_LOG_ERR, "no hashboard found; asic_miner exits");
        asic_deinit();
        return 1;
    }

    uint8_t ch = 0;
    while (g_run) {
        int      n   = g_bd[ch].chips_n;
        uint32_t now = (uint32_t)osif_now_ms();

        if (n <= 0) {
            NEXT_BOARD();
            continue;
        }
        if (set_asic_freq(ch)) {
            NEXT_BOARD();
            continue;
        }
        if (asic_set_target(ch)) {
            NEXT_BOARD();
            continue;
        }

        if ((int32_t)(now - telem_at) >= 0) {
            telem_at = now + TELEM_PERIOD_MS;
            if (telem_n++ % TZSET_TELEM_TICKS == 0)
                tzset();
            asic_send_info(ch);
        }

        uart_chip_select(ch, g_bd[ch].chip);
        if (!uart_tx_empty(ch)) {
            ch = (uint8_t)((ch + 1) % HBOARD_COUNT);
            osif_sleep_ms(1);
            continue;
        }

        if (g_bd[ch].stage == BS_RUN) {
            asic_read_nonce(ch);
            asic_send_nonce();
        }

        if (g_bd[ch].ri_sta)
            asic_collect_status();

        uart_chip_select(ch, g_bd[ch].chip);

        switch (g_bd[ch].stage) {
        case BS_COREREGS:
            if (!g_pll_at_target)
                break;
            asic_preset_work(ch);
            asic_send_default_work(ch);
            start_asic_work(ch);
            g_bd[ch].stage = BS_RUN;
            osif_sleep_ms(SCAN_CADENCE_MS);
            continue;
        case BS_RUN:
            asic_send_work(ch);
            g_bd[ch].chip = (uint8_t)((g_bd[ch].chip + 1) % n);
            break;
        default:
            break;
        }

        if (g_bd[ch].stage == BS_RI) {
            a3197s_enter_work_mode(ch);
            for (int i = 0; i < ASIC_PLL_POINTS; i++) {
                if (g_bd[ch].cmd_pts[i] != INIT_FREQ) {
                    memset(g_bd[ch].cmd_pts, 0, sizeof g_bd[ch].cmd_pts);
                    break;
                }
            }
            g_bd[ch].ri_sta = 1;
            g_bd[ch].stage  = BS_COREREGS;
        }

        efuse_scan_step();

        NEXT_BOARD();
    }

    asic_deinit();
    return 0;
}
