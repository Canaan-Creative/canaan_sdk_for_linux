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

#include "share.h"
#include <stdio.h>
#include <string.h>

void mc_dedup_init(mc_dedup *d)
{
    d->count = 0;
    d->head  = 0;
}

int mc_dedup_check(mc_dedup *d, const char *job_id, const char *xnonce2, const char *ntime,
        const char *nonce, const char *version_bits)
{
    char key[MC_DEDUP_KEY];
    snprintf(key, sizeof key, "%s|%s|%s|%s|%s", job_id ? job_id : "", xnonce2 ? xnonce2 : "",
            ntime ? ntime : "", nonce ? nonce : "", version_bits ? version_bits : "");

    for (int i = 0; i < d->count; i++)
        if (strcmp(d->keys[i], key) == 0)
            return 1;

    int slot = d->head;
    if (d->count < MC_DEDUP_MAX)
        slot = d->count++;
    else
        d->head = (d->head + 1) % MC_DEDUP_MAX;
    memcpy(d->keys[slot], key, strlen(key) + 1);
    return 0;
}

void mc_tally_init(mc_tally *t)
{
    t->accepted = 0;
    t->rejected = 0;
}

void mc_tally_record(mc_tally *t, int accepted)
{
    if (accepted)
        t->accepted++;
    else
        t->rejected++;
}
