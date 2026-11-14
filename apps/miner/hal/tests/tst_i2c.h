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

#ifndef TST_I2C_H
#define TST_I2C_H

#include <fcntl.h>
#include <string.h>

#include "boardconf.h"

static int tst_i2c_open(const char *path, int flags)
{
    if (path && (strcmp(path, I2C_PD_DEV) == 0 || strcmp(path, I2C_POWER_E2P_DEV) == 0))
        return open(TST_I2C_NODE, flags);
#if CAP_HB_MCU
    if (path && strcmp(path, I2C_MCU_DEV) == 0)
        return open(TST_I2C_NODE, flags);
#endif /* CAP_HB_MCU */
    return open(path, flags);
}

#endif /* TST_I2C_H */
