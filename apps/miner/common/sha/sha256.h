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

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32 /* digest length, bytes */
#define SHA256_BLOCK_SIZE  64 /* compression block length, bytes */

typedef struct {
    uint32_t state[8];               /* hash state words H0..H7 */
    uint64_t bitlen;                 /* total message length, bits */
    uint8_t  buf[SHA256_BLOCK_SIZE]; /* pending partial block */
    size_t   buflen;                 /* bytes held in buf */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]);
void sha256(const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);
void sha256d(const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#endif /* SHA256_H */
