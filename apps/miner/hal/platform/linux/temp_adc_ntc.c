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

#include "temp_ops.h"
#include "boardconf.h"

#if IMPL_BOARD_TEMP_IS_ADC_NTC

#include <math.h>
#include "adc_iio.h"
#include "temp_ntc.h"

static int ntc_read(int *degc)
{
    int raw = 0;
    if (adc_get_raw(BOARD_NTC_ADC_CH, &raw) < 0)
        return -1;
    float t = ntc_calc_temp(raw, BOARD_NTC_R_PULLUP, BOARD_NTC_R_25, BOARD_NTC_B_CONST);
    if (isnan(t) || t < -50.0f || t > 200.0f)
        return -1;
    *degc = (int)lroundf(t);
    return 0;
}

static const hal_temp_ops_t ntc_ops = {
    .read = ntc_read,
};

const hal_temp_ops_t *hal_temp(void)
{
    return &ntc_ops;
}

#endif /* IMPL_BOARD_TEMP_IS_ADC_NTC */
