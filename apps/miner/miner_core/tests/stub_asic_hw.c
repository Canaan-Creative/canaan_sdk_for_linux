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
#include "osif.h"
#include <stdint.h>
#include <string.h>

#define STUB_PLL_MAX_BOARDS 8 /* Hash boards (UART channels) covered by the per-board stub arrays */

/* Entries kept in each call log; later calls are counted but not stored */
#define STUB_SEQ_MAX 2048

static void stub_op_set_pll(uint8_t m, uint16_t a, uint8_t u);
static void stub_op_read_efuse(uint8_t channel, uint32_t *data);

/* Sequence number shared by FIFO writes, core-register ops, PLL commits and work sends */
int stub_ord;

/* Sequence number of bring-up calls (UART init/baud/flush, mode switches, enumerate) */
static int s_bring_ord;

static uint32_t s_baud[STUB_PLL_MAX_BOARDS];           /* Current UART baud per board, bps */
int             stub_uart_init_n[STUB_PLL_MAX_BOARDS]; /* uart_init() calls per board */
uint32_t stub_uart_init_baud[STUB_PLL_MAX_BOARDS]; /* Baud of the last uart_init() per board, bps */
int      stub_baud_cmd_n[STUB_PLL_MAX_BOARDS];     /* uart_write_baud_cmd() calls per board */
/* Baud of the last chip baud command per board, bps */
uint32_t stub_baud_cmd_val[STUB_PLL_MAX_BOARDS];
/* Bring-up sequence number of the last chip baud command per board */
int stub_baud_cmd_ord[STUB_PLL_MAX_BOARDS];
int stub_cfg_baud_n[STUB_PLL_MAX_BOARDS]; /* uart_config_baud() calls per board */
/* Baud of the last uart_config_baud() per board, bps */
uint32_t stub_cfg_baud_val[STUB_PLL_MAX_BOARDS];
int      stub_rxflush_n[STUB_PLL_MAX_BOARDS];  /* uart_clear_rxfifo() calls per board */
int      stub_workmode_n[STUB_PLL_MAX_BOARDS]; /* a3197s_enter_work_mode() calls per board */
int      stub_cfgmode_n[STUB_PLL_MAX_BOARDS];  /* a3197s_enter_config_mode() calls per board */
/* Replaces uart_tx_empty() when set; NULL reports the TX FIFO empty */
int (*stub_tx_empty_hook)(uint8_t ch);
int     stub_ss_n;                   /* uart_write_fifo() calls, including those not logged */
uint8_t stub_ss_board[STUB_SEQ_MAX]; /* Board of each logged uart_write_fifo() call */
/* Chip selected at each logged write; 0xFFFF for an out-of-range board */
uint16_t stub_ss_chip[STUB_SEQ_MAX];
/* Register address and first data word of each logged write */
uint32_t        stub_ss_addr[STUB_SEQ_MAX], stub_ss_val[STUB_SEQ_MAX];
static uint16_t s_sel[STUB_PLL_MAX_BOARDS]; /* Chip address currently selected per board */
/* Writes of 0x00001c2a to A3197S_SS_CTRL_ADDR per board */
int stub_ss_repin_by_board[STUB_PLL_MAX_BOARDS];
/* Error count uart_get_errcnt() returns per board */
uint32_t stub_uart_errcnt[STUB_PLL_MAX_BOARDS];

int stub_config_asic_n[STUB_PLL_MAX_BOARDS]; /* a3197s_config_asic() calls per board */
/* Bring-up sequence number of the last a3197s_config_asic() per board */
int      stub_config_asic_ord[STUB_PLL_MAX_BOARDS];
int      stub_core_n;                   /* Core-register operations, including those not logged */
uint8_t  stub_core_board[STUB_SEQ_MAX]; /* Board of each logged core-register operation */
uint16_t stub_core_chip[STUB_SEQ_MAX];  /* Chip selected at each logged core-register operation */
/* Kind of each logged operation: 0 MX2 reset, 1 core-register config */
uint8_t stub_core_kind[STUB_SEQ_MAX];
int     stub_core_ord[STUB_SEQ_MAX]; /* stub_ord value at each logged core-register operation */
int     stub_nonce_arm_n[STUB_PLL_MAX_BOARDS]; /* a3197s_enable_nonce_return() calls per board */
/* a3197s_set_freq() calls; PLL commits through the set_pll op */
int stub_pll_freq_n, stub_pll_commit_n;

/* correct_freq() of the base point at the last set_freq, the first commit and the last commit */
uint32_t stub_pll_last_base, stub_pll_commit_base_first, stub_pll_commit_base_last;
int      stub_pll_commit_by_board[STUB_PLL_MAX_BOARDS]; /* PLL commits per board */
int      stub_seq_n;                   /* PLL commits, including those not logged */
uint8_t  stub_seq_board[STUB_SEQ_MAX]; /* Board of each logged PLL commit */
uint16_t stub_seq_chip[STUB_SEQ_MAX];  /* Chip address of each logged PLL commit */
/* Base frequency from stub_pll_last_base at each logged PLL commit */
uint32_t stub_seq_base[STUB_SEQ_MAX];
uint8_t  stub_seq_upd[STUB_SEQ_MAX]; /* Update flag of each logged PLL commit */
int      stub_enum_chips; /* Chip count a3197s_enumerate() returns when stub_enum_hook is NULL */
int      stub_enum_n;     /* a3197s_enumerate() calls on all boards */
int      stub_enum_n_by_board[STUB_PLL_MAX_BOARDS]; /* a3197s_enumerate() calls per board */
uint32_t stub_enum_baud_last; /* UART baud of the board at the last a3197s_enumerate(), bps */
/* Chip count for a3197s_enumerate() when set; try_n counts earlier calls on that board */
int (*stub_enum_hook)(uint8_t ch, int try_n);

/* Replaces a3197s_read_reg() when set; NULL reads all zeros */
int (*stub_read_reg_hook)(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n);
uint32_t (*stub_cpm_freq_hook)(uint32_t pll); /* Replaces get_cpm_freq() when set; NULL returns 0 */

const double stub_pvt_temp_c = 50.0; /* Temperature a3197s_fetch_pvt_temp() reports, degrees C */
int          stub_pvt_select_n[STUB_PLL_MAX_BOARDS]; /* a3197s_pvt_select_mode() calls per board */

/* eFuse words the read_efuse op returns; word 0 becomes channel << 16 | selected chip */
uint32_t stub_efuse_data[16];
int      stub_efuse_read_n;

/* ASIC ops returned by hal_asic(): set_pll and read_efuse only */
static const hal_asic_ops_t s_ops = {
    .set_pll    = stub_op_set_pll,
    .read_efuse = stub_op_read_efuse,
};

/* Version words avalon_build_work() copies into every work */
uint32_t stub_work_version[TOAST_WORK_VERSION_WORDS];
int      stub_work_n;                   /* avalon_send_work() calls, including those not logged */
uint8_t  stub_work_board[STUB_SEQ_MAX]; /* Board of each logged work send */
int      stub_work_ord[STUB_SEQ_MAX];   /* stub_ord value at each logged work send */
uint16_t stub_work_chip[STUB_SEQ_MAX];  /* Chip selected at each logged work send */
int      stub_target_n;                 /* avalon_set_target() calls */
int      stub_target_fail;              /* Non-zero makes avalon_set_target() return HAL_ERR */
/* Chip selected at the last avalon_set_target() per board */
uint16_t stub_target_chip[STUB_PLL_MAX_BOARDS];
/* Supplies avalon_collect_nonces() results when set; call_n counts earlier calls */
int (*stub_collect_hook)(uint8_t ch, int call_n, avalon_nonce_out *out, int max);
int stub_collect_n;     /* avalon_collect_nonces() calls */
int stub_genwork_ok;    /* Non-zero makes mc_gen_chip_work() succeed; zero makes it return -1 */
int stub_default_job_n; /* mc_default_job() calls */

int      stub_genwork_n;                 /* mc_gen_chip_work() calls */
uint32_t stub_genwork_seq[STUB_SEQ_MAX]; /* Template seq of each logged mc_gen_chip_work() call */
uint32_t stub_genwork_n2[STUB_SEQ_MAX];  /* Extranonce2 of each logged mc_gen_chip_work() call */

uint32_t stub_correct_snap; /* Amount correct_freq() subtracts from any frequency above it */

void uart_init(uint8_t ch, uint32_t baud)
{
    if (ch < STUB_PLL_MAX_BOARDS) {
        stub_uart_init_n[ch]++;
        stub_uart_init_baud[ch] = baud;
        s_baud[ch]              = baud;
    }
    s_bring_ord++;
}
void uart_config_baud(uint8_t ch, uint32_t baud)
{
    if (ch < STUB_PLL_MAX_BOARDS) {
        stub_cfg_baud_n[ch]++;
        stub_cfg_baud_val[ch] = baud;
        s_baud[ch]            = baud;
    }
    s_bring_ord++;
}
void uart_clear_rxfifo(uint8_t ch)
{
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_rxflush_n[ch]++;
    s_bring_ord++;
}
int uart_tx_empty(uint8_t ch)
{
    return stub_tx_empty_hook ? stub_tx_empty_hook(ch) : 1;
}
void uart_write_baud_cmd(uint8_t ch, uint32_t baud)
{
    if (ch < STUB_PLL_MAX_BOARDS) {
        stub_baud_cmd_n[ch]++;
        stub_baud_cmd_val[ch] = baud;
        stub_baud_cmd_ord[ch] = s_bring_ord;
    }
    s_bring_ord++;
}

void uart_chip_select(uint8_t ch, uint16_t addr)
{
    if (ch < STUB_PLL_MAX_BOARDS)
        s_sel[ch] = addr;
}
uint16_t uart_get_chip_select(uint8_t ch)
{
    return (ch < STUB_PLL_MAX_BOARDS) ? s_sel[ch] : 0;
}

int uart_write_fifo(uint8_t ch, uint32_t addr, uint32_t *d, uint32_t n, enum uart_mode mode)
{
    (void)mode;
    if (stub_ss_n < STUB_SEQ_MAX) {
        stub_ss_board[stub_ss_n] = ch;
        stub_ss_chip[stub_ss_n]  = ch < STUB_PLL_MAX_BOARDS ? s_sel[ch] : 0xFFFFu;
        stub_ss_addr[stub_ss_n]  = addr;
        stub_ss_val[stub_ss_n]   = (d && n) ? d[0] : 0u;
    }
    stub_ss_n++;
    if (addr == A3197S_SS_CTRL_ADDR && d && n && d[0] == 0x00001c2au && ch < STUB_PLL_MAX_BOARDS)
        stub_ss_repin_by_board[ch]++;
    stub_ord++;
    return 0;
}
uint32_t uart_get_errcnt(uint8_t ch)
{
    return ch < STUB_PLL_MAX_BOARDS ? stub_uart_errcnt[ch] : 0;
}
void uart_clear_errcnt(uint8_t ch)
{
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_uart_errcnt[ch] = 0;
}
void a3197s_config_asic(uint8_t ch)
{
    if (ch < STUB_PLL_MAX_BOARDS) {
        stub_config_asic_n[ch]++;
        stub_config_asic_ord[ch] = s_bring_ord;
    }
    s_bring_ord++;
}

static void stub_core_rec(uint8_t ch, uint8_t kind)
{
    if (stub_core_n < STUB_SEQ_MAX) {
        stub_core_board[stub_core_n] = ch;
        stub_core_chip[stub_core_n]  = ch < STUB_PLL_MAX_BOARDS ? s_sel[ch] : 0xFFFFu;
        stub_core_kind[stub_core_n]  = kind;
        stub_core_ord[stub_core_n]   = stub_ord;
    }
    stub_core_n++;
    stub_ord++;
}
void a3197s_config_coreregs(uint8_t ch)
{
    stub_core_rec(ch, 1);
}
void a3197s_mx2_reset(uint8_t ch)
{
    stub_core_rec(ch, 0);
}
void a3197s_enter_config_mode(uint8_t ch)
{
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_cfgmode_n[ch]++;
    s_bring_ord++;
}
void a3197s_enter_work_mode(uint8_t ch)
{
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_workmode_n[ch]++;
    s_bring_ord++;
}
void a3197s_enable_nonce_return(uint8_t ch)
{
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_nonce_arm_n[ch]++;
}
void a3197s_write_version(uint8_t ch, const uint32_t v[8])
{
    (void)ch;
    (void)v;
}

void a3197s_set_freq(const uint16_t f[A3197S_PLL_POINTS])
{
    stub_pll_last_base = correct_freq(f[0]);
    stub_pll_freq_n++;
}

static void stub_op_set_pll(uint8_t m, uint16_t a, uint8_t u)
{
    if (stub_pll_commit_n == 0)
        stub_pll_commit_base_first = stub_pll_last_base;
    stub_pll_commit_base_last = stub_pll_last_base;
    if (m < STUB_PLL_MAX_BOARDS)
        stub_pll_commit_by_board[m]++;
    if (stub_seq_n < STUB_SEQ_MAX) {
        stub_seq_board[stub_seq_n] = m;
        stub_seq_chip[stub_seq_n]  = a;
        stub_seq_base[stub_seq_n]  = stub_pll_last_base;
        stub_seq_upd[stub_seq_n]   = u;
    }
    stub_seq_n++;
    stub_pll_commit_n++;
    stub_ord++;
}

uint32_t a3197s_enumerate(uint8_t ch)
{
    int try_n = ch < STUB_PLL_MAX_BOARDS ? stub_enum_n_by_board[ch] : 0;
    stub_enum_n++;
    stub_enum_baud_last = ch < STUB_PLL_MAX_BOARDS ? s_baud[ch] : 0;
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_enum_n_by_board[ch]++;
    s_bring_ord++;
    return stub_enum_hook ? (uint32_t)stub_enum_hook(ch, try_n) : (uint32_t)stub_enum_chips;
}

uint32_t get_cpm_freq(uint32_t pll)
{
    return stub_cpm_freq_hook ? stub_cpm_freq_hook(pll) : 0;
}

int a3197s_read_reg(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    if (stub_read_reg_hook)
        return stub_read_reg_hook(ch, addr, out, n);
    (void)ch;
    (void)addr;
    if (out)
        memset(out, 0, (size_t)n * sizeof *out);
    return 0;
}
int a3197s_pvt_select_mode(uint8_t ch, int temp_mode, uint16_t chip)
{
    (void)temp_mode;
    (void)chip;
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_pvt_select_n[ch]++;
    return 0;
}
void a3197s_fetch_pvt_temp(uint8_t ch, double *degc)
{
    (void)ch;
    if (degc)
        *degc = stub_pvt_temp_c;
}
void a3197s_fetch_pvt_volt(uint8_t ch, uint16_t *mv)
{
    (void)ch;
    if (mv)
        *mv = 800;
}
static void stub_op_read_efuse(uint8_t channel, uint32_t *data)
{
    stub_efuse_read_n++;
    memcpy(data, stub_efuse_data, sizeof stub_efuse_data);
    data[0] = ((uint32_t)channel << 16) | uart_get_chip_select(channel);
}
const hal_asic_ops_t *hal_asic(void)
{
    return &s_ops;
}

void avalon_build_work(const avalon_job_in *in, struct toast_work *w)
{
    (void)in;
    memcpy(w->version, stub_work_version, sizeof w->version);
}

int avalon_send_work(uint8_t ch, struct toast_work *w)
{
    (void)w;
    if (stub_work_n < STUB_SEQ_MAX) {
        stub_work_board[stub_work_n] = ch;
        stub_work_ord[stub_work_n]   = stub_ord;
        stub_work_chip[stub_work_n]  = uart_get_chip_select(ch);
    }
    stub_work_n++;
    stub_ord++;
    return HAL_OK;
}

int avalon_set_target(uint8_t ch, uint32_t th, uint32_t tl)
{
    (void)th;
    (void)tl;
    if (ch < STUB_PLL_MAX_BOARDS)
        stub_target_chip[ch] = uart_get_chip_select(ch);
    stub_target_n++;
    return stub_target_fail ? HAL_ERR : HAL_OK;
}

int avalon_collect_nonces(uint8_t ch, avalon_nonce_out *out, uint8_t max)
{
    int n = stub_collect_n++;
    if (!stub_collect_hook)
        return 0;
    return stub_collect_hook(ch, n, out, (int)max);
}
void mc_default_job(mc_asic_job *out)
{
    stub_default_job_n++;
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    out->seq = STATIC_WORK_JOB_ID;
}

int mc_gen_chip_work(const mc_asic_job *tpl, uint32_t n2, avalon_job_in *out)
{
    if (stub_genwork_n < STUB_SEQ_MAX) {
        stub_genwork_seq[stub_genwork_n] = tpl->seq;
        stub_genwork_n2[stub_genwork_n]  = n2;
    }
    stub_genwork_n++;
    if (!stub_genwork_ok)
        return -1;
    if (out)
        memset(out, 0, sizeof *out);
    return 0;
}
uint32_t correct_freq(uint32_t freq)
{
    return freq > stub_correct_snap ? freq - stub_correct_snap : freq;
}

int osif_log_op_sink_open(void)
{
    return -1;
}
