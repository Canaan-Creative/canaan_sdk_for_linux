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

#include "a3197s_decode.h"
#include <stdio.h>
#include <math.h>

#include "../../tests/include/tst_check.h"

static void pvt_scale_checks(void)
{
    CHECK(fabs(decode_pvt_temp(3461) - 108.38) < 0.05,
            "reference point: code 3461 -> %.2f degC (spec 108.4)", decode_pvt_temp(3461));
    CHECK(decode_pvt_volt(3072) == 900, "reference point: code 3072 -> %u mV (spec 900)",
            decode_pvt_volt(3072));

    for (uint16_t c = 2915; c < 3728; c++) {
        double d = decode_pvt_temp((uint16_t)(c + 1)) - decode_pvt_temp(c);
        if (!(d > 0.27 && d < 0.43)) {
            CHECK(0, "slope at code %u is %.4f degC/LSB, want 0.27..0.43", c, d);
            return;
        }
    }
    CHECK(1, "monotonic, 0.27..0.43 degC/LSB across the whole operating branch");
    CHECK(fabs((decode_pvt_temp(3255) - decode_pvt_temp(3254)) - 0.2777) < 0.001,
            "0.278 degC/LSB around code 3254");

    CHECK(fabs(decode_pvt_temp(3254) - 50.96) < 0.05,
            "code 3254 -> %.2f degC (a typical TMax under load)", decode_pvt_temp(3254));
    CHECK(fabs(decode_pvt_temp(3477) - 113.00) < 0.05,
            "code 3477 -> %.2f degC (the thermal soft-off trip)", decode_pvt_temp(3477));

    CHECK(decode_pvt_temp(2914) == -273.0 && decode_pvt_temp(2915) != -273.0,
            "lower edge of the operating branch is code 2915 (-50 degC)");
    CHECK(decode_pvt_temp(3728) != -273.0 && decode_pvt_temp(3729) == -273.0,
            "upper edge of the operating branch is code 3728 (+200 degC)");

    CHECK(decode_pvt_temp(1783) != -273.0 && decode_pvt_temp(1900) != -273.0 &&
                    decode_pvt_temp(1987) != -273.0,
            "spurious second branch 1783..1987 decodes in-window (e.g. 1900 -> %.1f degC)",
            decode_pvt_temp(1900));
    CHECK(decode_pvt_temp(1782) == -273.0 && decode_pvt_temp(1988) == -273.0,
            "the spurious branch is bounded: 1782 and 1988 sentinel");
}

int main(void)
{
    pvt_scale_checks();

    if (fails) {
        fprintf(stderr, "test_a3197s_temp: %d FAIL\n", fails);
        return 1;
    }
    printf("test_a3197s_temp: all pass\n");
    return 0;
}
