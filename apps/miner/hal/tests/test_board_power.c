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

#include "board_sensors.h"
#include "boardconf.h"
#include "tst_board_expect.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define MAX_REGS 12 /* capacity of the scripted register table */
#define MAX_TX   16 /* capacity of the transfer log */
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            fails++; \
        } \
    } while (0)
#define EQU(got, want, msg) \
    do { \
        unsigned long g_ = (unsigned long)(got), w_ = (unsigned long)(want); \
        if (g_ != w_) { \
            printf("FAIL: %s (got %lu, want %lu)\n", (msg), g_, w_); \
            fails++; \
        } \
    } while (0)
#define INA  0x40 /* INA226 input meter I2C address */
#define DCDC 0x48 /* DC-DC converter I2C address */
#if VOL_MAX < 3450
#define SET_MV   2700
#define SET_CODE 0xA2
#else
#define SET_MV   3450
#define SET_CODE 0x85
#endif

static struct {
    /* scripted reads: device address, register, 2 data bytes, non-zero = read fails */
    struct {
        uint16_t addr;
        uint8_t  reg;
        uint8_t  d[2];
        int      fail;
    } r[MAX_REGS];
    int nregs;
    /* transfer log: 1 = write, address, register, length, first 2 data bytes */
    struct {
        int      wr;
        uint16_t addr;
        uint8_t  reg;
        uint8_t  n;
        uint8_t  d[2];
    } tx[MAX_TX];
    int ntx;
    int inits;
} m;
static int fails;

static void mock_reset(void)
{
    memset(&m, 0, sizeof m);
}

static void reg_set(uint16_t addr, uint8_t reg, uint8_t b0, uint8_t b1)
{
    m.r[m.nregs].addr = addr;
    m.r[m.nregs].reg  = reg;
    m.r[m.nregs].d[0] = b0;
    m.r[m.nregs].d[1] = b1;
    m.nregs++;
}
int i2c_init(uint8_t id)
{
    (void)id;
    m.inits++;
    return 0;
}
int i2c_deinit(uint8_t id)
{
    (void)id;
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
    for (int i = 0; i < m.nregs; i++)
        if (m.r[i].addr == addr && m.r[i].reg == reg) {
            if (m.r[i].fail)
                return -1;
            memcpy(v, m.r[i].d, n > 2 ? 2 : n);
            return 0;
        }
    return -1;
}

int i2c_write(uint8_t id, uint16_t addr, uint8_t reg, uint8_t *v, uint8_t n)
{
    (void)id;
    if (m.ntx < MAX_TX) {
        m.tx[m.ntx].wr   = 1;
        m.tx[m.ntx].addr = addr;
        m.tx[m.ntx].reg  = reg;
        m.tx[m.ntx].n    = n;
        memcpy(m.tx[m.ntx].d, v, n > 2 ? 2 : n);
        m.ntx++;
    }
    return 0;
}

int adc_get_raw(int ch, int *raw)
{
    (void)ch;
    (void)raw;
    return -1;
}

static int reads_of(uint16_t addr, uint8_t reg)
{
    int n = 0;
    for (int i = 0; i < m.ntx; i++)
        if (!m.tx[i].wr && m.tx[i].addr == addr && m.tx[i].reg == reg)
            n++;
    return n;
}
static int writes(void)
{
    int n = 0;
    for (int i = 0; i < m.ntx; i++)
        if (m.tx[i].wr)
            n++;
    return n;
}
static int all_zero(const board_power *p)
{
    return p->vout_mv == 0 && p->iout_ma == 0 && p->pout_w == 0 && p->voutcmd_mv == 0 &&
            p->err == 0;
}

int main(void)
{
    board_power p;

    mock_reset();
    EQU(board_sensors_init(), 0, "init rc");
    EQU(m.inits, 2, "init opens both power buses");
    EQU(m.ntx, 2, "init issues exactly the cfg + calib writes");
    EQU(m.tx[0].wr, 1, "cfg is a write");
    EQU(m.tx[0].addr, INA, "cfg to INA226 0x40");
    EQU(m.tx[0].reg, 0x00, "cfg reg 0x00");
    EQU(m.tx[0].d[0], 0x47, "cfg msb 0x47");
    EQU(m.tx[0].d[1], 0x27, "cfg lsb 0x27 (0x4727)");
    EQU(m.tx[1].reg, 0x05, "calib reg 0x05");
    EQU(m.tx[1].d[0], 0x0A, "calib msb 0x0A");
    EQU(m.tx[1].d[1], 0xEC, "calib lsb 0xEC (0x0AEC)");

    mock_reset();
    reg_set(INA, 0x02, 0x56, 0x31);
    reg_set(INA, 0x04, 0x66, 0x67);
    reg_set(DCDC, 0xF8, 0x81, 0x00);
    EQU(board_power_read(&p), 0, "read rc");
    EQU(p.vout_mv, 27581, "bus voltage x1.25 mV");
    EQU(p.iout_ma, 4800, "current x0.1831 mA");
    EQU(p.pout_w, 132, "pout = vout*iout");
    EQU(p.voutcmd_mv, 3574, "DC-DC 0xF8=0x81 -> 3600-26");
    CHECK(p.vout_mv != (uint16_t)(0x3156 * 1.25), "bus voltage is MSB-first");
    EQU(p.err, 0, "INA226 backend has no fault word -> err 0");
    EQU(m.ntx, 3, "read issues exactly 3 transactions (no fault-word read)");
    EQU(reads_of(INA, 0x02), 1, "reads INA226 bus-V 0x02");
    EQU(reads_of(INA, 0x04), 1, "reads INA226 current 0x04");
    EQU(reads_of(DCDC, 0xF8), 1, "reads DC-DC 0xF8");
    EQU(writes(), 0, "telemetry read writes nothing");

    mock_reset();
    reg_set(INA, 0x02, 0x56, 0x31);
    reg_set(INA, 0x04, 0xFF, 0x00);
    reg_set(DCDC, 0xF8, 0x85, 0x00);
    EQU(board_power_read(&p), 0, "negative-current read rc");
    EQU(p.vout_mv, 27581, "voltage still reported");
    EQU(p.iout_ma, 0, "negative current -> 0 mA");
    EQU(p.pout_w, 0, "negative current -> 0 W");
    EQU(p.err, 0, "still no fault word on this backend");
    EQU(p.voutcmd_mv, 3470, "DC-DC 0xF8=0x85 -> 3600-5*26");

    mock_reset();
    EQU(board_power_read(&p), -1, "INA226 absent -> rc -1");
    CHECK(all_zero(&p), "INA226 absent -> all-zero sentinel");

    mock_reset();
    EQU(board_power_set_volt(SET_MV), 0, "set_volt rc");
    EQU(m.inits, 1, "set_volt opens the DC-DC bus itself");
    EQU(m.ntx, 1, "set_volt is one transaction");
    EQU(m.tx[0].wr, 1, "set_volt writes");
    EQU(m.tx[0].addr, DCDC, "set_volt targets the DC-DC 0x48");
    EQU(m.tx[0].reg, 0xF8, "set_volt reg 0xF8");
    EQU(m.tx[0].d[0], SET_CODE, "set_volt mV -> 0xF8 code");

    if (fails) {
        printf("test_board_power (%s): %d FAILED\n", BOARD_NAME, fails);
        return 1;
    }
    printf("test_board_power (%s): OK\n", BOARD_NAME);
    return 0;
}
