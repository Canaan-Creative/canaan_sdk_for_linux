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

#include "pwm_sys.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PWM_PADS_PER_CHIP 3 /* PWM pads per K230 PWM controller */

/* K230 PWM controller base addresses as hex text, indexed by pad / PWM_PADS_PER_CHIP */
static const char *const k230_pwm_addrs[] = { "9140a000", "9140a040" };

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

static int read_int(const char *path, int *out)
{
    FILE *f = fopen(path, "re");
    if (!f)
        return -1;
    int v;
    int got = fscanf(f, "%d", &v);
    fclose(f);
    if (got != 1)
        return -1;
    *out = v;
    return 0;
}

static void chan_path(char *b, size_t n, int chip, int ch, const char *attr)
{
    snprintf(b, n, "%s/pwmchip%d/pwm%d/%s", PWM_ROOT, chip, ch, attr);
}

int pwm_chip_by_addr(const char *addr)
{
    if (!addr || !*addr)
        return -1;
    char needle[64];
    snprintf(needle, sizeof needle, "%s.", addr);

    DIR *d = opendir(PWM_ROOT);
    if (!d)
        return -1;
    int            found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int chip;
        if (sscanf(e->d_name, "pwmchip%d", &chip) != 1)
            continue;
        char link[512], tgt[256];
        snprintf(link, sizeof link, "%s/%s/device", PWM_ROOT, e->d_name);
        ssize_t k = readlink(link, tgt, sizeof tgt - 1);
        if (k < 0)
            continue;
        tgt[k] = '\0';
        if (strstr(tgt, needle)) {
            found = chip;
            break;
        }
    }
    closedir(d);
    return found;
}

int pwm_pad_resolve(unsigned pad, int *chip, int *ch)
{
    if (pad >= PWM_PADS_PER_CHIP * (sizeof k230_pwm_addrs / sizeof k230_pwm_addrs[0]))
        return -1;
    int c = pwm_chip_by_addr(k230_pwm_addrs[pad / PWM_PADS_PER_CHIP]);
    if (c < 0)
        return -1;
    *chip = c;
    *ch   = (int)(pad % PWM_PADS_PER_CHIP);
    return 0;
}

int pwm_init(int chip, int channel)
{
    char path[256], num[16];
    snprintf(path, sizeof path, "%s/pwmchip%d/export", PWM_ROOT, chip);
    snprintf(num, sizeof num, "%d", channel);
    (void)write_str(path, num);
    chan_path(path, sizeof path, chip, channel, "period");
    return access(path, F_OK) == 0 ? 0 : -1;
}

int pwm_set_period(int chip, int channel, int period)
{
    char path[256], v[24];
    chan_path(path, sizeof path, chip, channel, "period");
    snprintf(v, sizeof v, "%d", period);
    return write_str(path, v);
}

int pwm_set_duty(int chip, int channel, int duty)
{
    char path[256], v[24];
    chan_path(path, sizeof path, chip, channel, "duty_cycle");
    snprintf(v, sizeof v, "%d", duty);
    return write_str(path, v);
}

int pwm_get_duty(int chip, int channel, int *duty)
{
    char path[256];
    chan_path(path, sizeof path, chip, channel, "duty_cycle");
    return read_int(path, duty);
}

int pwm_set_status(int chip, int channel, int enable)
{
    char path[256];
    chan_path(path, sizeof path, chip, channel, "enable");
    return write_str(path, enable ? "1" : "0");
}
