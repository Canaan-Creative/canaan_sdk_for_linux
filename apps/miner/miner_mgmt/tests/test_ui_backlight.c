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

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../hal/ui/ui_backlight.c"

_Static_assert(IMPL_BACKLIGHT_IS_GPIO, "this test pins the nano3s GPIO backlight");

/* Last GPIO level written (-1 before any write) and the direction-set call count */
static int gpio = -1, gpio_dir_calls;
void       gpio_set_direction(int pin, int direction)
{
    assert(pin == BACKLIGHT_GPIO && direction == GPIO_OUT_LOW);
    gpio = 0;
    gpio_dir_calls++;
}
void gpio_set_value(int pin, int value)
{
    assert(pin == BACKLIGHT_GPIO);
    gpio = value;
}
void osif_log(osif_loglvl_t level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

int main(void)
{
    hal_ui_backlight_set(0, 100);
    assert(gpio_dir_calls == 1 && gpio == 0);
    hal_ui_backlight_set(1, 37);
    assert(gpio == 1);
    hal_ui_backlight_set(1, 1);
    assert(gpio == 1);
    hal_ui_backlight_set(1, 0);
    assert(gpio == 0);
    hal_ui_backlight_set(1, 100);
    assert(gpio == 1);
    hal_ui_backlight_set(0, 100);
    assert(gpio == 0);
    assert(gpio_dir_calls == 1);
    puts("backlight is a dark-initialised GPIO enable");
    return 0;
}
