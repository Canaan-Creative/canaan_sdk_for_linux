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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osif.h"
#include "boardconf.h"
#include "miner_ipc.h"

#include "../../tests/include/tst_check.h"

#define TST_REC_MAX 1024 /* eFuse records kept by the osif_ipc_send() stub */

#define SIM_ENUM_RETRY_MAX 30u /* Enumeration tries per absent board in the bring-up time budget */
#define SIM_ENUM_TRY_MS    1170u /* Time per enumeration try in the bring-up time budget, ms */

#define RI_RESET_PULSE_MS 1000u /* Mode toggle after a failed enumeration, ms (2 x 500 ms) */
#define PRE_PULSE_MS      2u    /* Mode toggle before the first enumeration, ms (2 x 1 ms) */
#define BAUD_SWITCH_MS    60u   /* Sleeps around the switch to the mining baud, ms (2 x 30 ms) */

#define TST_SEL_PARKED 0x5a /* Chip address selected on every board before a scan */

extern int      stub_enum_chips, stub_enum_n; /* Stub enumerate chip count and call count */
extern int      stub_enum_n_by_board[];       /* Stub enumerate calls per board */
extern uint32_t stub_enum_baud_last;          /* UART baud at the last stub enumerate, bps */
/* Chip count for the stub enumerate when set */
extern int (*stub_enum_hook)(uint8_t ch, int try_n);
extern int      stub_uart_init_n[];    /* uart_init() calls per board */
extern uint32_t stub_uart_init_baud[]; /* Baud of the last uart_init() per board, bps */
/* Chip baud commands per board; bring-up order of the last one */
extern int      stub_baud_cmd_n[], stub_baud_cmd_ord[];
extern uint32_t stub_baud_cmd_val[]; /* Last chip baud command per board, bps */
extern int      stub_cfg_baud_n[];   /* uart_config_baud() calls per board */
extern uint32_t stub_cfg_baud_val[]; /* Baud of the last uart_config_baud() per board, bps */
/* RX FIFO flushes, work-mode entries and config-mode entries per board */
extern int stub_rxflush_n[], stub_workmode_n[], stub_cfgmode_n[];
/* a3197s_config_asic() calls per board; bring-up order of the last one */
extern int      stub_config_asic_n[], stub_config_asic_ord[];
extern int      stub_pll_commit_by_board[]; /* PLL commits per board */
extern int      stub_pll_commit_n;          /* PLL commits on all boards */
extern uint32_t stub_efuse_data[16];        /* eFuse words the stub read_efuse op returns */
extern int      stub_efuse_read_n;          /* Stub read_efuse op calls */

static uint64_t tst_now_ms, tst_slept_ms; /* Simulated clock and total time slept, ms */

static int s_ipc_init_n; /* osif_ipc_init() calls */

/* Event clock; deinit and last-record events; scans; BS_RI boards at scan start; telem bin */
static int            s_ev, s_deinit_ev, s_last_ev, s_scan_n, s_scan_ri, s_telem_bin;
static asic_efuse_rec s_rec[TST_REC_MAX]; /* eFuse records sent, in order */
/* eFuse records sent, including those not kept; sends left to refuse */
static int s_rec_n, s_send_refuse;

/* Stop after bring-up; sleep cap; sleeps; sleep that requests / presents the MM eFuse */
static int     s_stop_after_bringup, s_sleep_cap, s_sleeps, s_req_at, s_present_at;
static uint8_t s_present_bin; /* Bin grade in the presented MM eFuse state */

static int s_answer_on_try; /* First try on which enum_answer_on_try() finds the full chain */
static int s_dead_boards;   /* Boards from 0 up that enum_dead_low() reports empty */
static int s_bogus_tries;   /* Tries on which enum_bogus_then_good() reports one chip too many */

static int s_stop_try; /* Try on which enum_stop_on_try() stops the main loop */

static void tst_on_sleep(uint32_t ms);
uint64_t    osif_now_ms(void)
{
    return tst_now_ms;
}
void osif_sleep_ms(uint32_t ms)
{
    tst_slept_ms += ms;
    tst_now_ms += ms;
    tst_on_sleep(ms);
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}
int fan_dev_get_rpm(int fan)
{
    (void)fan;
    return 0;
}
static void   tst_stop_loop(void);
osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    s_ipc_init_n++;
    return OSIF_OK;
}
static int tst_boards_in_ri(void);
void       osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
    if (!s_deinit_ev)
        s_deinit_ev = ++s_ev;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    asic_efuse_rec r;

    (void)h;
    if (type == MSG_ASIC_TELEM && len == (int)sizeof(asic_chip_telem)) {
        asic_chip_telem ct;

        memcpy(&ct, data, sizeof ct);
        s_telem_bin = ct.tm.bin_grade;
        return OSIF_OK;
    }
    if (type != MSG_ASIC_EFUSE || len != (int)sizeof r)
        return OSIF_OK;
    if (s_send_refuse) {
        s_send_refuse--;
        return OSIF_ERR;
    }
    memcpy(&r, data, sizeof r);
    if (s_rec_n < TST_REC_MAX)
        s_rec[s_rec_n] = r;
    s_rec_n++;
    if (!r.last && r.seq == 0) {
        s_scan_n++;
        s_scan_ri = tst_boards_in_ri();
    }
    if (r.last) {
        s_last_ev = ++s_ev;
        tst_stop_loop();
    }
    return OSIF_OK;
}
osif_status_t osif_lock_create(osif_lock_t **l)
{
    if (l)
        *l = NULL;
    return OSIF_OK;
}
void osif_lock_acquire(osif_lock_t *l)
{
    (void)l;
}
void osif_lock_release(osif_lock_t *l)
{
    (void)l;
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static void tst_stop_loop(void)
{
    g_run = 0;
}

static int tst_boards_in_ri(void)
{
    int n = 0;

    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        if (g_bd[bd].chips_n > 0 && g_bd[bd].stage == BS_RI)
            n++;
    return n;
}

static void tst_mm_efuse(int present, uint8_t bin)
{
    mm_efuse_state st;

    memset(&st, 0, sizeof st);
    st.present   = (uint8_t)present;
    st.bin_grade = bin;
    ipc_cb(MSG_MM_EFUSE, &st, (int)sizeof st);
}
static void tst_on_sleep(uint32_t ms)
{
    (void)ms;
    if (s_stop_after_bringup && g_bd[HBOARD_COUNT - 1].chips_n >= 0 &&
            g_bd[HBOARD_COUNT - 1].chips_n <= (int)MINER_MAX_CHIPS)
        g_run = 0;
    if (s_sleep_cap && ++s_sleeps >= s_sleep_cap)
        g_run = 0;
    if (s_req_at && s_sleeps == s_req_at)
        tst_mm_efuse(0, 0);
    if (s_present_at && s_sleeps == s_present_at)
        tst_mm_efuse(1, s_present_bin);
}

static void reset_exit_trace(void)
{
    s_ev = s_deinit_ev = s_last_ev = s_scan_n = s_rec_n = 0;
    s_scan_ri                                           = -1;
    s_telem_bin                                         = -1;
    s_sleeps                                            = 0;
    stub_efuse_read_n                                   = 0;
    g_efuse_req                                         = 0;
    g_efuse_scan                                        = EFUSE_SCAN_IDLE;
    g_efuse_retry_not_before                            = 0;
    memset(&g_efuse_rec, 0, sizeof g_efuse_rec);
}

static void reset_all(void)
{
    stub_enum_n         = 0;
    stub_enum_baud_last = 0;
    memset(stub_enum_n_by_board, 0, sizeof(int) * 8);
    memset(stub_uart_init_n, 0, sizeof(int) * 8);
    memset(stub_uart_init_baud, 0, sizeof(uint32_t) * 8);
    memset(stub_baud_cmd_n, 0, sizeof(int) * 8);
    memset(stub_baud_cmd_ord, 0, sizeof(int) * 8);
    memset(stub_baud_cmd_val, 0, sizeof(uint32_t) * 8);
    memset(stub_cfg_baud_n, 0, sizeof(int) * 8);
    memset(stub_cfg_baud_val, 0, sizeof(uint32_t) * 8);
    memset(stub_rxflush_n, 0, sizeof(int) * 8);
    memset(stub_workmode_n, 0, sizeof(int) * 8);
    memset(stub_cfgmode_n, 0, sizeof(int) * 8);
    memset(stub_config_asic_n, 0, sizeof(int) * 8);
    memset(stub_config_asic_ord, 0, sizeof(int) * 8);
    memset(stub_pll_commit_by_board, 0, sizeof(int) * 8);
    stub_pll_commit_n = 0;
    tst_slept_ms      = 0;
}

static int enum_answer_on_try(uint8_t ch, int try_n)
{
    (void)ch;
    return try_n >= s_answer_on_try ? (int)MINER_MAX_CHIPS : 0;
}

static int enum_dead_low(uint8_t ch, int try_n)
{
    (void)try_n;
    return (int)ch < s_dead_boards ? 0 : (int)MINER_MAX_CHIPS;
}

static int enum_bogus_then_good(uint8_t ch, int try_n)
{
    (void)ch;
    return try_n < s_bogus_tries ? (int)MINER_MAX_CHIPS + 1 : (int)MINER_MAX_CHIPS;
}

static int enum_stop_on_try(uint8_t ch, int try_n)
{
    (void)ch;
    if (try_n == s_stop_try)
        g_run = 0;
    return 0;
}

static void run_to_efuse_start(void)
{
    g_pll_at_target = 1;
    reset_exit_trace();
    s_sleep_cap = 100000;
    g_run       = 1;
}

static void set_boards(const int *chips, int stage)
{
    reset_exit_trace();
    g_run = 1;
    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        g_bd[bd].chips_n = chips[bd];
        g_bd[bd].stage   = (uint8_t)stage;
        uart_chip_select((uint8_t)bd, TST_SEL_PARKED);
    }
}

static void scan_steps(int n)
{
    for (int i = 0; i < n && g_efuse_scan != EFUSE_SCAN_DONE; i++) {
        efuse_scan_step();
        tst_now_ms += EFUSE_SEND_RETRY_MIN_MS;
    }
}

static void scan_boards(const int *chips)
{
    set_boards(chips, BS_RUN);
    tst_mm_efuse(0, 0);
    scan_steps(TST_REC_MAX);
}

int main(void)
{
    printf("descriptor %s: %d board(s), %d chip(s)/board, ENUM_RETRY_MAX %d, "
           "enum baud %u -> mining baud %u\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, (int)MINER_MAX_CHIPS, ENUM_RETRY_MAX,
            (unsigned)UART_DEFAULT_BAUD_RATE, (unsigned)UART_HIGH_BAUD_RATE);

    CHECK(ENUM_RETRY_MAX == (int)SIM_ENUM_RETRY_MAX,
            "the bring-up margin is priced at %u tries per absent board and the "
            "loop runs %d",
            SIM_ENUM_RETRY_MAX, ENUM_RETRY_MAX);
    CHECK(SIM_ENUM_TRY_MS >= RI_RESET_PULSE_MS,
            "...at %u ms each, which must not be UNDER the %u ms of RI reset the "
            "try actually sleeps — an optimistic term in a watchdog budget",
            SIM_ENUM_TRY_MS, RI_RESET_PULSE_MS);

    reset_all();
    stub_enum_hook  = enum_answer_on_try;
    s_answer_on_try = 0;
    {
        int n = asic_bringup(0);
        CHECK(n == (int)MINER_MAX_CHIPS,
                "a board that answers reports its chain (%d chips, want %d)", n,
                (int)MINER_MAX_CHIPS);
        CHECK(stub_enum_n_by_board[0] == 1, "...after exactly one enumeration (got %d)",
                stub_enum_n_by_board[0]);
        CHECK(stub_enum_baud_last == (uint32_t)UART_DEFAULT_BAUD_RATE,
                "...taken at the ENUM baud %u, not the mining baud (got %u)",
                (unsigned)UART_DEFAULT_BAUD_RATE, stub_enum_baud_last);
        CHECK(tst_slept_ms == PRE_PULSE_MS + BAUD_SWITCH_MS,
                "...paying only the %u ms RI glitch + %u ms baud settle and NO "
                "retry (got %llu ms)",
                PRE_PULSE_MS, BAUD_SWITCH_MS, (unsigned long long)tst_slept_ms);
        CHECK(stub_workmode_n[0] == 1 && stub_cfgmode_n[0] == 1,
                "...one RI pulse, ending in CONFIG mode for register access "
                "(work %d, config %d)",
                stub_workmode_n[0], stub_cfgmode_n[0]);
    }

    CHECK(stub_config_asic_n[0] == 1, "the chain is configured once (got %d)",
            stub_config_asic_n[0]);
    CHECK(stub_baud_cmd_n[0] == 1 && stub_baud_cmd_val[0] == (uint32_t)UART_HIGH_BAUD_RATE,
            "...then told to switch to %u (got %d write(s), value %u)",
            (unsigned)UART_HIGH_BAUD_RATE, stub_baud_cmd_n[0], stub_baud_cmd_val[0]);
    CHECK(stub_config_asic_ord[0] < stub_baud_cmd_ord[0],
            "...in that order: config at record %d, baud command at %d — a rate "
            "change before the config would take the chain away mid-sequence",
            stub_config_asic_ord[0], stub_baud_cmd_ord[0]);
    CHECK(stub_cfg_baud_n[0] == 1 && stub_cfg_baud_val[0] == (uint32_t)UART_HIGH_BAUD_RATE,
            "...and the HOST follows to %u (got %d call(s), value %u)",
            (unsigned)UART_HIGH_BAUD_RATE, stub_cfg_baud_n[0], stub_cfg_baud_val[0]);
    CHECK(stub_rxflush_n[0] == 1, "...with the rx fifo flushed of the pre-switch garbage (got %d)",
            stub_rxflush_n[0]);
    CHECK(stub_uart_init_n[0] == 1 && stub_uart_init_baud[0] == (uint32_t)UART_DEFAULT_BAUD_RATE,
            "...the port having been opened at the enum baud %u (got %d open(s) "
            "at %u)",
            (unsigned)UART_DEFAULT_BAUD_RATE, stub_uart_init_n[0], stub_uart_init_baud[0]);

    reset_all();
    s_answer_on_try = 3;
    {
        int n = asic_bringup(0);
        CHECK(n == (int)MINER_MAX_CHIPS, "a board that answers on try %d still comes up (%d chips)",
                s_answer_on_try, n);
        CHECK(stub_enum_n_by_board[0] == s_answer_on_try + 1, "...after %d enumerations (got %d)",
                s_answer_on_try + 1, stub_enum_n_by_board[0]);
        CHECK(tst_slept_ms ==
                        PRE_PULSE_MS + BAUD_SWITCH_MS +
                                (uint64_t)s_answer_on_try * RI_RESET_PULSE_MS,
                "...costing %d x %u ms of RI reset on top of the fixed %u ms "
                "(got %llu ms)",
                s_answer_on_try, RI_RESET_PULSE_MS, PRE_PULSE_MS + BAUD_SWITCH_MS,
                (unsigned long long)tst_slept_ms);
        CHECK(stub_workmode_n[0] == 1 + s_answer_on_try && stub_cfgmode_n[0] == 1 + s_answer_on_try,
                "...one full low/high RI pulse per failed try, ending in config "
                "mode (work %d, config %d, want %d each)",
                stub_workmode_n[0], stub_cfgmode_n[0], 1 + s_answer_on_try);
    }

    reset_all();
    stub_enum_hook  = NULL;
    stub_enum_chips = 0;
    {
        int n = asic_bringup(0);
        CHECK(n == -1, "an absent board reports failure (got %d)", n);
        CHECK(stub_enum_n_by_board[0] == ENUM_RETRY_MAX,
                "...having been retried the full %d times (got %d)", ENUM_RETRY_MAX,
                stub_enum_n_by_board[0]);
        CHECK(tst_slept_ms == PRE_PULSE_MS + (uint64_t)ENUM_RETRY_MAX * RI_RESET_PULSE_MS,
                "...for %llu ms of sleep — the cost the watchdog feed-gate margin is spent on "
                "(got %llu ms)",
                (unsigned long long)(PRE_PULSE_MS + (uint64_t)ENUM_RETRY_MAX * RI_RESET_PULSE_MS),
                (unsigned long long)tst_slept_ms);
        CHECK(stub_config_asic_n[0] == 0, "...and is NOT configured (got %d)",
                stub_config_asic_n[0]);
        CHECK(stub_baud_cmd_n[0] == 0 && stub_cfg_baud_n[0] == 0,
                "...nor moved to the mining baud, either end (chain %d, host %d) "
                "— a dead chain left at %u could never be re-enumerated at %u",
                stub_baud_cmd_n[0], stub_cfg_baud_n[0], (unsigned)UART_HIGH_BAUD_RATE,
                (unsigned)UART_DEFAULT_BAUD_RATE);
    }

    reset_all();
    stub_enum_hook = enum_bogus_then_good;
    s_bogus_tries  = 2;
    {
        int n = asic_bringup(0);
        CHECK(n == (int)MINER_MAX_CHIPS,
                "an over-long answer is rejected and the retry finds the real "
                "chain (%d chips, want %d)",
                n, (int)MINER_MAX_CHIPS);
        CHECK(stub_enum_n_by_board[0] == s_bogus_tries + 1, "...taking %d enumerations (got %d)",
                s_bogus_tries + 1, stub_enum_n_by_board[0]);
        CHECK(tst_slept_ms ==
                        PRE_PULSE_MS + BAUD_SWITCH_MS + (uint64_t)s_bogus_tries * RI_RESET_PULSE_MS,
                "...and each rejection paid a real RI reset, not a free re-read "
                "(got %llu ms)",
                (unsigned long long)tst_slept_ms);
    }
    reset_all();
    s_bogus_tries = ENUM_RETRY_MAX + 1;
    {
        int n = asic_bringup(0);
        CHECK(n == -1,
                "a line that only ever answers out of range is an absent board "
                "(got %d)",
                n);
        CHECK(stub_enum_n_by_board[0] == ENUM_RETRY_MAX, "...after the full retry budget (got %d)",
                stub_enum_n_by_board[0]);
    }

    reset_all();
    stub_enum_hook = enum_stop_on_try;
    s_stop_try     = 2;
    g_run          = 1;
    {
        int n = asic_bringup(0);
        CHECK(n == -1, "an absent board stopped mid-retry reports failure (got %d)", n);
        CHECK(stub_enum_n_by_board[0] == s_stop_try + 1,
                "...after %d enumerations: a stop request ends the retries "
                "instead of running all %d (got %d)",
                s_stop_try + 1, ENUM_RETRY_MAX, stub_enum_n_by_board[0]);
    }
    g_run = 1;

    reset_all();
    stub_enum_hook = enum_dead_low;
    for (int dead = 0; dead <= (int)HBOARD_COUNT; dead++) {
        reset_all();
        s_dead_boards = dead;
        uint64_t t0   = tst_slept_ms;
        int      up   = 0;
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            if (asic_bringup((uint8_t)bd) > 0)
                up++;
        uint64_t cost = tst_slept_ms - t0;
        uint64_t want =
                (uint64_t)dead * (PRE_PULSE_MS + (uint64_t)ENUM_RETRY_MAX * RI_RESET_PULSE_MS) +
                (uint64_t)(HBOARD_COUNT - dead) * (PRE_PULSE_MS + BAUD_SWITCH_MS);
        CHECK(up == (int)HBOARD_COUNT - dead, "%d absent board(s): %d of %d enumerate (got %d)",
                dead, (int)HBOARD_COUNT - dead, (int)HBOARD_COUNT, up);
        CHECK(cost == want,
                "...costing %llu ms of bring-up sleep (got %llu) — the retry "
                "budget is paid once PER absent board, which is why the margin "
                "counts boards",
                (unsigned long long)want, (unsigned long long)cost);
    }
    printf("  one absent board costs %llu ms of sleep; %d would cost %llu\n",
            (unsigned long long)(PRE_PULSE_MS + (uint64_t)ENUM_RETRY_MAX * RI_RESET_PULSE_MS),
            (int)HBOARD_COUNT,
            (unsigned long long)((uint64_t)HBOARD_COUNT *
                    (PRE_PULSE_MS + (uint64_t)ENUM_RETRY_MAX * RI_RESET_PULSE_MS)));

    reset_all();
    stub_enum_hook       = NULL;
    stub_enum_chips      = 0;
    s_ipc_init_n         = 0;
    s_stop_after_bringup = 1;
    g_run                = 1;
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].chips_n = 0x7f7f7f7f;
    {
        int rc = asic_miner_main_unused();
        CHECK(rc == 1, "no hashboard at all -> main() exits non-zero (got %d)", rc);
        CHECK(s_ipc_init_n == 2,
                "...both of asic_miner's channels having been opened once, before "
                "bring-up rather than after it — datapath to core_miner and "
                "asic-ctrl to mm_miner (got %d call(s))",
                s_ipc_init_n);
        CHECK(stub_pll_commit_n == 0, "...and without ramping anything (got %d PLL commit(s))",
                stub_pll_commit_n);
        int zeroed = 1;
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            if (g_bd[bd].chips_n != 0)
                zeroed = 0;
        CHECK(zeroed, "...having zeroed every board's chip count (board 0 = %d)", g_bd[0].chips_n);
    }

    if (HBOARD_COUNT > 1) {
        reset_all();
        stub_enum_hook = enum_dead_low;
        s_dead_boards  = 1;
        s_ipc_init_n   = 0;
        g_run          = 1;
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            g_bd[bd].chips_n = -1;
        int rc = asic_miner_main_unused();
        CHECK(rc == 0, "one absent board of %d -> main() carries on (rc %d)", (int)HBOARD_COUNT,
                rc);
        CHECK(s_ipc_init_n == 2, "...opening both channels (got %d call(s))", s_ipc_init_n);
        CHECK(g_bd[0].chips_n == 0, "...with the absent board recorded as no chain (got %d)",
                g_bd[0].chips_n);
        int rest = 1;
        for (int bd = 1; bd < HBOARD_COUNT; bd++)
            if (g_bd[bd].chips_n != (int)MINER_MAX_CHIPS)
                rest = 0;
        CHECK(rest,
                "...and every board that DID answer carrying its chain (board 1 "
                "= %d, want %d)",
                g_bd[1].chips_n, (int)MINER_MAX_CHIPS);
        CHECK(stub_pll_commit_n == 0,
                "...and bring-up itself issues NO PLL write on any board: the "
                "INIT_FREQ program is the scan loop's own pll_step item, so a board "
                "that enumerated is not ramped before the loop runs (got %d "
                "commit(s), board 0 %d, board 1 %d)",
                stub_pll_commit_n, stub_pll_commit_by_board[0], stub_pll_commit_by_board[1]);
        CHECK(tst_slept_ms > (uint64_t)ENUM_RETRY_MAX * RI_RESET_PULSE_MS,
                "...and the whole %llu ms retry budget was spent inside the "
                "bring-up silence, not beside it (total %llu ms)",
                (unsigned long long)((uint64_t)ENUM_RETRY_MAX * RI_RESET_PULSE_MS),
                (unsigned long long)tst_slept_ms);
    } else {
        reset_all();
        stub_enum_hook  = NULL;
        stub_enum_chips = (int)MINER_MAX_CHIPS;
        s_ipc_init_n    = 0;
        g_run           = 1;
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            g_bd[bd].chips_n = -1;
        int rc = asic_miner_main_unused();
        CHECK(rc == 0 && s_ipc_init_n == 2,
                "the only board up -> main() carries on (rc %d, %d ipc init)", rc, s_ipc_init_n);
        CHECK(g_bd[0].chips_n == (int)MINER_MAX_CHIPS, "...with its chain recorded (%d chips)",
                g_bd[0].chips_n);
        CHECK((int)HBOARD_COUNT == 1,
                "on a 1-board product there is no partial bring-up: the fail-OPEN "
                "arm is unreachable and case 7's exit is the whole behaviour");
    }

    s_stop_after_bringup = 0;

    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        g_bd[bd].chips_n = 0;
        g_bd[bd].stage   = BS_RI;
    }
    CHECK(!is_all_running(),
            "with no board carrying a chain, the boards are never 'all running' "
            "and the efuse scan does not start");
    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        g_bd[bd].chips_n = (int)MINER_MAX_CHIPS;
        g_bd[bd].stage   = BS_RUN;
    }
    CHECK(is_all_running(), "every present board in BS_RUN is 'all running'");
    g_bd[HBOARD_COUNT - 1].stage = BS_COREREGS;
    CHECK(is_all_running(),
            "...and so is a present board in BS_COREREGS, its RI already pulled "
            "low and its frequency being set");
    g_bd[HBOARD_COUNT - 1].stage = BS_RI;
    CHECK(!is_all_running(), "...but one present board still in BS_RI is not");
    if (HBOARD_COUNT > 1) {
        g_bd[0].chips_n              = 0;
        g_bd[0].stage                = BS_RI;
        g_bd[HBOARD_COUNT - 1].stage = BS_RUN;
        CHECK(is_all_running(),
                "an absent board, parked in BS_RI forever, does not hold back "
                "'all running' for the boards that are present");
    }

    reset_all();
    stub_enum_hook  = NULL;
    stub_enum_chips = (int)MINER_MAX_CHIPS;
    run_to_efuse_start();
    s_req_at = 1;
    {
        int rc   = asic_miner_main_unused();
        int want = (int)HBOARD_COUNT * (int)MINER_MAX_CHIPS;
        CHECK(rc == 0 && s_scan_n == 1 && s_rec_n == want + 1 && stub_efuse_read_n == want,
                "mm requests the efuse during bring-up -> the scan runs once, one "
                "read and one record per chip plus the closing record (rc %d, "
                "%d scan(s), %d read(s), %d record(s), want %d)",
                rc, s_scan_n, stub_efuse_read_n, s_rec_n, want + 1);
        CHECK(s_scan_ri == 0,
                "...held until no present board is left in BS_RI (%d board(s) "
                "in BS_RI at the first record)",
                s_scan_ri);
        CHECK(s_last_ev && s_deinit_ev && s_last_ev < s_deinit_ev,
                "...and its closing record goes out BEFORE the channel it is "
                "sent on is freed (last at event %d, deinit at event %d)",
                s_last_ev, s_deinit_ev);
    }
    s_req_at = 0;

    reset_all();
    run_to_efuse_start();
    s_sleep_cap = 5000;
    {
        int rc = asic_miner_main_unused();
        CHECK(rc == 0 && stub_efuse_read_n == 0 && s_rec_n == 0 && is_all_running(),
                "no efuse message from mm_miner -> all boards run and no efuse "
                "is read or sent (rc %d, %d read(s), %d record(s))",
                rc, stub_efuse_read_n, s_rec_n);
    }

    reset_all();
    run_to_efuse_start();
    s_sleep_cap   = 5000;
    s_present_at  = 1;
    s_present_bin = 0x2a;
    g_bin_grade   = 0;
    {
        int rc = asic_miner_main_unused();
        CHECK(rc == 0 && stub_efuse_read_n == 0 && s_rec_n == 0 && is_all_running(),
                "mm reports efuse_info.txt present -> all boards run and no efuse "
                "is read or sent (rc %d, %d read(s), %d record(s))",
                rc, stub_efuse_read_n, s_rec_n);
        CHECK(g_bin_grade == 0x2a && s_telem_bin == 0x2a,
                "...and the bin grade mm sent is the one reported in telemetry "
                "(bin 0x%02x, telemetry %d)",
                (unsigned)g_bin_grade, s_telem_bin);
    }

    reset_all();
    run_to_efuse_start();
    s_sleep_cap  = 5000;
    s_present_at = 1;
    s_req_at     = 2000;
    {
        int rc = asic_miner_main_unused();
        CHECK(rc == 0 && s_scan_n == 1 && s_sleeps >= s_req_at &&
                        stub_efuse_read_n == (int)HBOARD_COUNT * (int)MINER_MAX_CHIPS,
                "a request that arrives once the boards are running -> the scan "
                "runs once (rc %d, %d scan(s), %d read(s) after %d sleep(s))",
                rc, s_scan_n, stub_efuse_read_n, s_sleeps);
    }
    s_present_at = 0;
    s_req_at     = 0;

    if (HBOARD_COUNT > 1) {
        reset_all();
        stub_enum_hook = enum_dead_low;
        s_dead_boards  = 1;
        run_to_efuse_start();
        s_req_at = 1;
        int rc   = asic_miner_main_unused();
        CHECK(rc == 0 && s_scan_n == 1 && s_rec_n > 0 && s_rec[0].board == 1,
                "one absent board of %d -> the efuse scan still runs once the "
                "present boards run, starting at the first present board (rc %d, "
                "%d scan(s), %d sleep(s) of %d)",
                (int)HBOARD_COUNT, rc, s_scan_n, s_sleeps, s_sleep_cap);
        s_req_at = 0;
    }
    s_sleep_cap = 0;
    reset_exit_trace();

    stub_enum_hook = NULL;

    {
        int chips[HBOARD_COUNT];
        int want = 0;

        for (int bd = 0; bd < HBOARD_COUNT; bd++) {
            chips[bd] = 2;
            want += chips[bd];
        }

        set_boards(chips, BS_RUN);
        scan_steps(TST_REC_MAX);
        CHECK(stub_efuse_read_n == 0 && s_rec_n == 0,
                "boards running and no request -> nothing is read (%d read(s), "
                "%d record(s))",
                stub_efuse_read_n, s_rec_n);

        set_boards(chips, BS_RI);
        tst_mm_efuse(0, 0);
        scan_steps(TST_REC_MAX);
        CHECK(stub_efuse_read_n == 0 && s_rec_n == 0,
                "a request while every board is still in BS_RI is held, not "
                "served (%d read(s), %d record(s))",
                stub_efuse_read_n, s_rec_n);
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            g_bd[bd].stage = BS_COREREGS;
        efuse_scan_step();
        CHECK(stub_efuse_read_n == 1 && s_rec_n == 0,
                "...and served as soon as every board is in BS_COREREGS: the "
                "first chip is read on that pass (%d read(s), %d record(s))",
                stub_efuse_read_n, s_rec_n);
        efuse_scan_step();
        CHECK(stub_efuse_read_n == 2 && s_rec_n == 1 && s_rec[0].seq == 0,
                "...its record goes out on the next pass, which reads the second "
                "chip (%d read(s), %d record(s))",
                stub_efuse_read_n, s_rec_n);
        tst_mm_efuse(0, 0);
        scan_steps(TST_REC_MAX);
        CHECK(s_scan_n == 1 && stub_efuse_read_n == want && s_rec_n == want + 1 && s_rec[want].last,
                "...a repeated request during the scan does not restart it (%d "
                "scan(s), %d read(s), %d record(s), want %d)",
                s_scan_n, stub_efuse_read_n, s_rec_n, want + 1);
        tst_mm_efuse(0, 0);
        for (int i = 0; i < TST_REC_MAX; i++)
            efuse_scan_step();
        CHECK(s_scan_n == 1 && stub_efuse_read_n == want && s_rec_n == want + 1,
                "...and a request after the scan is done reads nothing again "
                "(%d scan(s), %d read(s), %d record(s))",
                s_scan_n, stub_efuse_read_n, s_rec_n);

        set_boards(chips, BS_RUN);
        tst_mm_efuse(0, 0);
        efuse_scan_step();
        CHECK(stub_efuse_read_n == 1 && s_rec_n == 0,
                "a request while the boards run is served on the next pass (%d "
                "read(s), %d record(s))",
                stub_efuse_read_n, s_rec_n);

        if (HBOARD_COUNT > 1) {
            set_boards(chips, BS_RUN);
            g_bd[HBOARD_COUNT - 1].stage = BS_RI;
            tst_mm_efuse(0, 0);
            scan_steps(TST_REC_MAX);
            CHECK(stub_efuse_read_n == 0 && s_rec_n == 0,
                    "one present board still in BS_RI holds the request for all "
                    "boards (%d read(s), %d record(s))",
                    stub_efuse_read_n, s_rec_n);
        }
    }

    {
        int chips[HBOARD_COUNT] = { 0 };

        chips[0] = 1;
        set_boards(chips, BS_RUN);
        g_bin_grade = 0;
        tst_mm_efuse(1, 0x2b);
        scan_steps(TST_REC_MAX);
        CHECK(g_bin_grade == 0x2b && !g_efuse_req && stub_efuse_read_n == 0 && s_rec_n == 0,
                "mm reports the efuse file present -> its bin grade is taken and "
                "nothing is read (bin 0x%02x, %d read(s))",
                (unsigned)g_bin_grade, stub_efuse_read_n);
    }

    {
        int chips[HBOARD_COUNT];
        int want = 0, own = 1, restored = 1;

        for (int bd = 0; bd < HBOARD_COUNT; bd++) {
            chips[bd] = 3 + bd;
            want += chips[bd];
        }
        scan_boards(chips);
        for (int i = 0, bd = 0, chip = 0; i < want && i < s_rec_n; i++, chip++) {
            if (chip == chips[bd]) {
                bd++;
                chip = 0;
            }
            if (s_rec[i].seq != (uint32_t)i || s_rec[i].last || s_rec[i].board != bd ||
                    s_rec[i].chip != chip ||
                    s_rec[i].data[0] != (((uint32_t)bd << 16) | (uint32_t)chip))
                own = 0;
        }
        for (int bd = 0; bd < HBOARD_COUNT; bd++)
            if (uart_get_chip_select((uint8_t)bd) != TST_SEL_PARKED)
                restored = 0;
        CHECK(s_rec_n == want + 1 && own,
                "every chip's record carries the efuse read from that chip, not "
                "from chip 0 of its board (%d record(s), want %d)",
                s_rec_n, want + 1);
        CHECK(s_rec_n == want + 1 && s_rec[want].last && s_rec[want].seq == (uint32_t)want,
                "...and the scan closes with one last record numbered after them");
        CHECK(restored, "...and each board's chip select is put back after its reads");
    }

    {
        int chips[HBOARD_COUNT] = { 0 };

        chips[0]      = 2;
        s_send_refuse = 3;
        set_boards(chips, BS_RUN);
        tst_mm_efuse(0, 0);
        efuse_scan_step();
        efuse_scan_step();
        efuse_scan_step();
        tst_now_ms += EFUSE_SEND_RETRY_MIN_MS - 1;
        efuse_scan_step();
        CHECK(s_send_refuse == 2 && s_rec_n == 0,
                "a refused record is not resent before %u ms have passed, so "
                "a fast scan loop does not hammer the socket (%d refusal(s) "
                "left)",
                (unsigned)EFUSE_SEND_RETRY_MIN_MS, s_send_refuse);
        tst_now_ms += 1;
        scan_steps(TST_REC_MAX);
        CHECK(s_send_refuse == 0 && s_rec_n == 3 && s_rec[0].seq == 0 && s_rec[0].chip == 0 &&
                        s_rec[1].seq == 1 && s_rec[1].chip == 1 && s_rec[2].seq == 2 &&
                        s_rec[2].last,
                "a record mm_miner refuses is resent as is, so no chip is "
                "skipped or renumbered (%d record(s))",
                s_rec_n);
    }

    {
        int chips[HBOARD_COUNT] = { 0 };

        chips[0] = 1;
        /* Blank efuse: word 10 == 0, so the vote is majority(w11, 0, 0) == 0. */
        memset(stub_efuse_data, 0, sizeof stub_efuse_data);
        stub_efuse_data[11] = 0x17;
        g_bin_grade         = 0x55;
        scan_boards(chips);
        CHECK(g_bin_grade == 0,
                "scan record 0 on a blank efuse reports bin 0, not the previous "
                "value (got %u)",
                (unsigned)g_bin_grade);

        stub_efuse_data[10] = 0x1717;
        g_bin_grade         = 0x55;
        scan_boards(chips);
        CHECK(g_bin_grade == efuse_bin_grade(stub_efuse_data) && g_bin_grade == 0x17,
                "scan record 0 on a programmed efuse reports efuse_bin_grade() "
                "(got 0x%02x)",
                (unsigned)g_bin_grade);

        memset(stub_efuse_data, 0, sizeof stub_efuse_data);
    }
    return tst_done();
}
