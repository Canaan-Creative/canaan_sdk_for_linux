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

#define _POSIX_C_SOURCE 200809L
#include "work.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STRATUM_FIXTURES_DIR
/* Directory holding the stratum/ fixtures when the build does not set it */
#define STRATUM_FIXTURES_DIR "."
#endif
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

static int split_row(char *line, char **c)
{
    int   i = 0;
    char *sp, *t = strtok_r(line, "\t", &sp);
    while (t && i < 16) {
        c[i++] = t;
        t      = strtok_r(NULL, "\t", &sp);
    }
    return i;
}

static int t_hex2bin(const char *hex, uint8_t *out, int maxout)
{
    int n = 0;
    for (const char *p = hex; p && p[0] && p[1]; p += 2) {
        if (n >= maxout)
            return -1;
        char b[3] = { p[0], p[1], 0 }, *end;
        long v    = strtol(b, &end, 16);
        if (end != b + 2)
            return -1;
        out[n++] = (uint8_t)v;
    }
    return n;
}

static uint32_t t_le32(const uint8_t *o)
{
    return (uint32_t)o[0] | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
}

static int t_meets_target(const mc_job *j, const char *diffstr)
{
    uint8_t hdr[80], root[32], h_be[32];
    if (mc_build_header(j, hdr, root) != 0)
        return 0;
    mc_header_hash(hdr, h_be);
    return mc_meets_target_hash(h_be, diffstr);
}

static void test_fixture_shares(void)
{
    FILE *f = fopen(STRATUM_FIXTURES_DIR "/stratum/happy.shares.tsv", "rb");
    if (!f) {
        fprintf(stderr, "  FAIL: happy.shares.tsv not found\n");
        fails++;
        return;
    }

    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     acc = 0, verified = 0, built = 0, first = 1;
    int     split_same = 0, tmpl_blank = 0, hdr_verified = 0;
    int     hdr_layout = 0, tmpl_layout = 0;
    uint8_t root_prev[32] = { 0 };
    while ((n = getline(&buf, &cap, f)) > 0) {
        char work[8192];
        if ((size_t)n >= sizeof work)
            continue;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        memcpy(work, buf, (size_t)n + 1);
        char *c[16] = { 0 };
        if (split_row(work, c) < 16)
            continue;
        if (strcmp(c[15], "1") != 0)
            continue;
        acc++;
        mc_job  j = { .version = c[1],
             .prevhash         = c[2],
             .coinb1           = c[3],
             .coinb2           = c[4],
             .merkle           = c[5],
             .nbits            = c[6],
             .ntime            = c[10],
             .xnonce1          = c[8],
             .xnonce2          = c[9],
             .nonce            = c[11],
             .version_bits     = c[12],
             .mask             = c[13] };
        uint8_t hdr[80], root[32];
        if (mc_build_header(&j, hdr, root) == 0) {
            built++;
            if (first) {
                memcpy(root_prev, root, 32);
                first = 0;
            }
            uint32_t vraw  = (uint32_t)strtoul(c[1], NULL, 16),
                     mask  = (uint32_t)strtoul(c[13], NULL, 16),
                     vb    = (uint32_t)strtoul(c[12], NULL, 16);
            uint32_t vroll = (vraw & ~mask) | (vb & mask);
            uint32_t vhdr  = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            if (vhdr != vroll) {
                fprintf(stderr, "  version-roll mismatch\n");
                fails++;
            }

            uint8_t ph[32];
            int     lay = t_hex2bin(c[2], ph, 32) == 32;
            if (lay && t_le32(hdr + 0) != vroll)
                lay = 0;
            for (int w = 0; lay && w < 8; w++)
                for (int k = 0; k < 4; k++)
                    if (hdr[4 + w * 4 + k] != ph[w * 4 + 3 - k])
                        lay = 0;
            if (lay && memcmp(hdr + 36, root, 32) != 0)
                lay = 0;
            if (lay && t_le32(hdr + 68) != (uint32_t)strtoul(c[10], NULL, 16))
                lay = 0;
            if (lay && t_le32(hdr + 72) != (uint32_t)strtoul(c[6], NULL, 16))
                lay = 0;
            if (lay && t_le32(hdr + 76) != (uint32_t)strtoul(c[11], NULL, 16))
                lay = 0;
            if (lay)
                hdr_layout++;

            uint8_t              tmpl[80], sroot[32], shdr[80];
            static const uint8_t zero32[32] = { 0 };
            memset(tmpl, 0xAA, sizeof tmpl);
            memset(shdr, 0xAA, sizeof shdr);
            if (mc_header_tmpl(&j, tmpl) == 0 && mc_merkle_root(&j, sroot) == 0) {
                uint32_t vtmpl = tmpl[0] | (tmpl[1] << 8) | (tmpl[2] << 16) |
                        ((uint32_t)tmpl[3] << 24);
                if (memcmp(tmpl + 36, zero32, 32) == 0 && tmpl[76] == 0 && tmpl[77] == 0 &&
                        tmpl[78] == 0 && tmpl[79] == 0 && vtmpl == vraw)
                    tmpl_blank++;
                int tl = 1;
                for (int w = 0; tl && w < 8; w++)
                    for (int k = 0; k < 4; k++)
                        if (tmpl[4 + w * 4 + k] != ph[w * 4 + 3 - k])
                            tl = 0;
                if (tl && t_le32(tmpl + 68) != (uint32_t)strtoul(c[10], NULL, 16))
                    tl = 0;
                if (tl && t_le32(tmpl + 72) != (uint32_t)strtoul(c[6], NULL, 16))
                    tl = 0;
                if (tl)
                    tmpl_layout++;
                mc_header_apply(shdr, tmpl, sroot, (uint32_t)strtoul(c[10], NULL, 16),
                        (uint32_t)strtoul(c[11], NULL, 16), vb, mask);
                if (memcmp(shdr, hdr, 80) == 0 && memcmp(sroot, root, 32) == 0)
                    split_same++;
                uint8_t sh[32];
                mc_header_hash(shdr, sh);
                if (mc_meets_target_hash(sh, c[14]))
                    hdr_verified++;
            }
        }
        if (t_meets_target(&j, c[14]))
            verified++;
    }
    free(buf);
    fclose(f);

    CHECK(acc >= 1, "accepted shares in fixture");
    CHECK(built == acc, "mc_build_header built every accepted share");
    CHECK(verified == acc,
            "mc_build_header + mc_meets_target_hash: every accepted share verifies (product sha)");
    CHECK(hdr_layout == acc,
            "mc_build_header: version 0..3, prevhash 4..35, merkle 36..67, "
            "ntime 68..71, nbits 72..75, nonce 76..79");
    CHECK(tmpl_blank == acc,
            "mc_header_tmpl: blank merkle root, zero nonce, version unrolled (pool->header_bin)");
    CHECK(tmpl_layout == acc,
            "mc_header_tmpl: the job's prevhash, ntime and nbits at those same "
            "offsets");
    CHECK(split_same == acc,
            "mc_header_tmpl+mc_merkle_root+mc_header_apply == mc_build_header, byte for byte");
    CHECK(hdr_verified == acc,
            "mc_meets_target_hash: every accepted share verifies through the stored template");

    (void)root_prev;
}

static void test_target(void)
{
    uint8_t t1[32], t2[32];
    mc_target_from_diff("1", t1);
    CHECK(t1[0] == 0 && t1[4] == 0xff && t1[5] == 0xff && t1[6] == 0,
            "diff 1 -> bdiff-1 target shape");
    mc_target_from_diff("8192", t2);
    CHECK(memcmp(t2, t1, 32) < 0, "higher diff -> smaller (harder) target");

    uint8_t th[32];
    mc_target_from_diff("0.5", th);
    CHECK(memcmp(th, t1, 32) > 0, "diff 0.5 -> larger (easier) target than diff 1");
    CHECK(th[3] == 0x01 && th[4] == 0xff && th[5] == 0xfe && th[6] == 0,
            "diff 0.5 target == diff-1 target << 1");
    uint8_t t3[32];
    mc_target_from_diff("8192.0", t3);
    CHECK(memcmp(t3, t2, 32) == 0, "8192.0 == 8192 (integer path unchanged)");
    mc_target_from_diff("0", t3);
    CHECK(memcmp(t3, t1, 32) == 0, "diff 0 -> clamped to diff 1");
}

static void test_asic_target(void)
{
    uint32_t h, l;
    mc_asic_target("1", &h, &l);
    CHECK(h == 0xFFFF0000u && l == 0x00000000u, "diff 1 -> REG_TargetH=0xFFFF0000, L=0");

    mc_asic_target("4096", &h, &l);
    CHECK(h == 0x000FFFF0u && l == 0x00000000u,
            "diff 4096 (MC_ASIC_DIFF_MAX) -> REG_TargetH=0x000FFFF0, L=0 (API easy target)");
    mc_asic_target("1000000", &h, &l);
    CHECK(h == 0x000FFFF0u && l == 0x00000000u,
            "diff > MC_ASIC_DIFF_MAX clamps to the same absolute TargetH/L pair");

    uint8_t t[32];
    mc_target_from_diff("1024", t);
    uint32_t eh = ((uint32_t)t[4] << 24) | ((uint32_t)t[5] << 16) | ((uint32_t)t[6] << 8) | t[7];
    uint32_t el = ((uint32_t)t[8] << 24) | ((uint32_t)t[9] << 16) | ((uint32_t)t[10] << 8) | t[11];
    mc_asic_target("1024", &h, &l);
    CHECK(h == eh && l == el, "diff 1024 (<=max) regs == middle-64 of the full target");

    uint8_t tc[32];
    mc_target_from_diff("4096", tc);
    uint32_t ch = ((uint32_t)tc[4] << 24) | ((uint32_t)tc[5] << 16) | ((uint32_t)tc[6] << 8) |
            tc[7];
    uint32_t cl = ((uint32_t)tc[8] << 24) | ((uint32_t)tc[9] << 16) | ((uint32_t)tc[10] << 8) |
            tc[11];
    uint32_t h8, l8, hb, lb;
    mc_asic_target("8192", &h8, &l8);
    CHECK(h8 == ch && l8 == cl, "diff 8192 -> chip target clamped to MC_ASIC_DIFF_MAX (4096)");
    mc_asic_target("100000", &hb, &lb);
    CHECK(hb == h8 && lb == l8, "diff 100000 -> same clamped chip target");

    uint8_t tt[32];
    mc_target_from_diff("8192", tt);
    CHECK(memcmp(tt, tc, 32) != 0,
            "software target(8192) != clamped(4096): true diff kept for verify");

    uint32_t h1, l1, h2, l2;
    mc_asic_target("1", &h1, &l1);
    mc_asic_target("4096", &h2, &l2);
    uint64_t v1 = ((uint64_t)h1 << 32) | l1, v2 = ((uint64_t)h2 << 32) | l2;
    CHECK(v2 < v1, "higher diff -> smaller (harder) ASIC target");
}

static uint32_t bswap32_(uint32_t v)
{
    return __builtin_bswap32(v);
}
static void test_chip_record_byteswap(void)
{
    FILE *f = fopen(STRATUM_FIXTURES_DIR "/stratum/happy.shares.tsv", "rb");
    if (!f) {
        fprintf(stderr, "  FAIL: happy.shares.tsv not found\n");
        fails++;
        return;
    }
    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     done = 0;
    while (!done && (n = getline(&buf, &cap, f)) > 0) {
        char work[8192];
        if ((size_t)n >= sizeof work)
            continue;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        memcpy(work, buf, (size_t)n + 1);
        char *c[16] = { 0 };
        if (split_row(work, c) < 16)
            continue;
        if (strcmp(c[15], "1") != 0)
            continue;

        uint32_t vraw = (uint32_t)strtoul(c[1], NULL, 16),
                 mask = (uint32_t)strtoul(c[13], NULL, 16), vb = (uint32_t)strtoul(c[12], NULL, 16);
        uint32_t true_version = (vraw & ~mask) | (vb & mask);
        uint32_t true_nonce   = (uint32_t)strtoul(c[11], NULL, 16);
        uint32_t rec_nonce    = bswap32_(true_nonce);
        uint32_t rec_version  = bswap32_(true_version);

        char nh[9], vbh[9];
        snprintf(nh, sizeof nh, "%08x", bswap32_(rec_nonce));
        snprintf(vbh, sizeof vbh, "%08x", bswap32_(rec_version) & mask);
        mc_job jok = { .version = c[1],
            .prevhash           = c[2],
            .coinb1             = c[3],
            .coinb2             = c[4],
            .merkle             = c[5],
            .nbits              = c[6],
            .ntime              = c[10],
            .xnonce1            = c[8],
            .xnonce2            = c[9],
            .nonce              = nh,
            .version_bits       = vbh,
            .mask               = c[13] };
        CHECK(t_meets_target(&jok, c[14]), "chip-record bswap -> hash verifies");

        char nbad[9], vbad[9];
        snprintf(nbad, sizeof nbad, "%08x", rec_nonce);
        snprintf(vbad, sizeof vbad, "%08x", rec_version & mask);
        mc_job jbad       = jok;
        jbad.nonce        = nbad;
        jbad.version_bits = vbad;
        CHECK(!t_meets_target(&jbad, c[14]), "un-swapped record does NOT verify (bug guard)");
        done = 1;
    }
    free(buf);
    fclose(f);
    CHECK(done, "found an accepted share to exercise the byteswap contract");
}

static void test_share_metrics(void)
{
    FILE *f = fopen(STRATUM_FIXTURES_DIR "/stratum/happy.shares.tsv", "rb");
    if (!f) {
        fprintf(stderr, "  FAIL: happy.shares.tsv not found\n");
        fails++;
        return;
    }
    char   *buf = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     done = 0;
    while (!done && (n = getline(&buf, &cap, f)) > 0) {
        char work[8192];
        if ((size_t)n >= sizeof work)
            continue;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        memcpy(work, buf, (size_t)n + 1);
        char *c[16] = { 0 };
        if (split_row(work, c) < 16)
            continue;
        if (strcmp(c[15], "1") != 0)
            continue;
        done = 1;

        mc_job  j = { .version = c[1],
             .prevhash         = c[2],
             .coinb1           = c[3],
             .coinb2           = c[4],
             .merkle           = c[5],
             .nbits            = c[6],
             .ntime            = c[10],
             .xnonce1          = c[8],
             .xnonce2          = c[9],
             .nonce            = c[11],
             .version_bits     = c[12],
             .mask             = c[13] };
        uint8_t hdr[80], root[32];
        if (mc_build_header(&j, hdr, root) != 0) {
            fprintf(stderr, "  FAIL: could not build the share header\n");
            fails++;
            break;
        }
        uint8_t h[32], be[32], d1[32] = { 0 };
        sha256d(hdr, 80, h);
        for (int i = 0; i < 32; i++)
            be[i] = h[31 - i];
        d1[4]     = 0xff;
        d1[5]     = 0xff;
        double hd = 0.0, dd = 0.0;
        for (int i = 0; i < 32; i++) {
            hd = hd * 256.0 + be[i];
            dd = dd * 256.0 + d1[i];
        }
        uint8_t hb[32];
        mc_header_hash(hdr, hb);
        CHECK(memcmp(hb, be, 32) == 0,
                "mc_header_hash = sha256d of the header, byte-reversed to big-endian");
        char   m[256];
        double want = dd / hd, got = mc_share_diff_hash(hb);
        snprintf(m, sizeof m,
                "mc_share_diff_hash = bdiff-1 target / hash "
                "(%.6f, want %.6f)",
                got, want);
        CHECK(got > want * (1 - 1e-12) && got < want * (1 + 1e-12), m);
        snprintf(m, sizeof m,
                "...so an accepted share clears the difficulty it "
                "was judged at (%.6f >= %s)",
                got, c[14]);
        CHECK(got >= strtod(c[14], NULL), m);

        int ofs = 0;
        while (ofs <= 28 && !be[ofs])
            ofs++;
        uint32_t wh = ((uint32_t)be[ofs] << 24) | ((uint32_t)be[ofs + 1] << 16) |
                ((uint32_t)be[ofs + 2] << 8) | (uint32_t)be[ofs + 3];
        uint32_t gh = mc_share_hash_h32(hb);
        snprintf(m, sizeof m,
                "mc_share_hash_h32 = the BE hash's top word past "
                "its leading zero bytes (%08x, want %08x)",
                gh, wh);
        CHECK(gh == wh, m);
        snprintf(m, sizeof m,
                "...the %d leading zero bytes are skipped, so the "
                "word is significant",
                ofs);
        CHECK(ofs > 0 && (gh >> 24) != 0, m);
        CHECK(gh !=
                        (((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) |
                                (uint32_t)h[3]),
                "...and it is the BIG-endian reading, not the raw digest's first word");
    }
    free(buf);
    fclose(f);
    CHECK(done, "found an accepted share to measure");
}

static void test_coinbase_height(void)
{
    const char *cb1 = "01000000"
                      "01"
                      "0000000000000000000000000000000000000000000000000000000000000000"
                      "ffffffff"
                      "64"
                      "03"
                      "ab990e"
                      "deadbeef";
    CHECK(mc_coinbase_height(cb1) == 956843, "BIP34 height parsed from coinb1 (LE)");
    CHECK(mc_coinbase_height("0102") == 0, "too-short coinb1 -> 0");
    CHECK(mc_coinbase_height(NULL) == 0, "NULL coinb1 -> 0");
}

static void test_coin_fields(void)
{
    char bh[65];
    mc_block_hash_hex("59f51935b45111a8d968826b7fd97be3"
                      "50b955d40001cb410000000000000000",
            bh);
    CHECK(!strcmp(bh,
                  "00000000000000000001cb4150b955d4"
                  "7fd97be3d968826bb45111a859f51935"),
            "Current Block Hash = the captured prevhash, word order reversed");
    mc_block_hash_hex("0102030405060708090a0b0c0d0e0f10"
                      "1112131415161718191a1b1c1d1e1f20",
            bh);
    CHECK(!strcmp(bh,
                  "1d1e1f20191a1b1c1516171811121314"
                  "0d0e0f10090a0b0c0506070801020304"),
            "the words are reversed in order, not the bytes inside them");
    mc_block_hash_hex("abcd", bh);
    CHECK(bh[0] == '\0', "a short prevhash gives no block hash");
    mc_block_hash_hex("zz02030405060708090a0b0c0d0e0f10"
                      "1112131415161718191a1b1c1d1e1f20",
            bh);
    CHECK(bh[0] == '\0', "a non-hex prevhash gives no block hash");
    mc_block_hash_hex(NULL, bh);
    CHECK(bh[0] == '\0', "NULL prevhash gives no block hash");

    char d[32];
    snprintf(d, sizeof d, "%.8f", mc_net_diff_from_nbits("17021a42"));
    CHECK(!strcmp(d, "133869853540305.39062500"),
            "Network Difficulty reproduces the captured reply from its nbits");
    CHECK(mc_net_diff_from_nbits("1d00ffff") == 1.0, "the genesis nbits is difficulty 1");
    CHECK(mc_net_diff_from_nbits("1e00ffff") == 1.0, "an exponent past 0x1d clamps the shift");
    CHECK(mc_net_diff_from_nbits("1700ffff") == 281474976710656.0,
            "the 0x17 exponent shifts the numerator by 48 bits");
    CHECK(mc_net_diff_from_nbits("17000000") == 0.0, "a zero mantissa gives 0");
    CHECK(mc_net_diff_from_nbits("1600ffff") == 72057594037927936.0,
            "the 0x16 exponent shifts the numerator by 56 bits");
    CHECK(mc_net_diff_from_nbits("16021a42") == 256.0 * mc_net_diff_from_nbits("17021a42"),
            "one exponent step down multiplies the difficulty by 256");
    CHECK(mc_net_diff_from_nbits("1500ffff") == 18446744073709551616.0,
            "the 0x15 exponent shifts the numerator by 64 bits");
    CHECK(mc_net_diff_from_nbits("170") == 0.0, "a short nbits gives 0");
    CHECK(mc_net_diff_from_nbits(NULL) == 0.0, "NULL nbits gives 0");
}

static void test_strict_hex(void)
{
    static const char  ph[]    = "0102030405060708090a0b0c0d0e0f10"
                                 "1112131415161718191a1b1c1d1e1f20";
    static const char  br[]    = "2122232425262728292a2b2c2d2e2f30"
                                 "3132333435363738393a3b3c3d3e3f40";
    static const char *pairs[] = { " f", "+f", "-f" };
    mc_job             j       = { .version = "20000000",
                          .prevhash         = ph,
                          .coinb1           = "0100",
                          .coinb2           = "ffff",
                          .merkle           = br,
                          .nbits            = "1d00ffff",
                          .ntime            = "5f5e1000",
                          .xnonce1          = "aabb",
                          .xnonce2          = "00000001" };
    uint8_t            root[32], tmpl[80];
    char               bh[65], buf[66];

    CHECK(mc_merkle_root(&j, root) == 0, "strict hex: the baseline job folds a merkle root");
    CHECK(mc_header_tmpl(&j, tmpl) == 0, "strict hex: the baseline job builds a header");

    j.coinb1 = "010";
    CHECK(mc_merkle_root(&j, root) == -1, "strict hex: an odd-length coinb1 fails");
    j.coinb1 = "0100";
    j.merkle = "2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404";
    CHECK(mc_merkle_root(&j, root) == -1, "strict hex: an odd-length merkle branch fails");
    j.merkle = br;
    snprintf(buf, sizeof buf, "%s0", ph);
    j.prevhash = buf;
    CHECK(mc_header_tmpl(&j, tmpl) == -1, "strict hex: an odd-length prevhash fails");

    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        char msg[96];
        memcpy(buf, ph, sizeof ph);
        memcpy(buf, pairs[i], 2);
        j.prevhash = buf;
        snprintf(msg, sizeof msg, "strict hex: prevhash \"%s..\" fails the header", pairs[i]);
        CHECK(mc_header_tmpl(&j, tmpl) == -1, msg);
        mc_block_hash_hex(buf, bh);
        snprintf(msg, sizeof msg, "strict hex: prevhash \"%s..\" gives no block hash", pairs[i]);
        CHECK(bh[0] == '\0', msg);
        j.prevhash = ph;

        char cb1[5] = "0100";
        memcpy(cb1 + 2, pairs[i], 2);
        j.coinb1 = cb1;
        snprintf(msg, sizeof msg, "strict hex: coinb1 \"01%s\" fails the merkle root", pairs[i]);
        CHECK(mc_merkle_root(&j, root) == -1, msg);
        j.coinb1 = "0100";

        char nb[9] = "1d00ffff";
        memcpy(nb + 6, pairs[i], 2);
        snprintf(msg, sizeof msg, "strict hex: nbits \"1d00ff%s\" gives 0", pairs[i]);
        CHECK(mc_net_diff_from_nbits(nb) == 0.0, msg);
    }
}

int main(void)
{
    test_fixture_shares();
    test_share_metrics();
    test_coinbase_height();
    test_coin_fields();
    test_strict_hex();
    test_chip_record_byteswap();
    test_target();
    test_asic_target();
    if (fails) {
        fprintf(stderr, "test_work: %d FAIL\n", fails);
        return 1;
    }
    printf("test_work: all pass\n");
    return 0;
}
