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

#include "mc_api_poollist.h"
#include <stdlib.h>
#include <string.h>

static const char *payload(const char *req, int is_json, int *bare)
{
    *bare = 0;
    if (!is_json) {
        const char *p = strchr(req, '|');
        return p ? p + 1 : NULL;
    }
    const char *j = strstr(req, "\"parameter\"");
    if (!j)
        return NULL;
    const char *p = strchr(j + 11, ':');
    if (!p)
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '"')
        return p + 1;
    if (*p != '-' && (*p < '0' || *p > '9'))
        return NULL;
    *bare = 1;
    return p;
}

mc_poollist_rc mc_api_poollist_parse(const char *req, int is_json, mc_poollist_req *out)
{
    if (!req || !out)
        return MC_POOLLIST_MISSING;
    memset(out, 0, sizeof *out);

    int         bare;
    const char *p = payload(req, is_json, &bare);
    if (!p || !*p || *p == '"')
        return MC_POOLLIST_MISSING;

    if (bare) {
        out->ids[out->n++] = atoi(p);
        return MC_POOLLIST_OK;
    }

    while (out->n < MC_POOLLIST_MAX) {
        out->ids[out->n++] = atoi(p);
        p += strcspn(p, ",\"");
        if (*p != ',' || !p[1] || p[1] == '"')
            break;
        p++;
    }
    return MC_POOLLIST_OK;
}
