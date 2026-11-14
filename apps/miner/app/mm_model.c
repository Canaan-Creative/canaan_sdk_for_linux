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

#include "mm_model.h"
#include "ubootenv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int env_char_ok(unsigned char c)
{
    return c >= 0x20 && c <= 0x7e;
}

static void env_report(char *dst, size_t cap, const char *value)
{
    snprintf(dst, cap, "%s", value ? value : "");
    for (size_t i = 0; dst[i]; i++)
        if (!env_char_ok((unsigned char)dst[i]))
            dst[i] = '?';
}

int mm_model_check(const char *hardware, const char *software, char *env_hw, char *env_sw,
        size_t cap)
{
    char *hw     = ubootenv_get("HW");
    char *sw     = ubootenv_get("SW");
    int   result = -1;

    env_report(env_hw, cap, hw);
    env_report(env_sw, cap, sw);
    if (hw && hw[0] && sw && sw[0])
        result = strcmp(hw, hardware) != 0 || strcmp(sw, software) != 0;

    free(hw);
    free(sw);
    return result;
}
