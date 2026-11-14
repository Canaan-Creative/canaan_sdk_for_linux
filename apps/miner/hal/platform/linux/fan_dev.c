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

#include "fan_dev.h"
#include "board/board_tables.h"
#include "pwm_sys.h"
#include "timer.h"
#include "boardconf.h"
#include "osif.h"
#include <pthread.h>

#define FAN_PERIOD K230_PWM_PERIOD_NS /* fan PWM period, ns */

#define FAN_TACH_WINDOW_S 1 /* tach counter sampling window, s */

static int s_chip[FAN_COUNT];       /* pwmchip index driving each fan, -1 when unavailable */
static int s_ch[FAN_COUNT];         /* PWM channel within s_chip of each fan */
static int s_ready;                 /* at least one fan PWM channel brought up */
static int s_last_duty_x10 = -1;    /* last requested duty, 0.1 %, -1 before any request */
static int s_read_chip1[FAN_COUNT]; /* pwmchip index + 1 resolved for reading, 0 when unresolved */
static int s_tach_open;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER; /* guards the fan state above */

static int duty_to_ns(int duty_x10)
{
    if (FAN_PWM_ACTIVE_LOW)
        duty_x10 = 1000 - duty_x10;
    return duty_x10 * FAN_PERIOD / 1000;
}

int fan_dev_init(void)
{
    int ok = 0;

    pthread_mutex_lock(&s_lock);
    s_ready = 0;
    for (size_t i = 0; i < board_fan_count; i++) {
        int chip = -1, ch = -1;
        if (pwm_pad_resolve(board_fans[i].pwm, &chip, &ch) != 0 || pwm_init(chip, ch) != 0) {
            s_chip[i] = -1;
            osif_log(OSIF_LOG_ERR, "Fan %d: speed control PWM%u (chip %d channel %d) unavailable",
                    (int)i, (unsigned)board_fans[i].pwm, chip, ch);
            continue;
        }
        s_chip[i] = chip;
        s_ch[i]   = ch;
        pwm_set_period(chip, ch, FAN_PERIOD);
        pwm_set_duty(chip, ch, duty_to_ns(FAN_DUTY_DEFAULT_PCT * 10));
        pwm_set_status(chip, ch, ENABLE);
        ok++;
    }
    s_ready = ok > 0;
    if (s_ready)
        s_last_duty_x10 = FAN_DUTY_DEFAULT_PCT * 10;
    pthread_mutex_unlock(&s_lock);
    osif_log(ok == (int)board_fan_count ? OSIF_LOG_INFO : OSIF_LOG_ERR,
            "Fans: %d/%d pwm channel(s) start", ok, (int)board_fan_count);
    return s_ready ? 0 : -1;
}

static int fan_pwm_get_duty_pct(int fan)
{
    pthread_mutex_lock(&s_lock);
    int chip = (s_ready && s_chip[fan] >= 0) ? s_chip[fan] : s_read_chip1[fan] - 1;
    if (chip < 0) {
        int ch;
        if (pwm_pad_resolve(board_fans[fan].pwm, &chip, &ch) != 0) {
            pthread_mutex_unlock(&s_lock);
            return -1;
        }
        s_ch[fan]         = ch;
        s_read_chip1[fan] = chip + 1;
    }
    int duty_ns = 0;
    if (pwm_get_duty(chip, s_ch[fan], &duty_ns) != 0) {
        s_read_chip1[fan] = 0;
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    pthread_mutex_unlock(&s_lock);
    int pct = duty_ns * 100 / FAN_PERIOD;
    return FAN_PWM_ACTIVE_LOW ? 100 - pct : pct;
}

int fan_dev_get_rpm(int fan)
{
    if (fan < 0 || (size_t)fan >= board_fan_count || board_fans[fan].tach_node == NULL ||
            board_fans[fan].pulses_per_rev == 0)
        return -1;

    pthread_mutex_lock(&s_lock);
    if (!s_tach_open) {
        if (timer_init(FAN_TACH_WINDOW_S) != 0) {
            pthread_mutex_unlock(&s_lock);
            return -1;
        }
        s_tach_open = 1;
    }
    unsigned int pulses_per_s = 0;
    int          rc           = get_timer_count(&pulses_per_s, fan);
    pthread_mutex_unlock(&s_lock);
    if (rc != (int)sizeof pulses_per_s)
        return -1;
    return (int)(pulses_per_s * 60u / board_fans[fan].pulses_per_rev);
}

void fan_dev_set_duty_x10(int duty_x10)
{
    int d = duty_x10;

    if (d < FAN_DUTY_MIN_PCT * 10)
        d = FAN_DUTY_MIN_PCT * 10;
    if (d > FAN_DUTY_MAX_PCT * 10)
        d = FAN_DUTY_MAX_PCT * 10;
    int ns = duty_to_ns(d);
    pthread_mutex_lock(&s_lock);
    for (size_t i = 0; s_ready && i < board_fan_count; i++)
        if (s_chip[i] >= 0)
            pwm_set_duty(s_chip[i], s_ch[i], ns);
    s_last_duty_x10 = duty_x10;
    pthread_mutex_unlock(&s_lock);
}

void fan_dev_stop(void)
{
    int n  = 0;
    int ns = duty_to_ns(0);

    pthread_mutex_lock(&s_lock);
    for (size_t i = 0; s_ready && i < board_fan_count; i++)
        if (s_chip[i] >= 0 && pwm_set_duty(s_chip[i], s_ch[i], ns) == 0)
            n++;
    s_last_duty_x10 = 0;
    pthread_mutex_unlock(&s_lock);
    osif_log(OSIF_LOG_INFO, "Fans: %d/%d pwm channel(s) stop", n, (int)board_fan_count);
}

int fan_dev_last_duty_x10(void)
{
    pthread_mutex_lock(&s_lock);
    int d = s_last_duty_x10;
    pthread_mutex_unlock(&s_lock);
    return d;
}

int fan_dev_get_duty_pct(void)
{
    for (size_t i = 0; i < board_fan_count; i++) {
        int pct = fan_pwm_get_duty_pct((int)i);
        if (pct >= 0)
            return pct;
    }
    return -1;
}
