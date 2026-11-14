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
#include <string.h>

board_power stub_power;    /* Reading board_power_read() returns */
int         stub_power_rc; /* Return code of board_power_read() */

int osif_log_op_sink_open(void)
{
    return -1;
}

int board_sensors_init(void)
{
    return 0;
}
int board_power_read(board_power *out)
{
    if (out)
        *out = stub_power;
    return stub_power_rc;
}
int board_power_set_volt(uint16_t mv)
{
    (void)mv;
    return 0;
}
int board_ntc_read(int *degc)
{
    if (degc)
        *degc = 0;
    return 0;
}
uint8_t dcdc_mv_to_reg(uint16_t mv)
{
    (void)mv;
    return 0;
}
