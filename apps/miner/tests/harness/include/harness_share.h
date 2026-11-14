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

/*
 * SV1 share -> block-header reconstruction and hash-verify.
 *
 * The assembly below is the PUBLIC Bitcoin +
 * stratum V1 spec (coinbase = coinb1+xnonce1+xnonce2+coinb2; merkle root by
 * folding the branch; 80-byte header; version-rolling; double-SHA256 vs the
 * difficulty target) computed with the independent sha256_ref oracle.
 * The exact byte orders (prevhash 4-byte word-swap,
 * merkle root as-is, little-endian hash compare) were confirmed against the
 * captured pool-accepted shares: a correct build MUST reproduce hash <= target.
 */
#ifndef HARNESS_SHARE_H
#define HARNESS_SHARE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sha256_ref.h"

/* fields lifted from one stratum/happy.shares.tsv row (all hex except diff). */
typedef struct {
    /* block version, previous block hash, coinbase halves, merkle branches */
    const char *version, *prevhash, *coinb1, *coinb2, *merkle /* comma-sep */;
    /* nbits, ntime, extranonces, nonce, rolled version bits, rolling mask, difficulty */
    const char *nbits, *ntime, *xnonce1, *xnonce2, *nonce, *version_bits, *mask, *diff;
} HsShare;

__attribute__((unused)) static int hs_hex2bin(const char *hex, uint8_t *out, int maxout)
{
    int n = 0;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        if (n >= maxout)
            return -1;
        char  b[3] = { p[0], p[1], 0 };
        char *end;
        long  v = strtol(b, &end, 16);
        if (end != b + 2)
            return -1;
        out[n++] = (uint8_t)v;
    }
    return n;
}

__attribute__((unused)) static void hs_put_le32(uint8_t *o, uint32_t v)
{
    o[0] = v & 0xff;
    o[1] = (v >> 8) & 0xff;
    o[2] = (v >> 16) & 0xff;
    o[3] = (v >> 24) & 0xff;
}

__attribute__((unused)) static void hs_target(const char *diffstr, uint8_t tgt[32])
{
    uint8_t d1[32]       = { 0 };
    d1[4]                = 0xff;
    d1[5]                = 0xff; /* 0x00000000FFFF0000...0000 */
    unsigned long long d = strtoull(diffstr, NULL, 10);
    if (d == 0)
        d = 1;
    unsigned long long rem = 0;
    for (int i = 0; i < 32; i++) {
        unsigned long long cur = (rem << 8) | d1[i];
        tgt[i]                 = (uint8_t)(cur / d);
        rem                    = cur % d;
    }
}

__attribute__((unused)) static int hs_build(const HsShare *s, uint8_t header[80],
        uint8_t merkle[32])
{
    uint8_t     cb[16384];
    int         n        = 0, k;
    const char *parts[4] = { s->coinb1, s->xnonce1, s->xnonce2, s->coinb2 };
    for (int i = 0; i < 4; i++) {
        k = hs_hex2bin(parts[i], cb + n, (int)sizeof cb - n);
        if (k < 0)
            return -1;
        n += k;
    }
    sha256d_ref(cb, (size_t)n, merkle);

    /* fold each merkle branch: root = dSHA256(root || branch) */
    char branches[8192];
    strncpy(branches, s->merkle ? s->merkle : "", sizeof branches - 1);
    branches[sizeof branches - 1] = 0;
    for (char *tok = strtok(branches, ","); tok; tok = strtok(NULL, ",")) {
        uint8_t br[32], cat[64];
        if (hs_hex2bin(tok, br, 32) != 32)
            return -1;
        memcpy(cat, merkle, 32);
        memcpy(cat + 32, br, 32);
        sha256d_ref(cat, 64, merkle);
    }

    uint32_t ver  = (uint32_t)strtoul(s->version, NULL, 16);
    uint32_t mask = s->mask && *s->mask ? (uint32_t)strtoul(s->mask, NULL, 16) : 0;
    uint32_t vb =
            s->version_bits && *s->version_bits ? (uint32_t)strtoul(s->version_bits, NULL, 16) : 0;
    ver = (ver & ~mask) | (vb & mask); /* version rolling */

    uint8_t ph[32];
    if (hs_hex2bin(s->prevhash, ph, 32) != 32)
        return -1;

    int p = 0;
    hs_put_le32(header + p, ver);
    p += 4;
    for (int w = 0; w < 8; w++) { /* prevhash: reverse each 4-byte word */
        header[p + w * 4 + 0] = ph[w * 4 + 3];
        header[p + w * 4 + 1] = ph[w * 4 + 2];
        header[p + w * 4 + 2] = ph[w * 4 + 1];
        header[p + w * 4 + 3] = ph[w * 4 + 0];
    }
    p += 32;
    memcpy(header + p, merkle, 32);
    p += 32; /* merkle root as-is */
    hs_put_le32(header + p, (uint32_t)strtoul(s->ntime, NULL, 16));
    p += 4;
    hs_put_le32(header + p, (uint32_t)strtoul(s->nbits, NULL, 16));
    p += 4;
    hs_put_le32(header + p, (uint32_t)strtoul(s->nonce, NULL, 16));
    p += 4;
    return p == 80 ? 0 : -1;
}

__attribute__((unused)) static int hs_meets_target(const HsShare *s)
{
    uint8_t header[80], merkle[32], h[32], h_be[32], tgt[32];
    if (hs_build(s, header, merkle) != 0)
        return 0;
    sha256d_ref(header, 80, h);
    for (int i = 0; i < 32; i++)
        h_be[i] = h[31 - i]; /* LE hash -> BE magnitude */
    hs_target(s->diff, tgt);
    return memcmp(h_be, tgt, 32) <= 0;
}

#endif /* HARNESS_SHARE_H */
