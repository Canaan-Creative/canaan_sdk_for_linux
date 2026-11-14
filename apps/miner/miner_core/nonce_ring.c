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

#include "nonce_ring.h"
#include <string.h>

void nonce_ring_init(nonce_ring_t *r, void *storage, uint16_t item_size, uint16_t capacity)
{
    r->slots     = storage;
    r->item_size = item_size;
    r->capacity  = capacity;
    r->read_pos  = 0;
    r->write_pos = 0;
    r->discarded = 0;
}

int nonce_ring_full(const nonce_ring_t *r)
{
    return (uint16_t)((r->write_pos + 1) % r->capacity) == r->read_pos;
}

int nonce_ring_empty(const nonce_ring_t *r)
{
    return r->write_pos == r->read_pos;
}

int nonce_ring_push(nonce_ring_t *r, const void *item)
{
    if (nonce_ring_full(r)) {
        r->discarded++;
        return -1;
    }
    memcpy((char *)r->slots + (size_t)r->write_pos * r->item_size, item, r->item_size);
    r->write_pos = (uint16_t)((r->write_pos + 1) % r->capacity);
    return 0;
}

void *nonce_ring_peek(nonce_ring_t *r)
{
    if (nonce_ring_empty(r))
        return NULL;
    return (char *)r->slots + (size_t)r->read_pos * r->item_size;
}

void nonce_ring_consume(nonce_ring_t *r)
{
    if (!nonce_ring_empty(r))
        r->read_pos = (uint16_t)((r->read_pos + 1) % r->capacity);
}
