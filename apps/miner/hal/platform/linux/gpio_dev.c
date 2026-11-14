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

#include "gpio_dev.h"

#include <stdio.h>
#include <string.h>

static int write_str(const char *path, const char *val)
{
    FILE *f = fopen(path, "we");
    if (!f)
        return -1;
    int rc = fputs(val, f) >= 0 ? 0 : -1;
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

static void gpio_export(int gpio)
{
    char path[256], num[16];
    snprintf(path, sizeof path, "%s/export", GPIO_ROOT);
    snprintf(num, sizeof num, "%d", gpio);
    (void)write_str(path, num);
}

int gpio_set_direction_checked(int gpio, int dir)
{
    static const char *const s[] = {
        [GPIO_IN]       = "in",
        [GPIO_OUT]      = "out",
        [GPIO_OUT_LOW]  = "low",
        [GPIO_OUT_HIGH] = "high",
    };
    if (dir < 0 || dir > GPIO_OUT_HIGH)
        return -1;
    gpio_export(gpio);
    char path[256];
    snprintf(path, sizeof path, "%s/gpio%d/direction", GPIO_ROOT, gpio);
    return write_str(path, s[dir]);
}

int gpio_set_value_checked(int gpio, int val)
{
    char path[256];
    snprintf(path, sizeof path, "%s/gpio%d/value", GPIO_ROOT, gpio);
    return write_str(path, val ? "1" : "0");
}

void gpio_set_direction(int gpio, int dir)
{
    (void)gpio_set_direction_checked(gpio, dir);
}

void gpio_set_value(int gpio, int val)
{
    (void)gpio_set_value_checked(gpio, val);
}
