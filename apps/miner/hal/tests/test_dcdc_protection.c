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

#include "psu_ops.h"
#include "gpio_dev.h"
#include "i2c_dev.h"
#include "osif.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PD   0x42 /* USB PD controller I2C address */
#define INA  0x40 /* INA226 input meter I2C address */
#define DCDC 0x48 /* DC-DC converter I2C address */

static uint8_t  caps[6];  /* PD source capability registers 0x6A..0x6F */
static uint8_t  rail_reg; /* DC-DC output voltage step register 0xF8 */
static uint16_t bus_raw;  /* INA226 bus voltage register, 1.25 mV per LSB */
/* address and register whose reads fail; failures left, negative = always */
static int fail_read_addr, fail_read_reg, fail_read_left;
static int fail_write_addr, fail_write_reg; /* address and register whose writes fail, -1 = none */
static int fail_init;
/* power enable GPIO level; non-zero fails all GPIO calls; count of high writes */
static int pin, gpio_fail, energized;
/* non-zero fails setting the direction, writing high, writing low */
static int direction_fail, high_write_fail, low_write_fail;
/* last PD capability select value, -1 = none; count of PD request commands */
static int pdo_select, go_writes;
static int caps_absent, caps_before_cfg, cfg_written;
static int slept;
static int waiting_logs, no_cap_errs;

int i2c_init(uint8_t bus)
{
    (void)bus;
    return fail_init ? -1 : 0;
}
int i2c_read(uint8_t bus, uint16_t addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    (void)bus;
    if (addr == fail_read_addr && reg == fail_read_reg && fail_read_left) {
        if (fail_read_left > 0)
            fail_read_left--;
        return -1;
    }
    memset(data, 0, len);
    if (addr == PD && reg >= 0x6a && reg <= 0x6f) {
        caps_before_cfg += !cfg_written;
        if (caps_absent > 0)
            caps_absent--;
        else
            *data = caps[reg - 0x6a];
    }
    if (addr == DCDC && reg == 0xf8)
        *data = rail_reg;
    if (addr == INA && reg == 0x02 && len == 2) {
        data[0] = (uint8_t)(bus_raw >> 8);
        data[1] = (uint8_t)bus_raw;
    }
    return 0;
}
int i2c_write(uint8_t bus, uint16_t addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    (void)bus;
    (void)len;
    if (addr == fail_write_addr && reg == fail_write_reg)
        return -1;
    if (addr == DCDC && reg == 0xf8)
        rail_reg = *data;
    if (addr == PD && reg == 0x02 && *data == 0x1b)
        cfg_written = 1;
    if (addr == PD && reg == 0x19)
        pdo_select = *data;
    if (addr == PD && reg == 0x18)
        go_writes++;
    return 0;
}
void gpio_set_direction(int gpio, int dir)
{
    (void)gpio;
    if (!gpio_fail && !direction_fail)
        pin = dir == GPIO_OUT_HIGH;
}
void gpio_set_value(int gpio, int value)
{
    (void)gpio;
    if (gpio_fail || (value == GPIO_HIGH ? high_write_fail : low_write_fail))
        return;
    pin = value;
    if (value == GPIO_HIGH)
        energized++;
}
int gpio_set_direction_checked(int gpio, int dir)
{
    gpio_set_direction(gpio, dir);
    return gpio_fail || direction_fail ? -1 : 0;
}
int gpio_set_value_checked(int gpio, int value)
{
    gpio_set_value(gpio, value);
    return gpio_fail || (value == GPIO_HIGH ? high_write_fail : low_write_fail) ? -1 : 0;
}
unsigned int sleep(unsigned int seconds)
{
    (void)seconds;
    slept++;
    return 0;
}
void osif_log(osif_loglvl_t level, const char *fmt, ...)
{
    if (level == OSIF_LOG_INFO && !strcmp(fmt, "Power: waiting for the adapter capabilities"))
        waiting_logs++;
    if (level == OSIF_LOG_ERR && !strcmp(fmt, "Power: adapter offers no usable voltage"))
        no_cap_errs++;
}

static void bus_mv(unsigned mv)
{
    bus_raw = (uint16_t)(mv * 4 / 5);
}

static void reset(void)
{
    memset(caps, 0, sizeof caps);
    caps[4] = 0x80 | 50;
    bus_mv(20000);
    fail_read_addr = fail_read_reg = fail_write_addr = fail_write_reg = -1;
    fail_read_left                                                    = 0;
    fail_init = gpio_fail = energized = direction_fail = high_write_fail = low_write_fail = 0;
    pdo_select                                                                            = -1;
    go_writes                                                                             = 0;
    pin                                                                                   = 1;
    caps_absent = caps_before_cfg = cfg_written = slept = waiting_logs = no_cap_errs = 0;
}

static void fail_read(int addr, int reg, int times)
{
    fail_read_addr = addr;
    fail_read_reg  = reg;
    fail_read_left = times;
}
static void fail_write(int addr, int reg)
{
    fail_write_addr = addr;
    fail_write_reg  = reg;
}

static void test_rail(const hal_psu_ops_t *psu)
{
    reset();
    assert(psu->rail_on(VOL_ENUM) == HAL_PSU_RAIL_OK && pin == GPIO_HIGH);
    assert(psu->rail_off() == HAL_PSU_RAIL_OK && pin == GPIO_LOW);

    reset();
    fail_write(DCDC, 0xf8);
    assert(psu->rail_off() == HAL_PSU_RAIL_OK && pin == GPIO_LOW);
    reset();
    fail_read(DCDC, 0xf8, -1);
    assert(psu->rail_off() == HAL_PSU_RAIL_OK && pin == GPIO_LOW);
    reset();
    fail_init = 1;
    assert(psu->rail_off() == HAL_PSU_RAIL_OK && pin == GPIO_LOW);

    reset();
    gpio_fail = 1;
    assert(psu->rail_off() != HAL_PSU_RAIL_OK);
    reset();
    low_write_fail = 1;
    assert(psu->rail_off() != HAL_PSU_RAIL_OK);
    assert(psu->rail_on(VOL_ENUM) != HAL_PSU_RAIL_OK && !energized);

    static const int soft[][2] = {
        { DCDC, 0xf8 },
        { INA, 0x00 },
        { INA, 0x05 },
        { PD, 0x02 },
        { PD, 0x18 },
        { PD, 0x19 },
    };
    for (unsigned i = 0; i < sizeof soft / sizeof soft[0]; i++) {
        reset();
        fail_write(soft[i][0], soft[i][1]);
        assert(psu->rail_on(VOL_ENUM) == HAL_PSU_RAIL_OK && energized == 1);
    }
    reset();
    fail_read(DCDC, 0xf8, -1);
    assert(psu->rail_on(VOL_ENUM) == HAL_PSU_RAIL_OK && energized == 1);
    reset();
    fail_init = 1;
    assert(psu->rail_on(VOL_ENUM) == HAL_PSU_RAIL_OK && energized == 1);
    reset();
    assert(psu->rail_on(VOL_ENUM) == HAL_PSU_RAIL_OK && go_writes == 0);

    reset();
    gpio_fail = 1;
    assert(psu->rail_on(VOL_ENUM) != HAL_PSU_RAIL_OK && !energized);
    reset();
    direction_fail = 1;
    assert(psu->rail_on(VOL_ENUM) != HAL_PSU_RAIL_OK && !energized);
    reset();
    high_write_fail = 1;
    assert(psu->rail_on(VOL_ENUM) != HAL_PSU_RAIL_OK && !energized && pin == GPIO_LOW);
}

static void test_pd_budget(const hal_psu_ops_t *psu)
{
    uint16_t watts = 0;
    assert(psu->pout_limit != NULL);

    reset();
    caps[5] = 0x80 | 50;
    bus_mv(28045);
    assert(psu->pout_limit(&watts) == 0 && watts == 140);
    assert(pdo_select == 0xc0 && go_writes == 1);
    assert(pin == GPIO_LOW && !energized);

    reset();
    caps[5] = 0x80 | 50;
    bus_mv(27400);
    assert(psu->pout_limit(&watts) == 0 && watts == 135);
    bus_mv(27500);
    assert(psu->pout_limit(&watts) == 0 && watts == 135);
    bus_raw = 22001;
    assert(psu->pout_limit(&watts) == 0 && watts == 140);

    reset();
    assert(psu->pout_limit(&watts) == 0 && watts == 100 && pdo_select == 0x28);

    reset();
    caps[5] = 0x80 | 50;
    fail_read(PD, 0x6f, -1);
    assert(psu->pout_limit(&watts) == 0 && watts == 100 && pdo_select == 0x28);

    reset();
    caps[5] = 0x80;
    assert(psu->pout_limit(&watts) != 0);
    reset();
    memset(caps, 0, sizeof caps);
    assert(psu->pout_limit(&watts) != 0 && go_writes == 0);

    reset();
    fail_write(PD, 0x18);
    assert(psu->pout_limit(&watts) != 0);
    reset();
    fail_write(PD, 0x19);
    assert(psu->pout_limit(&watts) == 0 && watts == 100);
    reset();
    fail_write(PD, 0x02);
    assert(psu->pout_limit(&watts) == 0 && watts == 100);

    reset();
    fail_read(INA, 0x02, 4);
    assert(psu->pout_limit(&watts) == 0 && watts == 100);
    reset();
    fail_read(INA, 0x02, 5);
    assert(psu->pout_limit(&watts) != 0);
    reset();
    bus_mv(0);
    assert(psu->pout_limit(&watts) != 0);
}

static void test_pd_caps_wait(const hal_psu_ops_t *psu)
{
    uint16_t watts = 0;

    reset();
    caps_absent = 3 * 6;
    assert(psu->pout_limit(&watts) == 0 && watts == 100 && pdo_select == 0x28);
    assert(caps_before_cfg == 0 && waiting_logs == 1 && no_cap_errs == 0 && slept == 3 + 1);

    reset();
    memset(caps, 0, sizeof caps);
    assert(psu->pout_limit(&watts) == -1 && watts == 0 && go_writes == 0);
    assert(waiting_logs == 1 && no_cap_errs == 1 && slept == 5);

    reset();
    assert(psu->pout_limit(&watts) == 0 && watts == 100);
    assert(caps_before_cfg == 0 && waiting_logs == 0 && slept == 1);
}

static void test_monitor_retry(const hal_psu_ops_t *psu)
{
    hal_psu_reading r;
    reset();
    bus_mv(28000);
    fail_read(INA, 0x02, 4);
    assert(psu->read(&r) == 0 && r.vout_units == 28000);
    reset();
    fail_read(INA, 0x02, 5);
    assert(psu->read(&r) != 0);
}

static void test_vout_fault(const hal_psu_ops_t *psu)
{
    assert(psu->flags == (HAL_PSU_F_OFF_CONFIRM | HAL_PSU_F_LATCH_ON_FAIL));
    assert(psu->vout_fault != NULL);
    assert(!psu->vout_fault(POWER_VALID_VOLT_MIN, 0, 0));
    assert(!psu->vout_fault(POWER_VALID_VOLT_MIN + 1, -1, -1));
    assert(psu->vout_fault(POWER_VALID_VOLT_MIN - 1, 0, 0));
    assert(psu->vout_fault(POWER_VALID_VOLT_MIN - 1, 100, 1200));
    assert(psu->vout_fault(POWER_VALID_VOLT_MIN - 1, -1, -1));
    assert(psu->vout_fault(0, 0, 0));
    assert(psu->vout_fault(-1, -1, -1));
}

int main(void)
{
    const hal_psu_ops_t *psu = hal_psu();
    test_rail(psu);
    test_pd_budget(psu);
    test_pd_caps_wait(psu);
    test_monitor_retry(psu);
    test_vout_fault(psu);
    puts("nano3s DC-DC protection OK");
    return 0;
}
