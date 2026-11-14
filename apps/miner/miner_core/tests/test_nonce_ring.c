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
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

int main(void)
{
    uint32_t     storage[4];
    nonce_ring_t r;
    nonce_ring_init(&r, storage, sizeof(uint32_t), 4);

    CHECK(nonce_ring_empty(&r) && !nonce_ring_full(&r), "starts empty");
    CHECK(nonce_ring_peek(&r) == NULL, "peek empty -> NULL");

    uint32_t a = 0xAA, b = 0xBB, c = 0xCC, d = 0xDD;
    CHECK(nonce_ring_push(&r, &a) == 0, "push 1");
    CHECK(nonce_ring_push(&r, &b) == 0, "push 2");
    CHECK(nonce_ring_push(&r, &c) == 0, "push 3 (now full: capacity-1 usable)");
    CHECK(nonce_ring_full(&r), "ring reports full at capacity-1");

    CHECK(nonce_ring_push(&r, &d) == -1, "push 4 -> -1 (rejected)");
    CHECK(r.discarded == 1, "discarded counter incremented");
    CHECK(nonce_ring_push(&r, &d) == -1 && r.discarded == 2, "second overflow counted");

    CHECK(*(uint32_t *)nonce_ring_peek(&r) == 0xAA, "peek -> oldest (AA)");
    CHECK(*(uint32_t *)nonce_ring_peek(&r) == 0xAA, "peek stable (still AA)");
    nonce_ring_consume(&r);
    CHECK(*(uint32_t *)nonce_ring_peek(&r) == 0xBB, "after consume -> BB");

    CHECK(nonce_ring_push(&r, &d) == 0, "push after consume (wrap) ok");
    nonce_ring_consume(&r);
    nonce_ring_consume(&r);
    CHECK(*(uint32_t *)nonce_ring_peek(&r) == 0xDD, "FIFO to the wrapped DD");
    nonce_ring_consume(&r);
    CHECK(nonce_ring_empty(&r), "drained -> empty");
    CHECK(r.discarded == 2, "no nonce lost silently (discarded stayed 2)");

    if (fails) {
        fprintf(stderr, "test_nonce_ring: %d FAIL\n", fails);
        return 1;
    }
    printf("test_nonce_ring: all pass\n");
    return 0;
}
