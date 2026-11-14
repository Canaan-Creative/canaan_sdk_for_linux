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

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "temp_ntc.h"

#define PU 10000 /* NTC divider pull-up resistance, ohms */
#define RN 10000 /* NTC resistance at 25 degC, ohms */
#define B  3950  /* NTC B constant, K */

int main(void)
{
    assert(fabsf(ntc_calc_temp(2048, PU, RN, B) - 25.0f) < 0.01f);
    assert(fabsf(ntc_calc_temp(1000, PU, RN, B) - 52.8045f) < 0.01f);
    assert(fabsf(ntc_calc_temp(3000, PU, RN, B) - 3.9397f) < 0.01f);
    assert(fabsf(ntc_calc_temp(4094, PU, RN, B) - (-83.9057f)) < 0.01f);
    assert(ntc_calc_temp(4095, PU, RN, B) == (float)-273.15);
    assert(ntc_calc_temp(5000, PU, RN, B) == (float)-273.15);
    printf("ntc decode goldens OK\n");
    return 0;
}
