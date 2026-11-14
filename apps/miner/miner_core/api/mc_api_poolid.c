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

#include "mc_api_poolid.h"
#include <stdlib.h>
#include <string.h>

mc_poolid_rc mc_api_poolid_parse(const char *req, int is_json, int *out_id)
{
    if (!req || !out_id)
        return MC_POOLID_MISSING;

    const char *p;
    if (is_json) {
        const char *j = strstr(req, "\"parameter\"");
        if (!j)
            return MC_POOLID_MISSING;
        p = strchr(j + 11, ':');
        if (!p)
            return MC_POOLID_MISSING;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == '"') {
            p++;
            if (*p == '"')
                return MC_POOLID_MISSING;
        } else if (*p != '-' && (*p < '0' || *p > '9')) {
            return MC_POOLID_MISSING;
        }
    } else {
        p = strchr(req, '|');
        if (!p)
            return MC_POOLID_MISSING;
        p++;
        if (*p == '\0')
            return MC_POOLID_MISSING;
    }

    *out_id = atoi(p);
    return MC_POOLID_OK;
}
