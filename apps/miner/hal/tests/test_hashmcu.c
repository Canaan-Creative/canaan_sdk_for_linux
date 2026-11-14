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

#include "hashmcu.h"
#include "hbmcu_ops.h"
#include "boardconf.h"
#include "miner_ipc.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define TCA9546A 0x70 /* TCA9546A I2C mux address */
#define MCU      0x10 /* hashboard monitor MCU I2C address */
#define R_OUT    0x15 /* outlet temperature register */
#define R_IN     0x16 /* inlet temperature register */

#define MAX_REGS 16 /* capacity of the scripted register table */
#define MAX_TX   32 /* capacity of the transfer log */

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            fails++; \
        } \
    } while (0)
#define EQI(got, want, msg) \
    do { \
        long g_ = (long)(got), w_ = (long)(want); \
        if (g_ != w_) { \
            printf("FAIL: %s (got %ld, want %ld)\n", (msg), g_, w_); \
            fails++; \
        } \
    } while (0)

static int fails;

static struct {
    int sel;      /* mux channel mask last written, -1 = none */
    int mux_fail; /* non-zero fails mux writes */
    int init_fail;
    /* scripted MCU reads: mux channel mask, register, 3 data bytes, non-zero = read fails */
    struct {
        uint8_t ch, reg, d[3];
        int     fail;
    } r[MAX_REGS];
    int nregs;
    /* transfer log: 1 = write, address, register, length */
    struct {
        int      wr;
        uint16_t addr;
        uint8_t  reg;
        uint8_t  n;
    } tx[MAX_TX];
    int ntx;
    int inits;
} m;

static void mock_reset(void)
{
    memset(&m, 0, sizeof m);
    m.sel = -1;
}

static void mcu_reg(int b, uint8_t reg, uint16_t val)
{
    uint8_t lo = (uint8_t)(val & 0xff), hi = (uint8_t)(val >> 8);
    m.r[m.nregs].ch   = (uint8_t)(1 << b);
    m.r[m.nregs].reg  = reg;
    m.r[m.nregs].d[0] = (uint8_t)(lo ^ hi);
    m.r[m.nregs].d[1] = lo;
    m.r[m.nregs].d[2] = hi;
    m.nregs++;
}

static void mcu_reg_raw(int b, uint8_t reg, uint8_t b0, uint8_t b1, uint8_t b2)
{
    m.r[m.nregs].ch   = (uint8_t)(1 << b);
    m.r[m.nregs].reg  = reg;
    m.r[m.nregs].d[0] = b0;
    m.r[m.nregs].d[1] = b1;
    m.r[m.nregs].d[2] = b2;
    m.nregs++;
}

int i2c_init(uint8_t id)
{
    (void)id;
    m.inits++;
    return m.init_fail ? -1 : 0;
}
int i2c_deinit(uint8_t id)
{
    (void)id;
    return 0;
}

int i2c_write(uint8_t id, uint16_t addr, uint8_t reg, uint8_t *v, uint8_t n)
{
    (void)id;
    (void)v;
    if (m.ntx < MAX_TX) {
        m.tx[m.ntx].wr   = 1;
        m.tx[m.ntx].addr = addr;
        m.tx[m.ntx].reg  = reg;
        m.tx[m.ntx].n    = n;
        m.ntx++;
    }
    if (addr == TCA9546A) {
        if (m.mux_fail)
            return -1;
        m.sel = reg;
    }
    return 0;
}

int i2c_read(uint8_t id, uint16_t addr, uint8_t reg, uint8_t *v, uint8_t n)
{
    (void)id;
    if (m.ntx < MAX_TX) {
        m.tx[m.ntx].wr   = 0;
        m.tx[m.ntx].addr = addr;
        m.tx[m.ntx].reg  = reg;
        m.tx[m.ntx].n    = n;
        m.ntx++;
    }
    if (addr != MCU || m.sel < 0)
        return -1;
    for (int i = 0; i < m.nregs; i++)
        if (m.r[i].ch == (uint8_t)m.sel && m.r[i].reg == reg) {
            if (m.r[i].fail)
                return -1;
            memcpy(v, m.r[i].d, n > 3 ? 3 : n);
            return 0;
        }
    return -1;
}

static int mux_write_of(int b)
{
    for (int i = 0; i < m.ntx; i++)
        if (m.tx[i].wr && m.tx[i].addr == TCA9546A && m.tx[i].reg == (uint8_t)(1 << b))
            return i;
    return -1;
}
static int reads_of(uint8_t reg)
{
    int n = 0;
    for (int i = 0; i < m.ntx; i++)
        if (!m.tx[i].wr && m.tx[i].addr == MCU && m.tx[i].reg == reg)
            n++;
    return n;
}
static int none(const hashmcu_temps *t)
{
    return t->outlet_degc == ASIC_TEMP_NONE && t->inlet_degc == ASIC_TEMP_NONE;
}

static uint16_t degc_word(int degc)
{
    int counts = degc * 16;
    return (uint16_t)(counts & 0x0fff);
}

int main(void)
{
    hashmcu_temps t;

    if (!CAP_HB_MCU) {
        mock_reset();
        CHECK(hal_hbmcu() == NULL, "no hbmcu ops table on a product without the MCUs");
        EQI(hashmcu_init(), 0, "init reports 0 boards");
        EQI(hashmcu_count(), 0, "count 0");
        EQI(hashmcu_read(0, &t), -1, "read rc -1");
        CHECK(none(&t), "read leaves the sentinel, never a plausible temperature");
        EQI(m.inits, 0, "the MCU bus is never even opened");
        EQI(m.ntx, 0, "no i2c transaction");
        if (fails) {
            printf("test_hashmcu: %d FAILED\n", fails);
            return 1;
        }
        printf("test_hashmcu: OK (CAP_HB_MCU 0 — degradation contract)\n");
        return 0;
    }

    mock_reset();
    EQI(hashmcu_init(), HBOARD_COUNT, "init reports every hashboard");
    EQI(hashmcu_count(), HBOARD_COUNT, "count == HBOARD_COUNT");
    EQI(m.inits, 1, "init opens the MCU bus");
    EQI(m.ntx, 0, "init issues no i2c transaction");

    mock_reset();
    hashmcu_init();
    mcu_reg(0, R_OUT, degc_word(40));
    mcu_reg(0, R_IN, degc_word(33));
    EQI(hashmcu_read(0, &t), 0, "good read rc");
    EQI(t.outlet_degc, 40, "outlet 40 degC");
    EQI(t.inlet_degc, 33, "inlet 33 degC");
    EQI(m.tx[0].wr, 1, "first transaction is the mux select");
    EQI(m.tx[0].addr, TCA9546A, "mux at 0x70");
    EQI(m.tx[0].reg, 1 << 0, "board 0 -> mux channel bit 1<<0");
    EQI(m.tx[0].n, 0, "mux select is a bare one-byte write (no register)");
    EQI(m.tx[1].addr, MCU, "MCU slave 0x10");
    EQI(m.tx[1].reg, R_OUT, "outlet register 0x15");
    EQI(m.tx[1].n, 3, "3-byte framed read");
    EQI(m.tx[2].reg, R_IN, "inlet register 0x16");
    EQI(m.tx[2].n, 3, "3-byte framed read");
    EQI(m.ntx, 3, "one select + two register reads");

    struct {
        uint16_t    word;
        int         degc;
        const char *msg;
    } sc[] = {
        { 0x0600, 96, "0x0600 -> 96 degC (over the 95 degC arm)" },
        { 0x05F0, 95, "0x05F0 -> 95 degC (not over)" },
        { 0x05FF, 95, "0x05FF -> 95 degC (truncated, not rounded to 96)" },
        { 0x0000, 0, "0x0000 -> 0 degC" },
        { 0x0F60, -10, "0x0F60 -> -10 degC (12-bit two's complement)" },
        { 0x0FFF, 0, "0x0FFF -> 0 degC (-1 count truncates to 0)" },
    };
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        mock_reset();
        hashmcu_init();
        mcu_reg(0, R_OUT, sc[i].word);
        mcu_reg(0, R_IN, sc[i].word);
        EQI(hashmcu_read(0, &t), 0, sc[i].msg);
        EQI(t.outlet_degc, sc[i].degc, sc[i].msg);
    }

    mock_reset();
    hashmcu_init();
    EQI(hashmcu_read(0, &t), -1, "silent MCU -> rc -1");
    CHECK(none(&t), "silent MCU -> -273 on both fields, not 0");
    EQI(reads_of(R_IN), 0, "silent MCU -> fail-fast, inlet not attempted");
    EQI(m.ntx, 2, "silent MCU -> select + one blocked read only");

    mock_reset();
    hashmcu_init();
    mcu_reg(0, R_OUT, degc_word(88));
    mcu_reg(0, R_IN, degc_word(50));
    EQI(hashmcu_read(0, &t), 0, "pre-stale read rc");
    EQI(t.outlet_degc, 88, "pre-stale outlet 88");
    m.r[0].fail = m.r[1].fail = 1;
    EQI(hashmcu_read(0, &t), -1, "gone-quiet rc -1");
    CHECK(none(&t), "gone-quiet -> sentinel, NOT the stale 88");

    mock_reset();
    hashmcu_init();
    mcu_reg_raw(0, R_OUT, 0x00, 0x80, 0x02);
    mcu_reg(0, R_IN, degc_word(30));
    EQI(hashmcu_read(0, &t), -1, "bad checksum -> rc -1");
    EQI(t.outlet_degc, ASIC_TEMP_NONE, "bad checksum -> sentinel");

    mock_reset();
    hashmcu_init();
    mcu_reg(0, R_OUT, 0xFFFF);
    EQI(hashmcu_read(0, &t), -1, "0xFFFF -> rc -1");
    EQI(t.outlet_degc, ASIC_TEMP_NONE, "0xFFFF -> sentinel, not 4095 counts");

    mock_reset();
    hashmcu_init();
    m.mux_fail = 1;
    EQI(hashmcu_read(0, &t), -1, "mux NAK -> rc -1");
    CHECK(none(&t), "mux NAK -> sentinel");
    EQI(m.ntx, 1, "mux NAK -> no MCU transaction attempted");

    mock_reset();
    hashmcu_init();
    for (int b = 0; b < HBOARD_COUNT; b++) {
        mcu_reg(b, R_OUT, degc_word(60 + b));
        mcu_reg(b, R_IN, degc_word(20 + b));
    }
    for (int b = 0; b < HBOARD_COUNT; b++) {
        char msg[64];
        EQI(hashmcu_read(b, &t), 0, "per-board read rc");
        snprintf(msg, sizeof msg, "board %d outlet is its own (%d)", b, 60 + b);
        EQI(t.outlet_degc, 60 + b, msg);
        snprintf(msg, sizeof msg, "board %d inlet is its own (%d)", b, 20 + b);
        EQI(t.inlet_degc, 20 + b, msg);
        snprintf(msg, sizeof msg, "board %d selected mux channel 1<<%d", b, b);
        CHECK(mux_write_of(b) >= 0, msg);
    }
    EQI(m.ntx, HBOARD_COUNT * 3, "HBOARD_COUNT x (select + 2 reads)");

    mock_reset();
    hashmcu_init();
    mcu_reg(0, R_OUT, degc_word(70));
    mcu_reg(0, R_IN, degc_word(25));
    EQI(hashmcu_read(0, &t), 0, "live board rc");
    EQI(t.outlet_degc, 70, "live board outlet");
    EQI(hashmcu_read(HBOARD_COUNT - 1, &t), HBOARD_COUNT == 1 ? 0 : -1, "last board rc");
    if (HBOARD_COUNT > 1)
        CHECK(none(&t), "silent last board -> sentinel, not board 0's 70");

    mock_reset();
    hashmcu_init();
    EQI(hashmcu_read(-1, &t), -1, "board -1 refused");
    CHECK(none(&t), "board -1 -> sentinel");
    EQI(hashmcu_read(HBOARD_COUNT, &t), -1, "board HBOARD_COUNT refused");
    CHECK(none(&t), "board HBOARD_COUNT -> sentinel");
    EQI(m.ntx, 0, "out-of-range issues no i2c transaction");

    mock_reset();
    m.init_fail = 1;
    EQI(hashmcu_init(), 0, "no MCU bus -> init reports 0 boards");
    EQI(hashmcu_count(), 0, "no MCU bus -> count 0");
    EQI(hashmcu_read(0, &t), -1, "no MCU bus -> read rc -1");
    CHECK(none(&t), "no MCU bus -> sentinel");
    EQI(m.ntx, 0, "no MCU bus -> no i2c transaction");

    if (fails) {
        printf("test_hashmcu: %d FAILED\n", fails);
        return 1;
    }
    printf("test_hashmcu: OK (HBOARD_COUNT %d)\n", (int)HBOARD_COUNT);
    return 0;
}
