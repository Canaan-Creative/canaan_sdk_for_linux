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

#include "asic_internal.h"
#include "a3197s_ops.h"
#include "a3197s_pll_seq.h"
#include "a3197s_decode.h"
#include "uart_pro.h"
#include "osif.h"

#include <string.h>

#define SS_TIMER_VALID_VALUE   (0x30000u + 3000u) /* SPDLOG_TIMER: window complete */
#define SS_TIMER_DEFAULT_VALUE (0x10000u + 3000u) /* SPDLOG_TIMER: new window */
#define SPDLOG_FAIL_TIMES      30u                /* unusable speed-log reads before reset */
#define SEND_DROP_CORE_TELEM   (1u << 0)
#define SEND_DROP_CORE_VER     (1u << 1)
#define SEND_DROP_MM_TELEM     (1u << 2)

/* status read kind: errors, PLL core counts, speed log, voltage, temperature */
enum { RS_ECC = 0, RS_SPDCNT, RS_SPDLOG, RS_VSENSOR, RS_TSENSOR, RS_TYPES };

/* every chip has a temperature read */
static uint8_t g_pvt_lapped[HBOARD_COUNT];

static uint16_t g_chip_volt[HBOARD_COUNT][MINER_MAX_CHIPS]; /* mV */
/* cores per PLL point */
static uint8_t g_chip_pllcnt[HBOARD_COUNT][MINER_MAX_CHIPS][ASIC_PLL_POINTS];
/* measured PLL point frequency, MHz */
static uint16_t g_chip_cf[HBOARD_COUNT][MINER_MAX_CHIPS][ASIC_PLL_POINTS];
/* speed-log pass/fail counts for realtime DH */
static uint32_t g_chip_pass[HBOARD_COUNT][MINER_MAX_CHIPS];
static uint32_t g_chip_fail[HBOARD_COUNT][MINER_MAX_CHIPS];
/* speed-log pass/fail counts of the last completed window */
static uint32_t g_chip_pass_bak[HBOARD_COUNT][MINER_MAX_CHIPS];
static uint32_t g_chip_fail_bak[HBOARD_COUNT][MINER_MAX_CHIPS];
/* chip temperature, degrees C */
static double g_chip_temp[HBOARD_COUNT][MINER_MAX_CHIPS];
/* chip errors plus UART failures since bring-up */
static uint32_t g_board_err_crc[HBOARD_COUNT];
/* UART failures since bring-up */
static uint32_t g_board_com_crc[HBOARD_COUNT];
/* consecutive unusable speed-log reads */
static uint32_t g_spd_failcnt[HBOARD_COUNT][MINER_MAX_CHIPS];

/* MH/s from speed-log pass counts */
static uint32_t g_mhsspd;
/* nominal MH/s from cores and PLL frequency */
static uint32_t g_mhsmm_win;

/* status read cursor */
static struct {
    uint8_t type, chip, board, vt_selected;
} g_rs;
static uint8_t g_send_dropping;

void asic_telem_init(void)
{
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        for (int i = 0; i < MINER_MAX_CHIPS; i++)
            g_chip_temp[bd][i] = ASIC_TEMP_NONE;
}

void asic_telem_board_reset(uint8_t ch)
{
    g_board_err_crc[ch] = 0;
    g_board_com_crc[ch] = 0;
    memset(g_spd_failcnt[ch], 0, sizeof g_spd_failcnt[ch]);
}

static void spdlog_rearm(uint8_t ch)
{
    uint32_t rearm = SS_TIMER_DEFAULT_VALUE;
    uart_write_fifo(ch, A3197S_SPDLOG_TIMER_ADDR, &rearm, 1, UART_WRITE_MODE);
}

static void next_rminer(void)
{
    g_rs.board = (uint8_t)((g_rs.board + 1) % HBOARD_COUNT);
    if (g_rs.board == 0)
        g_rs.type = (uint8_t)((g_rs.type + 1) % RS_TYPES);
}

void asic_collect_status(void)
{
    if (g_bd[g_rs.board].chips_n <= 0) {
        next_rminer();
        return;
    }
    const uint8_t ch    = g_rs.board;
    const int     chips = g_bd[ch].chips_n;
    if (!uart_tx_empty(ch))
        return;
    if (g_rs.chip >= chips)
        g_rs.chip = 0;
    const uint8_t c = g_rs.chip;
    uart_chip_select(ch, (uint16_t)c);

    switch (g_rs.type) {
    case RS_ECC: {
        uint32_t ev[7] = { 0 };
        if (a3197s_read_reg(ch, A3197S_UART_TIMEOUT_ADDR, ev, 7) == 0) {
            uint32_t asic_add = (ev[0] & 0xffu) + ((ev[0] >> 8) & 0xfffu) + ev[4] +
                    (ev[5] & 0xffffffu) + (ev[6] & 0xffffffu);
            g_board_err_crc[ch] += asic_add;
            if (asic_add) {
                osif_log(OSIF_LOG_ERR,
                        "CRC m:%d a:%d fsm_to:%u di_to:%u check_fail:%u start:%u glitch:%u", ch, c,
                        ev[0] & 0xffu, (ev[0] >> 8) & 0xfffu, ev[4], ev[5] & 0xffffffu,
                        ev[6] & 0xffffffu);
            }
            uint32_t ones[7] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                0xffffffffu, 0xffffffffu };
            uart_write_fifo(ch, A3197S_UART_TIMEOUT_ADDR, ones, 7, UART_WRITE_MODE);
        }
        if (c == 0) {
            uint32_t e = uart_get_errcnt(ch);
            g_board_err_crc[ch] += e;
            g_board_com_crc[ch] += e;
            uart_clear_errcnt(ch);
        }
        uint32_t ri = 0;
        a3197s_read_reg(ch, A3197S_REGRI_ADDR, &ri, 1);
        if (ri)
            osif_log(OSIF_LOG_ERR, "ri error m:%d a:%03d", ch, c);
        break;
    }
    case RS_SPDCNT: {
        uint32_t c1 = 0, chkdrop = 0, w[ASIC_PLL_POINTS] = { 0, 0, 0, 0 };
        a3197s_read_reg(ch, A3197S_PLLCNT1_ADDR, &c1, 1);
        a3197s_read_reg(ch, A3197S_CHKDROP_ADDR, &chkdrop, 1);
        a3197s_read_reg(ch, A3197S_PLL0L1_ADDR, &w[0], 1);
        a3197s_read_reg(ch, A3197S_PLL1L1_ADDR, &w[1], 1);
        a3197s_read_reg(ch, A3197S_PLL2L1_ADDR, &w[2], 1);
        a3197s_read_reg(ch, A3197S_PLL3L1_ADDR, &w[3], 1);
        for (int k = 0; k < ASIC_PLL_POINTS; k++) {
            g_chip_pllcnt[ch][c][k] = (uint8_t)((c1 >> (8 * k)) & 0xff);
            g_chip_cf[ch][c][k]     = (uint16_t)get_cpm_freq(w[k]);
        }
        break;
    }
    case RS_SPDLOG: {
        int      win_latched = 0;
        uint32_t tmr         = 0;
        a3197s_read_reg(ch, A3197S_SPDLOG_TIMER_ADDR, &tmr, 1);
        if (tmr == SS_TIMER_VALID_VALUE) {
            uint32_t pv = 0;
            a3197s_read_reg(ch, A3197S_SPDLOG_PASSCNT_ADDR, &pv, 1);
            if (pv == 0) {
                g_spd_failcnt[ch][c]++;
            } else {
                uint32_t fv = 0;
                a3197s_read_reg(ch, A3197S_SPDLOG_FAILCNT_ADDR, &fv, 1);
                g_spd_failcnt[ch][c]   = 0;
                g_chip_pass[ch][c]     = pv;
                g_chip_fail[ch][c]     = fv;
                g_chip_pass_bak[ch][c] = pv;
                g_chip_fail_bak[ch][c] = fv;
            }

            a3197s_config_coreregs(ch);
            spdlog_rearm(ch);
            if (c == chips - 1)
                win_latched = 1;
        } else {
            if (tmr != SS_TIMER_DEFAULT_VALUE)
                g_spd_failcnt[ch][c]++;
            uint32_t pv = 0, fv = 0;
            a3197s_read_reg(ch, A3197S_SPDLOG_PASSCNT_ADDR, &pv, 1);
            a3197s_read_reg(ch, A3197S_SPDLOG_FAILCNT_ADDR, &fv, 1);
            if (pv + fv > 100000u) {
                g_chip_pass[ch][c] = pv;
                g_chip_fail[ch][c] = fv;
            }
        }
        if (g_spd_failcnt[ch][c] > SPDLOG_FAIL_TIMES) {
            g_spd_failcnt[ch][c]   = 0;
            g_chip_pass[ch][c]     = 0;
            g_chip_fail[ch][c]     = 0;
            g_chip_pass_bak[ch][c] = 0;
            g_chip_fail_bak[ch][c] = 0;
            spdlog_rearm(ch);
        }
        if (win_latched) {
            double pass_sum = 0.0, num = 0.0;
            for (int bd = 0; bd < HBOARD_COUNT; bd++) {
                for (int cc = 0; cc < g_bd[bd].chips_n; cc++) {
                    pass_sum += (double)g_chip_pass_bak[bd][cc];
                    for (int k = 0; k < ASIC_PLL_POINTS; k++)
                        num += (double)g_chip_pllcnt[bd][cc][k] * g_chip_cf[bd][cc][k];
                }
            }
            g_mhsspd    = (uint32_t)(pass_sum * 0.256);
            g_mhsmm_win = (uint32_t)(num * 8.0); /* 8 AB cores per big core */
        }
        break;
    }
    case RS_VSENSOR:
    case RS_TSENSOR: {
        if (c == 0 && !g_rs.vt_selected) {
            a3197s_pvt_select_mode(ch, g_rs.type == RS_TSENSOR, (uint16_t)c);
            g_rs.vt_selected = 1;
            return;
        }
        if (g_rs.type == RS_TSENSOR) {
            double t;
            a3197s_fetch_pvt_temp(ch, &t);
            g_chip_temp[ch][c] = t;
        } else {
            uint16_t mv;
            a3197s_fetch_pvt_volt(ch, &mv);
            g_chip_volt[ch][c] = mv;
        }
        break;
    }
    default:
        break;
    }

    g_rs.chip = (uint8_t)((g_rs.chip + 1) % chips);
    if (g_rs.chip == 0) {
        g_rs.vt_selected = 0;
        if (g_rs.type == RS_TSENSOR)
            g_pvt_lapped[ch] = 1;
        next_rminer();
    }
}

void asic_send_info(uint8_t ch)
{
    asic_chip_telem ct;
    memset(&ct, 0, sizeof ct);
    ct.boards = HBOARD_COUNT;

    int    chips_all = 0;
    double rt_pass = 0.0, rt_fail = 0.0;
    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        ct.err_crc[bd]    = g_board_err_crc[bd];
        ct.com_crc[bd]    = g_board_com_crc[bd];
        ct.local_work[bd] = g_bd[bd].local_work;

        int nc = g_bd[bd].chips_n;
        if (nc <= 0)
            continue;
        if (g_bd[bd].stage == BS_RUN)
            ct.tm.in_work = 1;
        ct.chips[bd] = (uint16_t)nc;
        chips_all += nc;
        ct.pvt_lapped[bd] = g_pvt_lapped[bd];
        for (int c = 0; c < nc; c++) {
            ct.pvt_t[bd][c] = (int16_t)(g_chip_temp[bd][c] <= ASIC_TEMP_NONE ? ASIC_TEMP_NONE :
                                                                               g_chip_temp[bd][c]);
            ct.pvt_v[bd][c] = g_chip_volt[bd][c];
            ct.spdlog_pass[bd][c] = g_chip_pass_bak[bd][c];
            ct.spdlog_fail[bd][c] = g_chip_fail_bak[bd][c];
            if (g_chip_pass[bd][c]) {
                rt_pass += (double)g_chip_pass[bd][c];
                rt_fail += (double)g_chip_fail[bd][c];
            }
            for (int k = 0; k < ASIC_PLL_POINTS; k++) {
                ct.pllcnt[bd][c][k] = g_chip_pllcnt[bd][c][k];
                ct.cf[bd][c][k]     = g_chip_cf[bd][c][k];
            }
        }
    }

    /* 8 AB cores per big core */
    ct.tm.mhs_mm            = g_mhsmm_win ?
                       g_mhsmm_win :
                       ((uint32_t)chips_all * A3197S_CORE_COUNT * g_bd[ch].work_freq * 8u);
    ct.tm.freq_mhz          = (uint16_t)(chips_all > 0 ?
                             ct.tm.mhs_mm / ((uint32_t)chips_all * A3197S_CORE_COUNT * 8u) :
                             g_bd[ch].work_freq);
    ct.tm.ghs_spd_mhs       = g_mhsspd;
    double dh               = rt_pass ? rt_fail / (rt_pass + rt_fail) * 100.0 : 100.0;
    ct.tm.realtime_dh_x1000 = (uint32_t)(dh * 1000.0 + 0.5);

    ct.tm.temp_max = ASIC_TEMP_NONE;
    ct.tm.temp_avg = ASIC_TEMP_NONE;

    ct.tm.bin_grade = g_bin_grade;

    ct.nonce_discarded          = g_nring.discarded;
    unsigned int      dropped   = 0;
    static const char version[] = ASIC_MINER_VERSION;
    if (osif_ipc_send(g_ipc_asic_core, MSG_ASIC_TELEM, &ct, (int)sizeof ct) != OSIF_OK)
        dropped |= SEND_DROP_CORE_TELEM;
    if (osif_ipc_send(g_ipc_asic_core, MSG_ASIC_VERSION, version, sizeof version) != OSIF_OK)
        dropped |= SEND_DROP_CORE_VER;
    if (osif_ipc_send(g_ipc_asic_mm, MSG_ASIC_TELEM, &ct, (int)sizeof ct) != OSIF_OK)
        dropped |= SEND_DROP_MM_TELEM;
    if (dropped && !g_send_dropping) {
        osif_log(OSIF_LOG_ERR, "status update dropped:%s%s%s",
                dropped & SEND_DROP_CORE_TELEM ? " telem->core_miner" : "",
                dropped & SEND_DROP_CORE_VER ? " version->core_miner" : "",
                dropped & SEND_DROP_MM_TELEM ? " telem->mm_miner" : "");
        g_send_dropping = 1;
    } else if (!dropped && g_send_dropping) {
        osif_log(OSIF_LOG_WARN, "status update resumed");
        g_send_dropping = 0;
    }
}
