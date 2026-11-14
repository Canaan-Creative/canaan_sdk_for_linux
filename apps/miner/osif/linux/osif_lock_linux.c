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

#include "osif.h"
#include <pthread.h>
#include <stdlib.h>

struct osif_lock {
    pthread_mutex_t m; /* non-recursive mutex */
};

osif_status_t osif_lock_create(osif_lock_t **l)
{
    osif_lock_t *o = calloc(1, sizeof *o);
    if (!o)
        return OSIF_ERR;
    if (pthread_mutex_init(&o->m, NULL) != 0) {
        free(o);
        return OSIF_ERR;
    }
    *l = o;
    return OSIF_OK;
}

void osif_lock_acquire(osif_lock_t *l)
{
    pthread_mutex_lock(&l->m);
}
void osif_lock_release(osif_lock_t *l)
{
    pthread_mutex_unlock(&l->m);
}
