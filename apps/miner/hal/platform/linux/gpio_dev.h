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

#ifndef GPIO_DEV_H
#define GPIO_DEV_H

#include "boardconf.h"

#define GPIO_ROOT "/sys/class/gpio"

enum gpio_direction {
    GPIO_IN       = 0,
    GPIO_OUT      = 1, /* sysfs "out": output, initially low, same as GPIO_OUT_LOW */
    GPIO_OUT_LOW  = 2, /* output, initially low */
    GPIO_OUT_HIGH = 3, /* output, initially high */
};

enum gpio_value {
    GPIO_LOW  = 0,
    GPIO_HIGH = 1,
};

void gpio_set_direction(int gpio, int dir);
void gpio_set_value(int gpio, int val);
int  gpio_set_direction_checked(int gpio, int dir);
int  gpio_set_value_checked(int gpio, int val);

#endif /* GPIO_DEV_H */
