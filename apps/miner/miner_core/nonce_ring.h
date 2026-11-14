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

#ifndef NONCE_RING_H
#define NONCE_RING_H

#include <stdint.h>

/* Ring buffer of fixed-size items. */
typedef struct nonce_ring {
    void    *slots;
    uint16_t item_size, capacity; /* bytes per item; slot count */
    uint16_t read_pos, write_pos; /* next slot to read; next slot to write */
    uint32_t discarded;           /* items dropped on a full ring, count */
} nonce_ring_t;

void  nonce_ring_init(nonce_ring_t *r, void *storage, uint16_t item_size, uint16_t capacity);
int   nonce_ring_full(const nonce_ring_t *r);
int   nonce_ring_empty(const nonce_ring_t *r);
int   nonce_ring_push(nonce_ring_t *r, const void *item);
void *nonce_ring_peek(nonce_ring_t *r);
void  nonce_ring_consume(nonce_ring_t *r);

#endif /* NONCE_RING_H */
