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

#include "work.h"
#include "sha256.h"
#include "hex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR_VERSION  0  /* block version, byte offset */
#define HDR_PREVHASH 4  /* previous block hash, byte offset */
#define HDR_MERKLE   36 /* merkle root, byte offset */
#define HDR_NTIME    68 /* timestamp, byte offset */
#define HDR_NBITS    72 /* difficulty bits, byte offset */
#define HDR_NONCE    76 /* nonce, byte offset */

static void put_le32(uint8_t *o, uint32_t v)
{
    o[0] = v & 0xff;
    o[1] = (v >> 8) & 0xff;
    o[2] = (v >> 16) & 0xff;
    o[3] = (v >> 24) & 0xff;
}

static uint32_t get_le32(const uint8_t *o)
{
    return (uint32_t)o[0] | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
}

int mc_merkle_root(const mc_job *j, uint8_t root[32])
{
    uint8_t     cb[16384];
    int         n        = 0, k;
    const char *parts[4] = { j->coinb1, j->xnonce1, j->xnonce2, j->coinb2 };
    for (int i = 0; i < 4; i++) {
        k = hex_decode_str(cb + n, sizeof cb - (size_t)n, parts[i]);
        if (k < 0)
            return -1;
        n += k;
    }
    sha256d(cb, (size_t)n, root);

    /* fold each merkle branch: root = dSHA256(root || branch) */
    char branches[8192];
    snprintf(branches, sizeof branches, "%s", j->merkle ? j->merkle : "");
    char *save;
    for (char *tok = strtok_r(branches, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        uint8_t br[32], cat[64];
        if (hex_decode_str(br, sizeof br, tok) != 32)
            return -1;
        memcpy(cat, root, 32);
        memcpy(cat + 32, br, 32);
        sha256d(cat, 64, root);
    }
    return 0;
}

int mc_header_tmpl(const mc_job *j, uint8_t tmpl[80])
{
    uint8_t ph[32];
    if (hex_decode_str(ph, sizeof ph, j->prevhash) != 32)
        return -1;

    memset(tmpl, 0, 80); /* blank merkle root + zero nonce */
    put_le32(tmpl + HDR_VERSION, (uint32_t)strtoul(j->version, NULL, 16));
    for (int w = 0; w < 8; w++) { /* prevhash: reverse each 4-byte word */
        tmpl[HDR_PREVHASH + w * 4 + 0] = ph[w * 4 + 3];
        tmpl[HDR_PREVHASH + w * 4 + 1] = ph[w * 4 + 2];
        tmpl[HDR_PREVHASH + w * 4 + 2] = ph[w * 4 + 1];
        tmpl[HDR_PREVHASH + w * 4 + 3] = ph[w * 4 + 0];
    }
    put_le32(tmpl + HDR_NTIME, (uint32_t)strtoul(j->ntime, NULL, 16));
    put_le32(tmpl + HDR_NBITS, (uint32_t)strtoul(j->nbits, NULL, 16));
    return 0;
}

void mc_header_apply(uint8_t hdr[80], const uint8_t tmpl[80], const uint8_t merkle_root[32],
        uint32_t ntime, uint32_t nonce, uint32_t version_bits, uint32_t mask)
{
    memcpy(hdr, tmpl, 80);
    uint32_t ver = get_le32(hdr + HDR_VERSION);
    put_le32(hdr + HDR_VERSION, (ver & ~mask) | (version_bits & mask));
    memcpy(hdr + HDR_MERKLE, merkle_root, 32); /* merkle root as-is */
    put_le32(hdr + HDR_NTIME, ntime);
    put_le32(hdr + HDR_NONCE, nonce);
}

int mc_build_header(const mc_job *j, uint8_t header[80], uint8_t merkle_root[32])
{
    uint8_t tmpl[80];
    if (mc_merkle_root(j, merkle_root) != 0)
        return -1;
    if (mc_header_tmpl(j, tmpl) != 0)
        return -1;

    uint32_t mask = (j->mask && *j->mask) ? (uint32_t)strtoul(j->mask, NULL, 16) : 0;
    uint32_t vb   = (j->version_bits && *j->version_bits) ?
              (uint32_t)strtoul(j->version_bits, NULL, 16) :
              0;
    mc_header_apply(header, tmpl, merkle_root, (uint32_t)strtoul(j->ntime, NULL, 16),
            (uint32_t)strtoul(j->nonce, NULL, 16), vb, mask);
    return 0;
}

static void target_from_diff_d(double d, uint8_t tgt[32])
{
    uint8_t d1s[35] = { 0 }; /* diff-1 target << 24 */
    d1s[4]          = 0xff;
    d1s[5]          = 0xff;
    if (!(d > 0.0))
        d = 1.0;
    if (d < 1.0 / (1 << 24))
        d = 1.0 / (1 << 24);
    if (d > 4294967296.0)
        d = 4294967296.0;
    unsigned long long num = (unsigned long long)(d * (1 << 24) + 0.5);
    if (num == 0)
        num = 1;
    unsigned long long rem = 0;
    for (int i = 0; i < 35; i++) {
        unsigned long long cur = (rem << 8) | d1s[i];
        unsigned char      q   = (unsigned char)(cur / num);
        rem                    = cur % num;
        if (i >= 3)
            tgt[i - 3] = q; /* top 3 quotient bytes are 0 */
    }
}

void mc_target_from_diff(const char *diffstr, uint8_t tgt[32])
{
    target_from_diff_d(diffstr ? strtod(diffstr, NULL) : 1.0, tgt);
}

void mc_header_hash(const uint8_t hdr[80], uint8_t h_be[32])
{
    uint8_t h[32];
    sha256d(hdr, 80, h);
    for (int i = 0; i < 32; i++)
        h_be[i] = h[31 - i]; /* LE hash -> BE */
}

int mc_meets_target_hash(const uint8_t h_be[32], const char *diffstr)
{
    uint8_t tgt[32];
    mc_target_from_diff(diffstr, tgt);
    return memcmp(h_be, tgt, 32) <= 0;
}

double mc_share_diff_hash(const uint8_t h_be[32])
{
    uint8_t d1[32];
    target_from_diff_d(1.0, d1);
    double hd = 0.0, dd = 0.0;
    for (int i = 0; i < 32; i++) {
        hd = hd * 256.0 + h_be[i];
        dd = dd * 256.0 + d1[i];
    }
    return hd > 0.0 ? dd / hd : 0.0;
}

uint32_t mc_share_hash_h32(const uint8_t h_be[32])
{
    int ofs = 0;
    while (ofs < 28 && !h_be[ofs])
        ofs++; /* keeps ofs + 3 inside h_be */
    return ((uint32_t)h_be[ofs] << 24) | ((uint32_t)h_be[ofs + 1] << 16) |
            ((uint32_t)h_be[ofs + 2] << 8) | (uint32_t)h_be[ofs + 3];
}

uint32_t mc_coinbase_height(const char *coinb1_hex)
{
    if (!coinb1_hex)
        return 0;
    size_t L = strlen(coinb1_hex);
    if (L < 86 || (L & 1))
        return 0; /* need bytes 0..42 */
    uint8_t op;
    if (hex_decode(&op, coinb1_hex + 84, 1) < 0)
        return 0; /* push opcode at byte 42 */
    int n = op;
    if (n >= 1 && n <= 4) {
        if (L < (size_t)(86 + 2 * n))
            return 0;
        uint32_t h = 0;
        for (int i = 0; i < n; i++) {
            uint8_t b;
            if (hex_decode(&b, coinb1_hex + 86 + 2 * i, 1) < 0)
                return 0;
            h |= (uint32_t)b << (8 * i);
        }
        return h;
    }
    if (n >= 0x51 && n <= 0x60)
        return (uint32_t)(n - 0x50);
    return 0; /* OP_0 or unexpected */
}

void mc_block_hash_hex(const char *prevhash_hex, char out[65])
{
    uint8_t ph[32], rev[32];
    out[0] = '\0';
    if (!prevhash_hex || strlen(prevhash_hex) != 64 || hex_decode(ph, prevhash_hex, 32) < 0)
        return;
    for (int w = 0; w < 8; w++) /* reverse word order */
        memcpy(rev + w * 4, ph + (7 - w) * 4, 4);
    hex_encode(out, rev, sizeof rev);
}

double mc_net_diff_from_nbits(const char *nbits_hex)
{
    uint8_t nb[4];
    if (!nbits_hex || strlen(nbits_hex) != 8 || hex_decode_str(nb, sizeof nb, nbits_hex) != 4)
        return 0.0;
    int powdiff = (8 * (0x1d - 3)) - (8 * ((int)nb[0] - 3));
    if (powdiff < 0)
        powdiff = 0;
    uint32_t mantissa = ((uint32_t)nb[1] << 16) | ((uint32_t)nb[2] << 8) | nb[3];
    if (!mantissa)
        return 0.0;
    return ldexp(0xFFFF, powdiff) / (double)mantissa;
}

void mc_asic_target(const char *diffstr, uint32_t *reg_target_h, uint32_t *reg_target_l)
{
    double d = diffstr ? strtod(diffstr, NULL) : 1.0;
    if (d > (double)MC_ASIC_DIFF_MAX)
        d = (double)MC_ASIC_DIFF_MAX;
    uint8_t t[32];
    target_from_diff_d(d, t);
    /* bits [223:192] are bytes 4..7, [191:160] are bytes 8..11 */
    *reg_target_h = ((uint32_t)t[4] << 24) | ((uint32_t)t[5] << 16) | ((uint32_t)t[6] << 8) |
            (uint32_t)t[7];
    *reg_target_l = ((uint32_t)t[8] << 24) | ((uint32_t)t[9] << 16) | ((uint32_t)t[10] << 8) |
            (uint32_t)t[11];
}
