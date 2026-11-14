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

#include "sha256.h"
#include <string.h>

#define ROTR(x, n)   (((x) >> (n)) | ((x) << (32 - (n))))      /* 32-bit rotate right */
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))              /* choose */
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z))) /* majority */
#define BSIG0(x)     (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))  /* round function Sigma0 */
#define BSIG1(x)     (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))  /* round function Sigma1 */
#define SSIG0(x)     (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))   /* message schedule sigma0 */
#define SSIG1(x)     (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10)) /* message schedule sigma1 */

/* clang-format off */
static const uint32_t K[64] = { /* SHA-256 round constants */
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
/* clang-format on */

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int      t;

    for (t = 0; t < 16; t++)
        w[t] = (uint32_t)block[t * 4] << 24 | (uint32_t)block[t * 4 + 1] << 16 |
                (uint32_t)block[t * 4 + 2] << 8 | (uint32_t)block[t * 4 + 3];
    for (t = 16; t < 64; t++)
        w[t] = SSIG1(w[t - 2]) + w[t - 7] + SSIG0(w[t - 15]) + w[t - 16];

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (t = 0; t < 64; t++) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[t] + w[t];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        h           = g;
        g           = f;
        f           = e;
        e           = d + t1;
        d           = c;
        c           = b;
        b           = a;
        a           = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_init(sha256_ctx *c)
{
    c->state[0] = 0x6a09e667;
    c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372;
    c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f;
    c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab;
    c->state[7] = 0x5be0cd19;
    c->bitlen   = 0;
    c->buflen   = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    c->bitlen += (uint64_t)len * 8;

    if (c->buflen) {
        size_t need = SHA256_BLOCK_SIZE - c->buflen;
        size_t take = len < need ? len : need;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == SHA256_BLOCK_SIZE) {
            sha256_transform(c->state, c->buf);
            c->buflen = 0;
        }
    }

    while (len >= SHA256_BLOCK_SIZE) {
        sha256_transform(c->state, p);
        p += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }

    if (len) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE])
{
    uint64_t bitlen = c->bitlen;
    int      i;

    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        memset(c->buf + c->buflen, 0, SHA256_BLOCK_SIZE - c->buflen);
        sha256_transform(c->state, c->buf);
        c->buflen = 0;
    }
    memset(c->buf + c->buflen, 0, 56 - c->buflen);
    for (i = 0; i < 8; i++)
        c->buf[56 + i] = (uint8_t)(bitlen >> (56 - i * 8));
    sha256_transform(c->state, c->buf);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

void sha256(const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, msg, len);
    sha256_final(&c, out);
}

void sha256d(const void *msg, size_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
    uint8_t first[SHA256_DIGEST_SIZE];
    sha256(msg, len, first);
    sha256(first, SHA256_DIGEST_SIZE, out);
}
