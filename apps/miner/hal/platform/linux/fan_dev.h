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

#ifndef FAN_DEV_H
#define FAN_DEV_H

#include "boardconf.h"

int  fan_dev_init(void);
void fan_dev_set_duty_x10(int duty_x10);
void fan_dev_stop(void);
int  fan_dev_last_duty_x10(void);
int  fan_dev_get_duty_pct(void);
int  fan_dev_get_rpm(int fan);

#endif /* FAN_DEV_H */
