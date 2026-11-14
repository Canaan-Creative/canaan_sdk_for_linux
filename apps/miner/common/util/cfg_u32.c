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

#include "cfg_u32.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

int cfg_u32_parse(const char *s, uint32_t max, uint32_t *out)
{
    char *end;

    errno           = 0;
    unsigned long u = strtoul(s, &end, 10);
    if (!isdigit((unsigned char)*s) || *end || errno == ERANGE)
        return -1;
    *out = u > max ? max : (uint32_t)u;
    return 0;
}
