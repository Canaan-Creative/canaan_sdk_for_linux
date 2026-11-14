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

#ifndef PWM_SYS_H
#define PWM_SYS_H

#define PWM_ROOT           "/sys/class/pwm"
#define K230_PWM_PERIOD_NS 40000
#define ENABLE             1 /* PWM channel enable value */

int pwm_chip_by_addr(const char *addr);
int pwm_pad_resolve(unsigned pad, int *chip, int *ch);
int pwm_init(int chip, int channel);
int pwm_set_period(int chip, int channel, int period);
int pwm_set_duty(int chip, int channel, int duty);
int pwm_get_duty(int chip, int channel, int *duty);
int pwm_set_status(int chip, int channel, int enable);

#endif /* PWM_SYS_H */
