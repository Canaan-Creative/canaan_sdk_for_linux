/* SPDX-License-Identifier: MIT */
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

#ifndef TEMP_NTC_H
#define TEMP_NTC_H

#include <math.h>
#include <stdint.h>

#define NTC_V_REF     (1.8)                     /* ADC reference voltage, V */
#define NTC_ADC_SCALE (float)(NTC_V_REF / 4096) /* ADC voltage per count, V */
#define NTC_DEGREE_KA (273.15)                  /* 0 degC in K */
#define NTC_T2        (25 + NTC_DEGREE_KA)      /* NTC nominal-resistance temperature, K */

static inline float ntc_calc_temp(int adc_raw, uint32_t pu, uint32_t rnormal, uint32_t rbconst)
{
    if (adc_raw >= 4095)
        return -NTC_DEGREE_KA;
    float vt   = adc_raw * NTC_ADC_SCALE;
    float rt   = pu * vt / (NTC_V_REF - vt);
    float temp = 1 / (log(rt / rnormal) / rbconst + 1 / NTC_T2) - NTC_DEGREE_KA;
    return temp;
}

#endif /* TEMP_NTC_H */
