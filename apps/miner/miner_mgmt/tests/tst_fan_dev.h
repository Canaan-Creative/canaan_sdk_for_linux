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

#ifndef TST_FAN_DEV_H
#define TST_FAN_DEV_H

#include "fan_dev.h"

/*
 * fan_dev stubs for the supervisor unity tests: the last duty lands in
 * tst_duty_x10. With TST_FAN_DUTY_EV defined, each set is also logged as a
 * "duty" event, so include after tst_events.h.
 */
static int tst_duty_x10; /* Last duty passed to fan_dev_set_duty_x10(), 0.1 % units */

int fan_dev_init(void)
{
    return 0;
}
void fan_dev_set_duty_x10(int duty_x10)
{
    tst_duty_x10 = duty_x10;
#ifdef TST_FAN_DUTY_EV
    ev("duty", duty_x10);
#endif /* TST_FAN_DUTY_EV */
}
int fan_dev_last_duty_x10(void)
{
    return tst_duty_x10;
}
int fan_dev_get_duty_pct(void)
{
    return tst_duty_x10 / 10;
}

#endif /* TST_FAN_DEV_H */
