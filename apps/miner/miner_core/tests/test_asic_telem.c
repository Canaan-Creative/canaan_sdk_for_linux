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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osif.h"
#include "boardconf.h"
#include "miner_ipc.h"

#include "../../tests/include/tst_check.h"

/* Runs one status-read step on board per chip */
#define PASS(board, chips) \
    do { \
        for (int i_ = 0; i_ < (chips); i_++) \
            rr_step((board), (chips)); \
    } while (0)

#define TEMP_STEP(board, chips) rr_step((board), (chips)) /* Runs one status-read step on board */

#define DISPLAY_PASSES RS_TSENSOR /* Passes over the read types that precede RS_TSENSOR */

static asic_telem      tst_tm; /* Telemetry part of the last MSG_ASIC_TELEM sent */
static asic_chip_telem tst_ct; /* Last MSG_ASIC_TELEM payload sent */
/* MSG_ASIC_TELEM messages captured into tst_tm and tst_ct */
static int tst_tm_n, tst_ct_n;

static int tst_tach[FAN_COUNT]; /* Fan speed fan_dev_get_rpm() returns per fan, RPM */

/* Temperature the stub a3197s_fetch_pvt_temp() reports, degrees C */
extern const double stub_pvt_temp_c;

extern uint32_t stub_uart_errcnt[]; /* UART error count the stub reports per board */
extern int      stub_enum_chips;
/* Replaces the stub a3197s_read_reg() when set */
extern int (*stub_read_reg_hook)(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n);
extern int         stub_ss_n;      /* Stub uart_write_fifo() calls, including those not logged */
extern uint32_t    stub_ss_addr[]; /* Register address of each logged stub uart_write_fifo() call */
static osif_ipc_t *tst_refuse_h;   /* IPC handle whose sends of tst_refuse_type fail */
static uint32_t    tst_refuse_type;   /* Message type refused on tst_refuse_h */
static uint64_t    tst_now;           /* Simulated clock returned by osif_now_ms(), ms */
static char        tst_err_log[512];  /* Text of the last OSIF_LOG_ERR message */
static int         tst_err_n;         /* OSIF_LOG_ERR messages logged */
static char        tst_warn_log[512]; /* Text of the last OSIF_LOG_WARN message */
static int         tst_warn_n;        /* OSIF_LOG_WARN messages logged */

static uint32_t tst_ecc_word; /* Word tst_ecc_read() returns from A3197S_UART_TIMEOUT_ADDR */

/* The seven-register UART error block at 0x3ca4, plus every read of it. */
static uint32_t tst_ecc_blk[7];
static int      tst_ecc_blk_n, tst_ecc_blk_bad;

static int tst_ecc_fail; /* Non-zero makes tst_ecc_fail_read() fail reads of the UART error block */

/* Speed-log timer, pass count and fail count tst_spd_read() returns */
static uint32_t tst_spd_tmr, tst_spd_pv, tst_spd_fv;

static int tst_chkdrop_n; /* Reads of A3197S_CHKDROP_ADDR */

int fan_dev_get_rpm(int fan)
{
    if (fan < 0 || fan >= FAN_COUNT)
        return -1;
    return tst_tach[fan];
}

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    return OSIF_OK;
}
void osif_ipc_deinit(osif_ipc_t *h)
{
    (void)h;
}

osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    if (h && h == tst_refuse_h && type == tst_refuse_type)
        return OSIF_ERR;
    if (type == MSG_ASIC_TELEM && data && len == (int)sizeof tst_ct) {
        memcpy(&tst_ct, data, sizeof tst_ct);
        tst_ct_n++;
        tst_tm = tst_ct.tm;
        tst_tm_n++;
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
uint64_t osif_now_ms(void)
{
    return tst_now;
}
void osif_sleep_ms(uint32_t ms)
{
    (void)ms;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    char *buf;
    if (lvl == OSIF_LOG_ERR) {
        buf = tst_err_log;
        tst_err_n++;
    } else if (lvl == OSIF_LOG_WARN) {
        buf = tst_warn_log;
        tst_warn_n++;
    } else {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 512, fmt, ap);
    va_end(ap);
}

#define main asic_miner_main_unused
#include "asic_miner_tu.h"
#undef main

static void publish(uint8_t board, int chips)
{
    tst_tm_n = tst_ct_n = 0;
    memset(&tst_tm, 0, sizeof tst_tm);
    memset(&tst_ct, 0, sizeof tst_ct);
    g_bd[board].chips_n = chips;
    asic_send_info(board);
}

static void rr_step(int board, int chips)
{
    int save[HBOARD_COUNT];
    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        save[bd]         = g_bd[bd].chips_n;
        g_bd[bd].chips_n = 0;
    }
    g_bd[board].chips_n = chips;

    for (int guard = 0; guard < 4; guard++) {
        g_rs.board = (uint8_t)board;
        uint8_t t0 = g_rs.type, c0 = g_rs.chip;
        asic_collect_status();
        if (g_rs.board != (uint8_t)board) {
            for (int walk = 0; walk < HBOARD_COUNT && g_rs.board != 0; walk++)
                asic_collect_status();
            break;
        }
        if (g_rs.type != t0 || g_rs.chip != c0)
            break;
    }

    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].chips_n = save[bd];
}
static int tst_ecc_read(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    (void)ch;
    if (!out)
        return 0;
    memset(out, 0, (size_t)n * sizeof *out);
    if (addr == A3197S_UART_TIMEOUT_ADDR && n >= 1)
        out[0] = tst_ecc_word;
    return 0;
}
static int tst_ecc_blk_read(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    (void)ch;
    if (!out)
        return 0;
    if (addr >= A3197S_UART_TIMEOUT_ADDR && addr < A3197S_UART_TIMEOUT_ADDR + 4 * 7) {
        tst_ecc_blk_n++;
        if (addr != A3197S_UART_TIMEOUT_ADDR || n != 7)
            tst_ecc_blk_bad++;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (addr + 4 * i - A3197S_UART_TIMEOUT_ADDR) / 4;
        out[i]       = idx < 7 ? tst_ecc_blk[idx] : 0;
    }
    return 0;
}
static int tst_ecc_fail_read(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    int rc = tst_ecc_blk_read(ch, addr, out, n);
    if (tst_ecc_fail && out && addr == A3197S_UART_TIMEOUT_ADDR) {
        memset(out, 0, (size_t)n * sizeof *out);
        return 1;
    }
    return rc;
}

static int ecc_clear_n(void)
{
    int n = 0;
    for (int i = 0; i < stub_ss_n; i++)
        if (stub_ss_addr[i] == A3197S_UART_TIMEOUT_ADDR)
            n++;
    return n;
}
static int tst_spd_read(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    (void)ch;
    if (!out)
        return 0;
    memset(out, 0, (size_t)n * sizeof *out);
    if (addr == A3197S_SPDLOG_TIMER_ADDR)
        out[0] = tst_spd_tmr;
    if (addr == A3197S_SPDLOG_PASSCNT_ADDR)
        out[0] = tst_spd_pv;
    if (addr == A3197S_SPDLOG_FAILCNT_ADDR)
        out[0] = tst_spd_fv;
    return 0;
}

static void spd_reset(void)
{
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].chips_n = 0;
    memset(g_chip_pass, 0, sizeof g_chip_pass);
    memset(g_chip_fail, 0, sizeof g_chip_fail);
    memset(g_chip_pass_bak, 0, sizeof g_chip_pass_bak);
    memset(g_chip_fail_bak, 0, sizeof g_chip_fail_bak);
    memset(g_spd_failcnt, 0, sizeof g_spd_failcnt);
}

static void ecc_pass(int chips)
{
    g_rs.type = RS_ECC;
    g_rs.chip = 0;
    PASS(0, chips);
}
static int tst_count_chkdrop(uint8_t ch, uint32_t addr, uint32_t *out, uint32_t n)
{
    (void)ch;
    if (out)
        memset(out, 0, (size_t)n * sizeof *out);
    if (addr == A3197S_CHKDROP_ADDR)
        tst_chkdrop_n++;
    return 0;
}

int main(void)
{
    printf("descriptor %s: %d fan(s), %d board(s)\n", BOARDCONF_SECTION, (int)FAN_COUNT,
            (int)HBOARD_COUNT);

    for (int f = 0; f < FAN_COUNT; f++)
        tst_tach[f] = 1460 + 10 * f;
    publish(0, MINER_MAX_CHIPS);

    CHECK(tst_tm_n == 2,
            "asic_send_info published MSG_ASIC_TELEM to BOTH peers — core_miner "
            "for the :4028 display and mm_miner for the control loops (got %d)",
            tst_tm_n);
    for (int f = 0; f < FAN_COUNT; f++)
        CHECK(tst_tm.fan_rpm[f] == 0,
                "fan %d is left at 0 even with a live counter behind it (%u rpm "
                "available, published %u)",
                f, (unsigned)(1460 + 10 * f), (unsigned)tst_tm.fan_rpm[f]);

    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].stage = BS_COREREGS;
    publish(0, MINER_MAX_CHIPS);
    CHECK(tst_tm.in_work == 0, "a chain still in COREREGS is not in work (got %u)",
            (unsigned)tst_tm.in_work);

    g_bd[0].stage = BS_RUN;
    publish(0, MINER_MAX_CHIPS);
    CHECK(tst_tm.in_work == 1, "a chain that reached BS_RUN is in work (got %u)",
            (unsigned)tst_tm.in_work);

#if HBOARD_COUNT > 1
    publish(1, MINER_MAX_CHIPS);
    CHECK(tst_tm.in_work == 1,
            "board 1's tick reports the MACHINE in work while board 0 hashes "
            "(got %u)",
            (unsigned)tst_tm.in_work);
#endif /* HBOARD_COUNT > 1 */
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].stage = BS_RI;

    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        g_bd[bd].local_work = 1000u + 7u * (uint32_t)bd;
    publish(0, MINER_MAX_CHIPS);
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        CHECK(tst_ct.local_work[bd] == 1000u + 7u * (uint32_t)bd,
                "board 0's publish carries board %d's work count, the estats "
                "LW[] source (got %u, want %u)",
                bd, (unsigned)tst_ct.local_work[bd], 1000u + 7u * (unsigned)bd);

    const int chips = 3;

    publish(0, chips);
    CHECK(tst_ct_n == 2, "...and the per-chip arrays rode both (got %d)", tst_ct_n);
    CHECK(tst_ct.pvt_lapped[0] == 0,
            "board 0 starts un-lapped (pvt_t[] is still the -273 sentinel)");

    for (int t = 0; t < DISPLAY_PASSES; t++)
        PASS(0, chips);
    publish(0, chips);
    CHECK(tst_ct.pvt_lapped[0] == 0, "%d round-robin pass(es) wrapping does NOT lap the board",
            (int)DISPLAY_PASSES);

    {
        int filled = 0;
        for (int c = 0; c < chips; c++)
            if (tst_ct.pvt_t[0][c] == (int16_t)stub_pvt_temp_c)
                filled++;
        CHECK(filled == 0,
                "a display pass read the temperature of %d of %d chips — only "
                "RS_TSENSOR may",
                filled, chips);
    }

    for (int i = 0; i < chips - 1; i++)
        TEMP_STEP(0, chips);
    publish(0, chips);
    CHECK(tst_ct.pvt_lapped[0] == 0, "a partial temperature pass does NOT lap the board");

    TEMP_STEP(0, chips);
    publish(0, chips);
    CHECK(tst_ct.pvt_lapped[0] == 1,
            "the temperature pass wrapping laps the board, and it is PUBLISHED");

    {
        int temp_passes = 0, volt_passes = 0;
        for (int t = 0; t < RS_TYPES; t++) {
            for (int c = 0; c < chips; c++) {
                g_chip_temp[0][c] = ASIC_TEMP_NONE;
                g_chip_volt[0][c] = 0;
            }
            g_rs.type        = (uint8_t)t;
            g_rs.chip        = 0;
            g_rs.vt_selected = 0;
            PASS(0, chips);

            int nt = 0, nv = 0;
            for (int c = 0; c < chips; c++) {
                if (g_chip_temp[0][c] == stub_pvt_temp_c)
                    nt++;
                if (g_chip_volt[0][c] != 0)
                    nv++;
            }
            CHECK(nt == 0 || nt == chips,
                    "type %d read %d of %d temperatures — a pass covers the whole "
                    "chain or none of it",
                    t, nt, chips);
            CHECK(nv == 0 || nv == chips, "type %d read %d of %d voltages", t, nv, chips);
            if (nt)
                temp_passes++;
            if (nv)
                volt_passes++;
        }
        CHECK(temp_passes == 1,
                "%d of %d round-robin types read the chain temperature, expected "
                "exactly 1 — that ratio IS the refresh interval the thermal soft-off trips on",
                temp_passes, (int)RS_TYPES);
        CHECK(volt_passes == 1,
                "%d of %d types read the chain voltage, expected exactly 1: "
                "temperature and voltage are EQUALS in this cursor, one pass each",
                volt_passes, (int)RS_TYPES);
    }

#if HBOARD_COUNT > 1
    publish(1, chips);
    CHECK(tst_ct.pvt_lapped[1] == 0, "board 1 is still un-lapped while board 0 is lapped");

    for (int t = 0; t < DISPLAY_PASSES; t++)
        PASS(1, chips);
    for (int i = 0; i < chips; i++)
        TEMP_STEP(1, chips);
    publish(1, chips);
    CHECK(tst_ct.pvt_lapped[1] == 1, "board 1 laps on its OWN temperature pass");

    publish(0, chips);
    CHECK(tst_ct.pvt_lapped[0] == 1, "...and board 0 is still lapped");

    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        g_bd[bd].chips_n    = chips;
        g_pvt_lapped[bd]    = 1;
        g_bd[bd].local_work = 1000u + 7u * (uint32_t)bd;
        for (int c = 0; c < chips; c++)
            g_chip_temp[bd][c] = 40.0 + 20.0 * bd + c;
    }

    publish(0, chips);
    for (int bd = 0; bd < HBOARD_COUNT; bd++) {
        CHECK(tst_ct.chips[bd] == (uint16_t)chips,
                "board 0's publish carries board %d's chip count (got %u, want %d)", bd,
                (unsigned)tst_ct.chips[bd], chips);
        CHECK(tst_ct.pvt_lapped[bd] == 1,
                "board 0's publish carries board %d's lap flag — one row per "
                "datagram can never show all %d lapped at once, which is what "
                "pins TAvg at the sentinel",
                bd, (int)HBOARD_COUNT);
        CHECK(tst_ct.local_work[bd] == 1000u + 7u * (uint32_t)bd,
                "board 0's publish carries board %d's work count, the estats LW[] "
                "source (got %u, want %u)",
                bd, (unsigned)tst_ct.local_work[bd], 1000u + 7u * (unsigned)bd);
        for (int c = 0; c < chips; c++)
            CHECK(tst_ct.pvt_t[bd][c] == (int16_t)(40 + 20 * bd + c),
                    "board 0's publish carries board %d chip %d's temperature "
                    "(got %d, want %d)",
                    bd, c, (int)tst_ct.pvt_t[bd][c], 40 + 20 * bd + c);
    }

    publish((uint8_t)(HBOARD_COUNT - 1), chips);
    for (int bd = 0; bd < HBOARD_COUNT; bd++)
        CHECK(tst_ct.pvt_t[bd][0] == (int16_t)(40 + 20 * bd),
                "board %d's publish carries board %d's row too (got %d, want %d)",
                (int)HBOARD_COUNT - 1, bd, (int)tst_ct.pvt_t[bd][0], 40 + 20 * bd);

    g_bd[HBOARD_COUNT - 1].chips_n = 0;
    publish(0, chips);
    CHECK(tst_ct.chips[HBOARD_COUNT - 1] == 0,
            "a board that never enumerated publishes a 0 chip count (got %u)",
            (unsigned)tst_ct.chips[HBOARD_COUNT - 1]);
    CHECK(tst_ct.chips[0] == (uint16_t)chips,
            "...and the boards that DID enumerate are unaffected (got %u, want %d)",
            (unsigned)tst_ct.chips[0], chips);
#endif /* HBOARD_COUNT > 1 */

    const int ecc_chips = 3;
    stub_read_reg_hook  = tst_ecc_read;
    tst_ecc_word        = 3;
    stub_uart_errcnt[0] = 5;
    g_board_err_crc[0] = g_board_com_crc[0] = 0;

    ecc_pass(ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ct.com_crc[0] == 5,
            "COMCRC carries the transport's failed-read count ALONE (got %u, want 5)",
            (unsigned)tst_ct.com_crc[0]);
    for (int board = 1; board < HBOARD_COUNT; board++)
        CHECK(tst_ct.com_crc[board] == 0, "failed UART reads stay on their own board, not board %d",
                board);
    CHECK(tst_ct.err_crc[0] == (uint32_t)(3 * ecc_chips + 5),
            "CRC carries every chip's 0x3ca4 block PLUS that count (got %u, want %d)",
            (unsigned)tst_ct.err_crc[0], 3 * ecc_chips + 5);

    publish(0, ecc_chips);
    CHECK(tst_ct.err_crc[0] == (uint32_t)(3 * ecc_chips + 5) && tst_ct.com_crc[0] == 5,
            "a second publish restates the SAME tally — the wire value is "
            "CUMULATIVE and sending does not drain it, so a dropped datagram "
            "costs nothing (got %u/%u)",
            (unsigned)tst_ct.err_crc[0], (unsigned)tst_ct.com_crc[0]);

    tst_ecc_word        = 0;
    stub_uart_errcnt[0] = 2;
    ecc_pass(ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ct.com_crc[0] == 7,
            "the next pass ADDS the reads that failed since, and only those "
            "(got %u, want 7)",
            (unsigned)tst_ct.com_crc[0]);
    CHECK(tst_ct.err_crc[0] == (uint32_t)(3 * ecc_chips + 7),
            "...and CRC grows by the same 2, its ASIC block being quiet now "
            "(got %u, want %d)",
            (unsigned)tst_ct.err_crc[0], 3 * ecc_chips + 7);

    stub_enum_chips     = ecc_chips;
    stub_uart_errcnt[0] = 4;
    CHECK(asic_bringup(0) == ecc_chips, "bring-up enumerated %d chip(s)", ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ct.err_crc[0] == 0 && tst_ct.com_crc[0] == 0,
            "bring-up CLEARS both tallies for the board it re-enumerates, on "
            "every chip configuration, so the wire value counts "
            "from the current enumeration and is NOT monotonic for the life of "
            "the process (got %u/%u, want 0/0)",
            (unsigned)tst_ct.err_crc[0], (unsigned)tst_ct.com_crc[0]);

    ecc_pass(ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ct.com_crc[0] == 4,
            "...and the pass after bring-up counts up from that zero, harvesting "
            "the 4 reads the TRANSPORT failed across it and nothing older "
            "(got %u, want 4)",
            (unsigned)tst_ct.com_crc[0]);
    CHECK(tst_ct.err_crc[0] == 4,
            "...and CRC restarts from the same 4, its ASIC block being quiet now "
            "(got %u, want 4)",
            (unsigned)tst_ct.err_crc[0]);

    tst_ecc_blk[0]      = 0x00000201u; /* TIMEOUT: fsm_to 1, di_to 2 */
    tst_ecc_blk[1]      = 1000;        /* HDRPASS */
    tst_ecc_blk[2]      = 1000;        /* HDRFAIL */
    tst_ecc_blk[3]      = 1000;        /* CHKPASS */
    tst_ecc_blk[4]      = 5;           /* CHKFAIL */
    tst_ecc_blk[5]      = 0xff000007u; /* RX_START_ERR, low 24 bits count */
    tst_ecc_blk[6]      = 0xff000011u; /* RX_GLITCH, low 24 bits count */
    tst_ecc_blk_n       = 0;
    tst_ecc_blk_bad     = 0;
    stub_read_reg_hook  = tst_ecc_blk_read;
    stub_uart_errcnt[0] = 0;
    g_board_err_crc[0] = g_board_com_crc[0] = 0;
    ecc_pass(ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ecc_blk_n == ecc_chips && tst_ecc_blk_bad == 0,
            "an RS_ECC pass reads the whole 7-word block at 0x3ca4 in one read "
            "per chip (%d read(s), %d not "
            "7 words at TIMEOUT)",
            tst_ecc_blk_n, tst_ecc_blk_bad);
    CHECK(tst_ct.err_crc[0] == (uint32_t)((1 + 2 + 5 + 0x07 + 0x11) * ecc_chips),
            "CRC sums fsm_to, di_to, CHKFAIL and the low 24 bits of RX_START_ERR "
            "and RX_GLITCH, and none of the pass counters (got %u, want %d)",
            (unsigned)tst_ct.err_crc[0], (1 + 2 + 5 + 0x07 + 0x11) * ecc_chips);

    stub_read_reg_hook  = tst_ecc_fail_read;
    tst_ecc_fail        = 1;
    stub_uart_errcnt[0] = 2;
    stub_ss_n           = 0;
    g_board_err_crc[0] = g_board_com_crc[0] = 0;
    ecc_pass(ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ct.err_crc[0] == 2 && tst_ct.com_crc[0] == 2,
            "a failed read of the 0x3ca4 block adds nothing to CRC; only the "
            "transport's failed-read count lands (CRC %u, COMCRC %u, want 2/2)",
            (unsigned)tst_ct.err_crc[0], (unsigned)tst_ct.com_crc[0]);
    CHECK(ecc_clear_n() == 0,
            "...and the block is not cleared, so the chip keeps its counts for "
            "the next pass (%d clear write(s))",
            ecc_clear_n());

    tst_ecc_fail = 0;
    stub_ss_n    = 0;
    ecc_pass(ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ct.err_crc[0] == (uint32_t)(2 + (1 + 2 + 5 + 0x07 + 0x11) * ecc_chips),
            "the next successful read adds what the chips kept (got %u, want %d)",
            (unsigned)tst_ct.err_crc[0], 2 + (1 + 2 + 5 + 0x07 + 0x11) * ecc_chips);
    CHECK(ecc_clear_n() == ecc_chips,
            "...and clears the block on every chip it read (%d clear write(s), "
            "want %d)",
            ecc_clear_n(), ecc_chips);

    tst_chkdrop_n      = 0;
    stub_read_reg_hook = tst_count_chkdrop;
    g_rs.type          = RS_SPDCNT;
    g_rs.chip          = 0;
    PASS(0, ecc_chips);
    CHECK(tst_chkdrop_n == ecc_chips, "RS_SPDCNT reads CHKDROP once per chip (%d of %d)",
            tst_chkdrop_n, ecc_chips);

    stub_read_reg_hook = NULL;

    tst_now = 1000000;
    publish(0, chips);
    CHECK(tst_ct.asic_health == ASIC_HEALTH_OK, "a published snapshot is healthy (got %u)",
            (unsigned)tst_ct.asic_health);

    tst_now += 3600000;
    publish(0, chips);
    CHECK(tst_ct.asic_health == ASIC_HEALTH_OK,
            "...and stays healthy an hour later: no asic_health bit is derived "
            "from elapsed time, since any bit reboots the board (got %u)",
            (unsigned)tst_ct.asic_health);

    stub_uart_errcnt[0] = 0;
    uint32_t com_prev   = tst_ct.com_crc[0];
    ecc_pass(ecc_chips);
    publish(0, ecc_chips);
    CHECK(tst_ct.com_crc[0] == com_prev,
            "a clean UART sweep adds no failed reads (COMCRC %u, was %u)",
            (unsigned)tst_ct.com_crc[0], (unsigned)com_prev);

    spd_reset();
    publish(0, chips);
    CHECK(tst_tm.realtime_dh_x1000 == 100000u,
            "no chip with a spdlog pass count reports realtime DH 100%% "
            "(got %u, want 100000)",
            (unsigned)tst_tm.realtime_dh_x1000);

    spd_reset();
    g_chip_pass[0][0]     = 1000;
    g_chip_fail[0][0]     = 10;
    g_chip_pass[0][1]     = 0;
    g_chip_fail[0][1]     = 999;
    g_chip_pass[0][2]     = 3000;
    g_chip_fail[0][2]     = 7;
    g_chip_pass_bak[0][0] = 50000;
    g_chip_fail_bak[0][0] = 50000;
#if HBOARD_COUNT > 1
    g_bd[1].chips_n        = 1;
    g_chip_pass[1][0]      = 5000;
    g_chip_fail[1][0]      = 3;
    const uint32_t dh_want = 222u;
#else
    const uint32_t dh_want = 423u;
#endif /* HBOARD_COUNT > 1 */
    publish(0, chips);
    CHECK(tst_tm.realtime_dh_x1000 == dh_want,
            "realtime DH is fail / (pass + fail) * 100 over the live counters of "
            "every chip with a non-zero pass count, in 0.001%% (got %u, want %u)",
            (unsigned)tst_tm.realtime_dh_x1000, (unsigned)dh_want);

    spd_reset();
    stub_read_reg_hook = tst_spd_read;
    tst_spd_tmr        = SS_TIMER_VALID_VALUE;
    tst_spd_pv         = 200000;
    tst_spd_fv         = 1000;
    g_rs.type          = RS_SPDLOG;
    g_rs.chip          = 0;
    PASS(0, chips);
    publish(0, chips);
    const uint32_t spd_valid = tst_tm.ghs_spd_mhs;
    CHECK(spd_valid == (uint32_t)(3 * 200000 * 0.256),
            "a latched spdlog window reports pass_sum * 0.256 MH/s (got %u, "
            "want %u)",
            (unsigned)spd_valid, (unsigned)(3 * 200000 * 0.256));
    CHECK(tst_tm.realtime_dh_x1000 == 498u,
            "...and realtime DH from the same counters (got %u, want 498)",
            (unsigned)tst_tm.realtime_dh_x1000);

    tst_spd_tmr = SS_TIMER_DEFAULT_VALUE;
    tst_spd_pv  = 90000;
    tst_spd_fv  = 20000;
    g_rs.type   = RS_SPDLOG;
    g_rs.chip   = 0;
    PASS(0, chips);
    publish(0, chips);
    CHECK(tst_tm.realtime_dh_x1000 == 18182u,
            "an unlatched window with pass + fail > 100000 moves realtime DH "
            "(got %u, want 18182)",
            (unsigned)tst_tm.realtime_dh_x1000);
    CHECK(tst_tm.ghs_spd_mhs == spd_valid,
            "...but not GHSspd, which follows the latched counters only "
            "(got %u, want %u)",
            (unsigned)tst_tm.ghs_spd_mhs, (unsigned)spd_valid);
    stub_read_reg_hook = NULL;
    spd_reset();

    {
        static char core_tag, mm_tag;
        osif_ipc_t *core_h = (osif_ipc_t *)&core_tag;
        osif_ipc_t *mm_h   = (osif_ipc_t *)&mm_tag;

        g_ipc_asic_core = core_h;
        g_ipc_asic_mm   = mm_h;

        tst_refuse_h    = core_h;
        tst_refuse_type = MSG_ASIC_TELEM;
        tst_err_n       = 0;
        tst_err_log[0]  = 0;
        publish(0, chips);
        CHECK(tst_tm_n == 1 && tst_err_n == 1 &&
                        !strcmp(tst_err_log, "status update dropped: telem->core_miner") &&
                        !strstr(tst_err_log, "mm_miner") && !strstr(tst_err_log, "version"),
                "a TELEM refused by core_miner is logged against core_miner's "
                "telemetry alone; mm_miner still got its copy (%d delivered, "
                "log \"%s\")",
                tst_tm_n, tst_err_log);

        publish(0, chips);
        CHECK(tst_err_n == 1,
                "...and the same refusal on the next tick is not logged again "
                "(%d ERR line(s))",
                tst_err_n);

        tst_refuse_h    = NULL;
        tst_warn_n      = 0;
        tst_warn_log[0] = 0;
        publish(0, chips);
        CHECK(tst_tm.diag == 0 && tst_ct.asic_health == ASIC_HEALTH_OK,
                "a refusal sets neither diag nor asic_health (diag %u, health %u)",
                (unsigned)tst_tm.diag, (unsigned)tst_ct.asic_health);
        CHECK(tst_err_n == 1 && tst_warn_n == 1 && !strcmp(tst_warn_log, "status update resumed"),
                "the first fully accepted tick closes the outage with one WARN "
                "(%d ERR, %d WARN, \"%s\")",
                tst_err_n, tst_warn_n, tst_warn_log);
        publish(0, chips);
        CHECK(tst_tm.diag == 0 && tst_warn_n == 1,
                "...and no further WARN once every send is accepted "
                "(diag %u, %d WARN)",
                (unsigned)tst_tm.diag, tst_warn_n);

        tst_refuse_h    = mm_h;
        tst_refuse_type = MSG_ASIC_TELEM;
        tst_err_n       = 0;
        tst_err_log[0]  = 0;
        publish(0, chips);
        CHECK(tst_err_n == 1 && strstr(tst_err_log, "telem->mm_miner") &&
                        !strstr(tst_err_log, "core_miner"),
                "a TELEM refused by mm_miner names mm_miner alone (log \"%s\")", tst_err_log);

        tst_refuse_h    = core_h;
        tst_refuse_type = MSG_ASIC_VERSION;
        publish(0, chips);
        tst_refuse_h    = mm_h;
        tst_refuse_type = MSG_ASIC_TELEM;
        publish(0, chips);
        tst_refuse_h    = core_h;
        tst_refuse_type = MSG_ASIC_VERSION;
        publish(0, chips);
        CHECK(tst_err_n == 1,
                "a refused set that keeps changing within one outage is not "
                "logged again (%d ERR line(s))",
                tst_err_n);

        tst_refuse_h    = NULL;
        tst_warn_n      = 0;
        tst_warn_log[0] = 0;
        publish(0, chips);
        CHECK(tst_warn_n == 1 && !strcmp(tst_warn_log, "status update resumed") && tst_tm.diag == 0,
                "...and one WARN closes it (%d WARN, \"%s\", diag %u)", tst_warn_n, tst_warn_log,
                (unsigned)tst_tm.diag);

        tst_refuse_h    = core_h;
        tst_refuse_type = MSG_ASIC_VERSION;
        tst_err_n       = 0;
        tst_err_log[0]  = 0;
        publish(0, chips);
        CHECK(tst_err_n == 1 && strstr(tst_err_log, "version->core_miner") &&
                        !strstr(tst_err_log, "telemetry"),
                "a refusal after the outage closed starts a new one and is "
                "logged again (log \"%s\")",
                tst_err_log);

        tst_refuse_h = NULL;
        publish(0, chips);
        publish(0, chips);
        g_ipc_asic_core = NULL;
        g_ipc_asic_mm   = NULL;
    }

    return tst_done();
}
