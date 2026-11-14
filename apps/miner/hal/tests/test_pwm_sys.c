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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pwm_sys.h"

#undef PWM_ROOT
#define PWM_ROOT TST_PWM_ROOT /* sysfs PWM root overridden to the test's TST_PWM_ROOT */
#include "../platform/linux/pwm_sys.c"

static const char *const root = PWM_ROOT;

static void md(const char *rel)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    int rc = mkdir(p, 0755);
    assert(rc == 0);
}
static void wr(const char *rel, const char *content)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    assert(f);
    if (content)
        fputs(content, f);
    fclose(f);
}
static int rd(const char *rel, char *buf, size_t n)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r");
    if (!f)
        return -1;
    size_t k = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[k] = '\0';
    return 0;
}

int main(void)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    int clean = system(cmd);
    assert(clean == 0);
    int rootrc = mkdir(root, 0755);
    assert(rootrc == 0);

    md("pwmchip1");
    {
        char lp[160];
        snprintf(lp, sizeof lp, "%s/pwmchip1/device", root);
        int rc = symlink("soc/70020000.pwm", lp);
        assert(rc == 0);
    }
    md("pwmchip3");
    {
        char lp[160];
        snprintf(lp, sizeof lp, "%s/pwmchip3/device", root);
        int rc = symlink("soc/9140a040.pwm", lp);
        assert(rc == 0);
    }
    md("pwmchip5");
    {
        char lp[160];
        snprintf(lp, sizeof lp, "%s/pwmchip5/device", root);
        int rc = symlink("soc/9140a000.pwm", lp);
        assert(rc == 0);
    }
    wr("pwmchip3/export", "");
    md("pwmchip3/pwm0");
    wr("pwmchip3/pwm0/period", "");
    wr("pwmchip3/pwm0/duty_cycle", "");
    wr("pwmchip3/pwm0/enable", "");

    char buf[32];
    int  v;

    assert(pwm_chip_by_addr("9140a040") == 3);
    assert(pwm_chip_by_addr("70020000") == 1);
    assert(pwm_chip_by_addr("9140a000") == 5);
    assert(pwm_chip_by_addr("deadbeef") < 0);

    {
        static const struct {
            unsigned pad;
            int      chip;
            int      ch;
        } pads[] = {
            { 0, 5, 0 },
            { 1, 5, 1 },
            { 2, 5, 2 },
            { 3, 3, 0 },
            { 4, 3, 1 },
            { 5, 3, 2 },
        };
        for (size_t i = 0; i < sizeof pads / sizeof pads[0]; i++) {
            int chip = -99, ch = -99;
            assert(pwm_pad_resolve(pads[i].pad, &chip, &ch) == 0);
            if (chip != pads[i].chip || ch != pads[i].ch)
                fprintf(stderr, "  FAIL: PWM%u -> chip %d ch%d, want chip %d ch%d\n", pads[i].pad,
                        chip, ch, pads[i].chip, pads[i].ch);
            assert(chip == pads[i].chip && ch == pads[i].ch);
        }
        int chip = -99, ch = -99;
        assert(pwm_pad_resolve(6, &chip, &ch) < 0);
        assert(pwm_pad_resolve(255, &chip, &ch) < 0);
        assert(chip == -99 && ch == -99);
    }

    assert(pwm_init(3, 0) == 0);

    assert(pwm_set_period(3, 0, 20000) == 0);
    assert(rd("pwmchip3/pwm0/period", buf, sizeof buf) == 0 && !strcmp(buf, "20000"));

    assert(pwm_set_duty(3, 0, 5000) == 0);
    assert(pwm_get_duty(3, 0, &v) == 0 && v == 5000);

    assert(pwm_set_status(3, 0, ENABLE) == 0);
    assert(rd("pwmchip3/pwm0/enable", buf, sizeof buf) == 0 && !strcmp(buf, "1"));

    char p[160];

    snprintf(p, sizeof p, "%s/pwmchip5/device", root);
    int urc = unlink(p);
    assert(urc == 0);
    {
        int chip = -99, ch = -99;
        assert(pwm_pad_resolve(0, &chip, &ch) < 0);
        assert(chip == -99 && ch == -99);
        assert(pwm_pad_resolve(3, &chip, &ch) == 0 && chip == 3 && ch == 0);
    }

    const char *files[] = { "pwmchip3/pwm0/period", "pwmchip3/pwm0/duty_cycle",
        "pwmchip3/pwm0/enable", "pwmchip3/export", "pwmchip3/device", "pwmchip1/device" };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, files[i]);
        unlink(p);
    }
    snprintf(p, sizeof p, "%s/pwmchip3/pwm0", root);
    rmdir(p);
    snprintf(p, sizeof p, "%s/pwmchip3", root);
    rmdir(p);
    snprintf(p, sizeof p, "%s/pwmchip1", root);
    rmdir(p);
    snprintf(p, sizeof p, "%s/pwmchip5", root);
    rmdir(p);
    rmdir(root);

    printf("pwm_sys OK\n");
    return 0;
}
