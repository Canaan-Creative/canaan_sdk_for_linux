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
#include "boardconf.h"

#if IMPL_PSU_IS_DCDC_PD

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "board_sensors.h"
#include "gpio_dev.h"
#include "i2c_dev.h"
#include "osif.h"

#define INA226_ADDR    0x40 /* INA226 input meter i2c address */
#define DCDC_ADDR      0x48
#define INA226_CFG_REG 0x00
#define INA226_CAL_REG 0x05
#define INA226_BUSV    0x02 /* INA226 bus voltage register, 1.25 mV per LSB */
#define INA226_CUR     0x04 /* INA226 current register, 0.1831 mA per LSB */
#define INA226_MAN_ID  0xFE
/* DC-DC output voltage step register, 26 mV per step, bit 7 = below DCDC_VOL_REF */
#define DCDC_VOUT_REG  0xF8
#define DCDC_VOL_REF   3600           /* DC-DC output voltage at step 0, mV */
#define INA226_CFG_VAL { 0x47, 0x27 } /* INA226 configuration register value, MSB first */
#define INA226_CAL_VAL { 0x0A, 0xEC } /* INA226 calibration register value, MSB first */

#define PD_ADDR           0x42
#define PD_CFG_REG        0x02
#define PD_CFG_VAL        0x1b
#define PD_PDO_SELECT     0x19 /* PD source capability select register */
#define PD_GO_COMMAND     0x18 /* PD request command register */
#define PD_GO_SELECT      0x01 /* PD request command: request the selected capability */
#define POWER_ENABLE_GPIO 34
#define INA226_READ_TRIES 5 /* INA226 read attempts per register */
#define PD_CAP_WAIT_S     5
#define PDO_COUNT         (sizeof PDO / sizeof PDO[0])

/* PD source capabilities, lowest voltage first */
static const struct {
    uint8_t reg;
    int     volts; /* capability voltage, V */
    uint8_t sel;   /* PD_PDO_SELECT value that requests it */
} PDO[] = {
    { 0x6A, 5, 0x08 },
    { 0x6B, 9, 0x10 },
    { 0x6C, 12, 0x18 },
    { 0x6D, 15, 0x20 },
    { 0x6E, 20, 0x28 },
    { 0x6F, 28, 0xC0 },
};

static int ina_get(uint8_t reg, uint16_t *out)
{
    uint8_t d[2];
    for (int i = 0; i < INA226_READ_TRIES; i++) {
        if (i2c_read(I2C_PD, INA226_ADDR, reg, d, 2) == 0) {
            *out = (uint16_t)((d[0] << 8) | d[1]);
            return 0;
        }
    }
    return -1;
}

static int ina226_setup(void)
{
    uint8_t cfg[2] = INA226_CFG_VAL, cal[2] = INA226_CAL_VAL;
    if (i2c_write(I2C_PD, INA226_ADDR, INA226_CFG_REG, cfg, 2) != 0)
        return -1;
    if (i2c_write(I2C_PD, INA226_ADDR, INA226_CAL_REG, cal, 2) != 0)
        return -2;
    return 0;
}

static int dcdc_probe(void)
{
    if (i2c_init(I2C_PD) != 0) {
        osif_log(OSIF_LOG_ERR, "Power: cannot open the PD bus " I2C_PD_DEV ": %s", strerror(errno));
        return -1;
    }
    i2c_init(I2C_POWER_E2P);
    if (ina226_setup() == -1) {
        osif_log(OSIF_LOG_ERR, "Power: input meter (INA226) did not accept its configuration");
        return -1;
    }
    return 0;
}

static int dcdc_read(hal_psu_reading *out)
{
    uint16_t bv = 0, cur = 0;
    *out = (hal_psu_reading){ 0 };
    if (ina_get(INA226_BUSV, &bv) != 0)
        return -1;
    ina_get(INA226_CUR, &cur);
    double mv       = bv * 1.25;
    double ma       = (cur & 0x8000) ? 0.0 : cur * 0.1831;
    out->vout_units = (uint16_t)(mv + 0.5);
    out->iout_units = (uint16_t)(ma + 0.5);
    out->pout_w     = (uint16_t)(mv * ma / 1e6 + 0.5);
    uint8_t rb      = 0;
    if (i2c_read(I2C_POWER_E2P, DCDC_ADDR, DCDC_VOUT_REG, &rb, 1) == 0)
        out->voutcmd_units =
                (uint16_t)((rb & 0x80) ? DCDC_VOL_REF - (rb & 0x7f) * 26 : DCDC_VOL_REF + rb * 26);
    return 0;
}

uint8_t dcdc_mv_to_reg(uint16_t mv)
{
    if (mv < VOL_MIN || mv > VOL_MAX) {
        uint16_t was = mv;
        mv           = (mv < VOL_MIN) ? (uint16_t)VOL_MIN : (uint16_t)VOL_MAX;
        osif_log(OSIF_LOG_WARN,
                "Power: requested hashboard voltage %u outside %u..%u; "
                "using %u",
                was, (unsigned)VOL_MIN, (unsigned)VOL_MAX, mv);
    }
    int diff = (int)mv - DCDC_VOL_REF;
    return (diff < 0) ? (uint8_t)(0x80 - diff / 26) : (uint8_t)(diff / 26);
}

static int dcdc_set_volt(uint16_t units)
{
    uint8_t b = dcdc_mv_to_reg(units);

    if (i2c_init(I2C_POWER_E2P) != 0)
        return -1;
    return i2c_write(I2C_POWER_E2P, DCDC_ADDR, DCDC_VOUT_REG, &b, 1);
}

static void dcdc_report(unsigned before_w)
{
    hal_psu_reading r;
    if (dcdc_read(&r) != 0)
        return;
    osif_log(OSIF_LOG_INFO, "Power input: %u mV, %u mA (%u W), %u W (before PD connected)",
            r.vout_units, r.iout_units, r.pout_w, before_w);
}

static int pd_source(uint16_t *ma)
{
    *ma = 0;
    for (int index = (int)PDO_COUNT - 1; index >= 0; index--) {
        uint8_t capability = 0;
        if (i2c_read(I2C_PD, PD_ADDR, PDO[index].reg, &capability, 1) != 0 || !(capability & 0x80))
            continue;
        *ma = (uint16_t)((capability & 0x7f) * 100);
        return *ma ? index : -1;
    }
    return -1;
}

static int pd_pout_limit(uint16_t *watts)
{
    *watts = 0;
    gpio_set_direction(POWER_ENABLE_GPIO, GPIO_OUT_LOW);
    (void)i2c_init(I2C_PD);
    uint8_t v = PD_CFG_VAL;
    if (i2c_write(I2C_PD, PD_ADDR, PD_CFG_REG, &v, 1) != 0)
        osif_log(OSIF_LOG_WARN, "Power: PD config failed; continuing");
    uint16_t ma;
    int      best = pd_source(&ma);
    if (best < 0)
        osif_log(OSIF_LOG_INFO, "Power: waiting for the adapter capabilities");
    for (int waited = 0; best < 0 && waited < PD_CAP_WAIT_S; waited++) {
        sleep(1);
        best = pd_source(&ma);
    }
    if (best < 0) {
        osif_log(OSIF_LOG_ERR, "Power: adapter offers no usable voltage");
        return -1;
    }
    v = PDO[best].sel;
    (void)i2c_write(I2C_PD, PD_ADDR, PD_PDO_SELECT, &v, 1);
    v = PD_GO_SELECT;
    if (i2c_write(I2C_PD, PD_ADDR, PD_GO_COMMAND, &v, 1) != 0) {
        osif_log(OSIF_LOG_ERR, "Power: PD voltage request failed");
        return -1;
    }
    osif_log(OSIF_LOG_DBG, "Power: request %d V from the adapter (select 0x%02x)", PDO[best].volts,
            PDO[best].sel);
    sleep(1);

    uint16_t bv;
    if (ina_get(INA226_BUSV, &bv) != 0) {
        osif_log(OSIF_LOG_ERR, "Power: input voltage unreadable after PD negotiation");
        return -1;
    }
    uint32_t mv = (uint32_t)bv * 5u / 4u;
    mv          = (mv % 1000u > 500u) ? (mv / 1000u + 1u) * 1000u : (mv / 1000u) * 1000u;
    *watts      = (uint16_t)((uint32_t)ma * mv / 1000000u);
    osif_log(OSIF_LOG_INFO, "Power: PD adapter provides %u W (%u mA at %u mV)", *watts, ma,
            (unsigned)mv);
    return *watts ? 0 : -1;
}

static void dcdc_announce(void)
{
    uint8_t id[2] = { 0, 0 };
    if (i2c_read(I2C_PD, INA226_ADDR, INA226_MAN_ID, id, 2) == 0)
        osif_log(OSIF_LOG_DBG, "Power: input meter id %02x%02x (expect 5449)", id[0], id[1]);
    for (size_t i = 0; i < PDO_COUNT; i++) {
        uint8_t c = 0;
        if (i2c_read(I2C_PD, PD_ADDR, PDO[i].reg, &c, 1) == 0)
            osif_log(OSIF_LOG_DBG, "Power: PD %2d V (reg %02x): %s, max %d mA", PDO[i].volts,
                    PDO[i].reg, (c & 0x80) ? "offered" : "not offered", (c & 0x7f) * 100);
    }
}

static int dcdc_rail_off(void)
{
    int direction_rc = gpio_set_direction_checked(POWER_ENABLE_GPIO, GPIO_OUT_LOW);
    int value_rc     = gpio_set_value_checked(POWER_ENABLE_GPIO, GPIO_LOW);
    if (dcdc_set_volt((uint16_t)VOL_MIN) != 0)
        osif_log(OSIF_LOG_WARN, "Power: could not set hashboard voltage to minimum (%d)", VOL_MIN);
    if (direction_rc != 0 || value_rc != 0) {
        osif_log(OSIF_LOG_ERR,
                "Power: hashboard power-off failed (GPIO %d); "
                "the hashboard may still be powered",
                POWER_ENABLE_GPIO);
        return HAL_PSU_RAIL_E_UNCONFIRMED;
    }
    osif_log(OSIF_LOG_DBG, "Power: hashboard power off requested (GPIO %d)", POWER_ENABLE_GPIO);
    return HAL_PSU_RAIL_OK;
}

static int dcdc_rail_on(uint16_t units)
{
    int off_rc = dcdc_rail_off();
    if (off_rc != HAL_PSU_RAIL_OK)
        return off_rc;
    (void)i2c_init(I2C_PD);

    dcdc_announce();

    if (ina226_setup() != 0)
        osif_log(OSIF_LOG_WARN, "Power: input meter configuration failed; readings may be wrong");
    hal_psu_reading before = { 0 };
    (void)dcdc_read(&before);

    if (dcdc_set_volt(units) != 0)
        osif_log(OSIF_LOG_WARN, "Power: could not set hashboard voltage %u; powering on anyway",
                units);
    osif_log(OSIF_LOG_DBG, "Power: turning hashboard on at voltage %u (GPIO %d)", units,
            POWER_ENABLE_GPIO);
    if (gpio_set_value_checked(POWER_ENABLE_GPIO, GPIO_HIGH) != 0) {
        osif_log(OSIF_LOG_ERR, "Power: hashboard power-on failed (GPIO %d); powering off again",
                POWER_ENABLE_GPIO);
        (void)dcdc_rail_off();
        return HAL_PSU_RAIL_E_UNCONFIRMED;
    }
    osif_log(OSIF_LOG_DBG, "Power: hashboard power on requested");
    sleep(1);
    dcdc_report(before.pout_w);
    return HAL_PSU_RAIL_OK;
}

static int pd_vout_fault(int vout, int pout_w, int vout12)
{
    (void)pout_w;
    (void)vout12;
    return vout < POWER_VALID_VOLT_MIN;
}

static const hal_psu_ops_t dcdc_ops = {
    .probe      = dcdc_probe,
    .read       = dcdc_read,
    .set_volt   = dcdc_set_volt,
    .pout_limit = pd_pout_limit,
    .rail_on    = dcdc_rail_on,
    .rail_off   = dcdc_rail_off,
    .vout_fault = pd_vout_fault,
    .flags      = HAL_PSU_F_OFF_CONFIRM | HAL_PSU_F_LATCH_ON_FAIL,
};

const hal_psu_ops_t *hal_psu(void)
{
    return &dcdc_ops;
}

#endif /* IMPL_PSU_IS_DCDC_PD */
