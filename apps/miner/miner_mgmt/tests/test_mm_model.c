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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ubootenv.h"

static void test_free(void *pointer);
#define free test_free /* count frees in mm_model.c */
#include "mm_model.c"
#undef free

static const char *env_hw; /* U-Boot env HW value; NULL unset */
static const char *env_sw; /* U-Boot env SW value; NULL unset */
/* ubootenv_get() calls for HW and SW; returned copies not yet freed */
static int  asked_hw, asked_sw, outstanding;
static char seen_hw[32], seen_sw[32]; /* HW and SW values mm_model_check() reports */

char *ubootenv_get(const char *name)
{
    const char *value;
    char       *copy;
    if (!strcmp(name, "HW")) {
        asked_hw++;
        value = env_hw;
    } else if (!strcmp(name, "SW")) {
        asked_sw++;
        value = env_sw;
    } else {
        assert(0);
        return NULL;
    }
    if (!value)
        return NULL;
    copy = malloc(strlen(value) + 1);
    assert(copy);
    memcpy(copy, value, strlen(value) + 1);
    outstanding++;
    return copy;
}

static void test_free(void *pointer)
{
    if (pointer)
        outstanding--;
    free(pointer);
}
static int check(const char *hardware, const char *software)
{
    int result;
    asked_hw = asked_sw = 0;
    memset(seen_hw, 'x', sizeof seen_hw);
    memset(seen_sw, 'x', sizeof seen_sw);
    result = mm_model_check(hardware, software, seen_hw, seen_sw, sizeof seen_hw);
    assert(asked_hw == 1 && asked_sw == 1);
    assert(outstanding == 0);
    return result;
}

int main(void)
{
    env_hw = "N_MM1v1_X1";
    env_sw = "MM319";
    assert(check("N_MM1v1_X1", "MM319") == 0);
    assert(!strcmp(seen_hw, "N_MM1v1_X1") && !strcmp(seen_sw, "MM319"));
    assert(check("NO_SUCH_HW", "MM319") == 1);
    assert(check("N_MM1v1_X1", "other") == 1);
    assert(check("NO_SUCH_HW", "other") == 1);

    env_hw = "N_MM1v1_X1 ";
    env_sw = "MM319 ";
    assert(check("N_MM1v1_X1", "MM319") == 1);
    assert(!strcmp(seen_hw, "N_MM1v1_X1 ") && !strcmp(seen_sw, "MM319 "));

    env_hw = "N_MM1v1_X1\n forged";
    env_sw = "MM3\t19";
    assert(check("N_MM1v1_X1", "MM319") == 1);
    assert(!strcmp(seen_hw, "N_MM1v1_X1? forged") && !strcmp(seen_sw, "MM3?19"));

    env_sw = NULL;
    assert(check("N_MM1v1_X1", "MM319") == -1);
    assert(!seen_sw[0]);

    env_hw = NULL;
    env_sw = "MM319";
    assert(check("N_MM1v1_X1", "MM319") == -1);
    assert(!seen_hw[0] && !strcmp(seen_sw, "MM319"));

    env_hw = "";
    assert(check("N_MM1v1_X1", "MM319") == -1);
    assert(!seen_hw[0]);

    env_hw = "N_MM1v1_X1";
    env_sw = "";
    assert(check("N_MM1v1_X1", "MM319") == -1);
    assert(!strcmp(seen_hw, "N_MM1v1_X1") && !seen_sw[0]);

    env_hw = NULL;
    env_sw = NULL;
    assert(check("N_MM1v1_X1", "MM319") == -1);
    assert(!seen_hw[0] && !seen_sw[0]);

    puts("model read-only checks passed");
    return 0;
}
