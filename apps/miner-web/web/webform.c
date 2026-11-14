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

#define _DEFAULT_SOURCE

#include "webform.h"
#include "hex.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

size_t web_url_decode(char *dst, size_t cap, const char *src, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char)src[i];
        if (c == '+')
            c = ' ';
        else if (c == '%') {
            if (i + 2 >= len)
                goto invalid;
            int h = hex_nibble((unsigned char)src[i + 1]);
            int l = hex_nibble((unsigned char)src[i + 2]);
            if (h < 0 || l < 0)
                goto invalid;
            c = (h << 4) | l;
            i += 2;
        }
        if (c < 0x20 || c == 0x7f || o + 1 >= cap)
            goto invalid;
        dst[o++] = (char)c;
    }
    if (cap)
        dst[o] = 0;
    return o;
invalid:
    if (cap)
        dst[0] = 0;
    return SIZE_MAX;
}

int web_form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (cap)
        out[0] = '\0';
    if (!body || !key)
        return 0;
    size_t      klen  = strlen(key);
    int         found = 0;
    const char *p     = body;
    while (*p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            const char *v    = p + klen + 1;
            const char *e    = strchr(v, '&');
            size_t      vlen = e ? (size_t)(e - v) : strlen(v);
            if (found || web_url_decode(out, cap, v, vlen) == SIZE_MAX) {
                if (cap)
                    out[0] = 0;
                return -1;
            }
            found = 1;
        }
        const char *amp = strchr(p, '&');
        if (!amp)
            break;
        p = amp + 1;
    }
    return found;
}

int web_pools_from_form(const char *body, pool_set *ps)
{
    pool_set fresh;
    char     pass[POOL_PASS_MAX];
    int      rc = -1;
    pool_init(&fresh, 0, 0);
    for (int i = 1; i <= POOL_VALID_NUM; i++) {
        char kp[16], kw[16], kk[16];
        char url[POOL_URL_MAX], user[POOL_USER_MAX];
        snprintf(kp, sizeof kp, "pool%d", i);
        snprintf(kw, sizeof kw, "worker%d", i);
        snprintf(kk, sizeof kk, "passwd%d", i);
        if (web_form_get(body, kp, url, sizeof url) < 0 ||
                web_form_get(body, kw, user, sizeof user) < 0 ||
                web_form_get(body, kk, pass, sizeof pass) < 0)
            goto out;
        if (strchr(user, '"') || strchr(user, '\\') || strchr(pass, '"') || strchr(pass, '\\'))
            goto out;
        /* row i is slot i-1; an empty URL or worker leaves the slot free */
        if (!url[0] || !user[0])
            continue;
        int idx = pool_add(&fresh, url, user, pass);
        if (idx < 0)
            goto out;
        fresh.pools[idx].slot = i - 1;
    }
    *ps = fresh;
    rc  = ps->count;
out:
    explicit_bzero(pass, sizeof pass);
    explicit_bzero(&fresh, sizeof fresh);
    return rc;
}
