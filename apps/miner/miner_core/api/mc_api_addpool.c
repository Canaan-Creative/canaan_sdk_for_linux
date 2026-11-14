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

#include "mc_api_addpool.h"
#include <string.h>

static const char *payload(const char *req)
{
    const char *p = strchr(req, '|');
    if (p)
        return p + 1;
    const char *j = strstr(req, "\"parameter\"");
    if (!j)
        return NULL;
    const char *c = strchr(j, ':');
    if (!c)
        return NULL;
    const char *q = strchr(c, '"');
    if (!q)
        return NULL;
    return q + 1;
}

static int copy_field(char *dst, size_t cap, const char *beg, const char *end)
{
    size_t n = (size_t)(end - beg);
    if (n >= cap)
        return -1;
    memcpy(dst, beg, n);
    dst[n] = '\0';
    return 0;
}

mc_addpool_rc mc_api_addpool_parse(const char *req, mc_addpool_req *out)
{
    if (!req || !out)
        return MC_ADDPOOL_MISSING;
    memset(out, 0, sizeof *out);

    const char *p = payload(req);
    if (!p)
        return MC_ADDPOOL_MISSING;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return MC_ADDPOOL_MISSING;

    const char *end = p;
    while (*end && *end != '"' && (unsigned char)*end >= 0x20 && (unsigned char)*end != 0x7f)
        end++;
    const char *c1 = memchr(p, ',', (size_t)(end - p));
    if (!c1)
        return MC_ADDPOOL_INVALID;
    const char *c2 = memchr(c1 + 1, ',', (size_t)(end - (c1 + 1)));
    if (!c2)
        return MC_ADDPOOL_INVALID;

    if (c1 == p)
        return MC_ADDPOOL_INVALID;
    if (copy_field(out->url, sizeof out->url, p, c1))
        return MC_ADDPOOL_INVALID;
    if (copy_field(out->user, sizeof out->user, c1 + 1, c2))
        return MC_ADDPOOL_INVALID;
    if (copy_field(out->pass, sizeof out->pass, c2 + 1, end))
        return MC_ADDPOOL_INVALID;
    return MC_ADDPOOL_OK;
}
