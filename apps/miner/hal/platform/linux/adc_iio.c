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

#include "adc_iio.h"

#include <stdio.h>

int adc_get_raw(int channel, int *adc_raw)
{
    if (!adc_raw || channel < 0)
        return -1;

    const char *root = IIO_DEVICE_DEFAULT;

    char path[256];
    int  n = snprintf(path, sizeof path, "%s/in_voltage%d_raw", root, channel);
    if (n < 0 || (size_t)n >= sizeof path)
        return -1;

    FILE *f = fopen(path, "re");
    if (!f)
        return -1;

    int raw;
    int got = fscanf(f, "%d", &raw);
    fclose(f);
    if (got != 1)
        return -1;

    *adc_raw = raw;
    return 0;
}
