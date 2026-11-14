/* SPDX-License-Identifier: MIT */
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

#ifndef MINER_CHAN_H
#define MINER_CHAN_H

#include "osif.h"

typedef struct {
    osif_ipc_t     **h; /* where the channel handle is stored */
    const char      *path;
    int              is_server; /* 1 listens, 0 connects */
    osif_ipc_recv_cb cb;        /* handler for each received message, NULL for none */
    const char      *name;      /* channel name in logs, may be NULL */
    int              required;  /* 1: an open failure is fatal, 0: continue without it */
} miner_chan_t;

static inline int  miner_chan_open(const miner_chan_t *tab, int n);
static inline void miner_chan_close(const miner_chan_t *tab, int n);

static inline int miner_chan_open(const miner_chan_t *tab, int n)
{
    if (!tab || n <= 0)
        return 0;

    for (int i = 0; i < n; i++) {
        const miner_chan_t *c = &tab[i];
        if (!c->h || !c->path)
            continue;
        const char *name = c->name ? c->name : "ipc";
        *c->h            = NULL;
        if (osif_ipc_init(c->h, c->cb, c->path, c->is_server) == OSIF_OK)
            continue;
        *c->h = NULL;
        if (c->required) {
            osif_log(OSIF_LOG_ERR, "%s channel on %s FAILED to open", name, c->path);
            miner_chan_close(tab, i);
            return -1;
        }
        osif_log(OSIF_LOG_WARN,
                "%s channel on %s unavailable — continuing "
                "without it",
                name, c->path);
    }
    return 0;
}

static inline void miner_chan_close(const miner_chan_t *tab, int n)
{
    if (!tab)
        return;
    for (int i = n - 1; i >= 0; i--)
        if (tab[i].h) {
            osif_ipc_deinit(*tab[i].h);
            *tab[i].h = NULL;
        }
}

#endif /* MINER_CHAN_H */
