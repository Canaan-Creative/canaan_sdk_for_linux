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
#include "gpio_dev.h"

#undef GPIO_ROOT
#define GPIO_ROOT TST_GPIO_ROOT /* sysfs GPIO root overridden to the test's TST_GPIO_ROOT */
#include "../platform/linux/gpio_dev.c"

static const char *const root = GPIO_ROOT;

static void touch(const char *rel, const char *content)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    assert(f);
    if (content)
        fputs(content, f);
    fclose(f);
}

static int read_str(const char *rel, char *buf, size_t n)
{
    char p[256];
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

    touch("export", "");
    char gdir[256];
    snprintf(gdir, sizeof gdir, "%s/gpio17", root);
    int mkrc = mkdir(gdir, 0755);
    assert(mkrc == 0);
    touch("gpio17/direction", "");
    touch("gpio17/value", "");

    char buf[32];

    gpio_set_direction(17, GPIO_OUT_HIGH);
    assert(read_str("gpio17/direction", buf, sizeof buf) == 0 && !strcmp(buf, "high"));
    assert(read_str("export", buf, sizeof buf) == 0 && !strcmp(buf, "17"));

    gpio_set_value(17, 1);
    assert(read_str("gpio17/value", buf, sizeof buf) == 0 && !strcmp(buf, "1"));

    gpio_set_value(17, 0);
    assert(read_str("gpio17/value", buf, sizeof buf) == 0 && !strcmp(buf, "0"));

    gpio_set_direction(17, GPIO_IN);
    assert(read_str("gpio17/direction", buf, sizeof buf) == 0 && !strcmp(buf, "in"));

    assert(gpio_set_direction_checked(99, GPIO_OUT_LOW) != 0);
    assert(gpio_set_value_checked(99, GPIO_LOW) != 0);
    assert(gpio_set_direction_checked(17, -1) != 0);
    assert(gpio_set_direction_checked(17, GPIO_OUT_LOW) == 0);
    assert(gpio_set_value_checked(17, GPIO_LOW) == 0);
    assert(read_str("gpio17/value", buf, sizeof buf) == 0 && !strcmp(buf, "0"));

    char p[256];
    snprintf(p, sizeof p, "%s/gpio17/direction", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/gpio17/value", root);
    unlink(p);
    rmdir(gdir);
    snprintf(p, sizeof p, "%s/export", root);
    unlink(p);
    rmdir(root);

    printf("gpio_dev OK\n");
    return 0;
}
