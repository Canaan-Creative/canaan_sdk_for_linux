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
#include <sys/stat.h>
#include <unistd.h>
#include "adc_iio.h"

#undef IIO_DEVICE_DEFAULT
/* IIO device directory overridden to the test's TST_IIO_DEVICE */
#define IIO_DEVICE_DEFAULT TST_IIO_DEVICE
#include "../platform/linux/adc_iio.c"

static void write_file(const char *dir, const char *name, const char *content)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

int main(void)
{
    const char *dir = IIO_DEVICE_DEFAULT;
    char        cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    int clean = system(cmd);
    assert(clean == 0);
    int mkrc = mkdir(dir, 0755);
    assert(mkrc == 0);

    write_file(dir, "in_voltage3_raw", "1234\n");
    write_file(dir, "in_voltage0_raw", "0\n");

    int v = -1;
    assert(adc_get_raw(3, &v) == 0 && v == 1234);
    assert(adc_get_raw(0, &v) == 0 && v == 0);

    v = 777;
    assert(adc_get_raw(9, &v) < 0 && v == 777);
    assert(adc_get_raw(-1, &v) < 0);
    assert(adc_get_raw(0, NULL) < 0);

    char p[256];
    snprintf(p, sizeof p, "%s/in_voltage3_raw", dir);
    unlink(p);
    snprintf(p, sizeof p, "%s/in_voltage0_raw", dir);
    unlink(p);
    rmdir(dir);

    printf("adc_iio OK\n");
    return 0;
}
