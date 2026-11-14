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

#include "pool.h"
#include <string.h>

void pool_init(pool_set *ps, int backoff_base_ms, int backoff_max_ms)
{
    memset(ps, 0, sizeof *ps);
    ps->active          = -1;
    ps->backoff_base_ms = backoff_base_ms > 0 ? backoff_base_ms : 1000;
    ps->backoff_max_ms  = backoff_max_ms > 0 ? backoff_max_ms : 60000;
}

static int copy_field(char *dst, size_t cap, const char *src)
{
    if (!src || !*src)
        return -1;
    if (strlen(src) >= cap)
        return -1;
    strcpy(dst, src);
    return 0;
}

int pool_add(pool_set *ps, const char *url, const char *user, const char *pass)
{
    if (ps->count >= POOL_VALID_NUM)
        return -1;
    pool_t *p = &ps->pools[ps->count];
    if (copy_field(p->url, sizeof p->url, url) != 0)
        return -1;
    if (copy_field(p->user, sizeof p->user, user) != 0)
        return -1;
    if (!pass || strlen(pass) >= sizeof p->pass)
        return -1;
    strcpy(p->pass, pass);
    p->alive   = 1;
    p->enabled = 1;
    if (ps->count && ps->pools[ps->count - 1].slot + 1 >= POOL_VALID_NUM)
        for (int i = 0; i < ps->count; i++)
            ps->pools[i].slot = i;
    p->slot = ps->count ? ps->pools[ps->count - 1].slot + 1 : 0;
    return ps->count++;
}

void pool_mark_dead(pool_set *ps, int idx)
{
    if (idx >= 0 && idx < ps->count)
        ps->pools[idx].alive = 0;
}

void pool_mark_alive(pool_set *ps, int idx)
{
    if (idx >= 0 && idx < ps->count)
        ps->pools[idx].alive = 1;
}

int pool_backoff_next(pool_set *ps)
{
    if (ps->backoff_ms == 0)
        ps->backoff_ms = ps->backoff_base_ms;
    else if (ps->backoff_ms < ps->backoff_max_ms) {
        long doubled   = (long)ps->backoff_ms * 2;
        ps->backoff_ms = doubled > ps->backoff_max_ms ? ps->backoff_max_ms : (int)doubled;
    }
    return ps->backoff_ms;
}

void pool_backoff_reset(pool_set *ps)
{
    ps->backoff_ms = 0;
}
