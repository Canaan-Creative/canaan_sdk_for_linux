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

#ifndef ASIC_TEMP_PUB_H
#define ASIC_TEMP_PUB_H

#include "boardconf.h"
#include "miner_ipc.h"

#define TELEM_PERIOD_MS 1000u

typedef struct {
    int16_t max, avg; /* hottest and mean chip temperature, degrees C, or ASIC_TEMP_NONE */
    uint8_t valid;    /* chips with a temperature reading, count */
} asic_temp_pub;

static inline asic_temp_pub asic_temp_publish(const double *temp, int chips, int lapped)
{
    asic_temp_pub p   = { ASIC_TEMP_NONE, ASIC_TEMP_NONE, 0 };
    double        sum = 0, max = ASIC_TEMP_NONE;
    int           valid = 0;

    for (int i = 0; i < chips && i < MINER_MAX_CHIPS; i++) {
        if (temp[i] <= ASIC_TEMP_NONE)
            continue;
        sum += temp[i];
        if (temp[i] > max)
            max = temp[i];
        valid++;
    }
    p.valid = (uint8_t)valid;
    p.max   = (int16_t)max;
    p.avg   = (int16_t)(valid && lapped ? sum / valid : ASIC_TEMP_NONE);
    return p;
}

#endif /* ASIC_TEMP_PUB_H */
