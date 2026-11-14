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
#include "psu_ops.h"
#include "temp_ops.h"

int board_sensors_init(void)
{
    const hal_psu_ops_t *psu = hal_psu();
    return (psu && psu->probe) ? psu->probe() : -1;
}

int board_power_read(board_power *out)
{
    const hal_psu_ops_t *psu = hal_psu();
    hal_psu_reading      r   = { 0 };
    *out                     = (board_power){ 0 };
    if (!psu || !psu->read || psu->read(&r) != 0)
        return -1;
    out->vout_mv    = r.vout_units;
    out->iout_ma    = r.iout_units;
    out->pout_w     = r.pout_w;
    out->voutcmd_mv = r.voutcmd_units;
    out->err        = r.err;
    out->vout12     = r.vout12_units;
    return 0;
}

int board_power_set_volt(uint16_t counts)
{
    const hal_psu_ops_t *psu = hal_psu();
    return (psu && psu->set_volt) ? psu->set_volt(counts) : -1;
}

int board_power_rail_on(uint16_t counts)
{
    const hal_psu_ops_t *psu = hal_psu();
    return (psu && psu->rail_on) ? psu->rail_on(counts) : HAL_PSU_RAIL_E_UNSUPPORTED;
}

int board_power_rail_off(void)
{
    const hal_psu_ops_t *psu = hal_psu();
    return (psu && psu->rail_off) ? psu->rail_off() : HAL_PSU_RAIL_E_UNSUPPORTED;
}

const char *hal_psu_rail_strerror(int rc)
{
    switch (rc) {
    case HAL_PSU_RAIL_OK:
        return "ok";
    case HAL_PSU_RAIL_E_UNSUPPORTED:
        return "this board commands no rail";
    case HAL_PSU_RAIL_E_NODEV:
        return "the power bus would not open";
    case HAL_PSU_RAIL_E_REFUSED:
        return "the device refused the request";
    case HAL_PSU_RAIL_E_UNCONFIRMED:
        return "commanded, never confirmed";
    default:
        return "unknown";
    }
}

int board_ntc_read(int *degc)
{
    const hal_temp_ops_t *tmp = hal_temp();
    return (tmp && tmp->read) ? tmp->read(degc) : -1;
}
