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

#include "harness_todo.h"
#include "harness_fixture.h"
#include "harness_share.h"
#include "work.h"
#include "share.h"

static mc_job row_mcjob(char **c)
{
    mc_job j = { .version = c[1],
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
    return j;
}

static int job_meets_target(const mc_job *j, const char *diffstr)
{
    uint8_t hdr[80], root[32], h_be[32];
    if (mc_build_header(j, hdr, root) != 0)
        return 0;
    mc_header_hash(hdr, h_be);
    return mc_meets_target_hash(h_be, diffstr);
}

static int split_row(char *line, char **cols)
{
    int   i = 0;
    char *sp, *t = strtok_r(line, "\t", &sp);
    while (t && i < 16) {
        cols[i++] = t;
        t         = strtok_r(NULL, "\t", &sp);
    }
    return i;
}

static HsShare row_share(char **c)
{
    HsShare s = { .version = c[1],
        .prevhash          = c[2],
        .coinb1            = c[3],
        .coinb2            = c[4],
        .merkle            = c[5],
        .nbits             = c[6],
        .ntime             = c[10],
        .xnonce1           = c[8],
        .xnonce2           = c[9],
        .nonce             = c[11],
        .version_bits      = c[12],
        .mask              = c[13],
        .diff              = c[14] };
    return s;
}

int main(int argc, char **argv)
{
    const char *id = argc > 1 ? argv[1] : "";

    if (!strcmp(id, "mining_notify_header")) {
        hf_fails  = 0;
        char *tsv = hf_read("stratum/happy.shares.tsv");
        HF_CHECK(tsv != NULL, "shares.tsv present");
        if (tsv) {
            char *sl, *line = strtok_r(tsv, "\n", &sl);
            line        = strtok_r(NULL, "\n", &sl);
            char *c[16] = { 0 };
            int   found = 0;
            while (line) {
                if (split_row(line, c) >= 16 && !strcmp(c[15], "1")) {
                    found = 1;
                    break;
                }
                line = strtok_r(NULL, "\n", &sl);
            }
            HF_CHECK(found, "an accepted share (captured job) to build from");
            if (found) {
                HsShare s = row_share(c);
                uint8_t h1[80], m1[32], h2[80], m2[32], zero[32] = { 0 };
                HF_CHECK(hs_build(&s, h1, m1) == 0,
                        "notify->mm_work: coinbase+merkle+header built");
                HF_CHECK(hs_build(&s, h2, m2) == 0 && memcmp(m1, m2, 32) == 0,
                        "merkle root deterministic");
                HF_CHECK(memcmp(m1, zero, 32) != 0, "merkle root non-zero");
                HF_CHECK(c[5][0] != '\0', "job carried merkle branch(es) to fold");
                uint32_t vraw  = (uint32_t)strtoul(c[1], NULL, 16),
                         mask  = (uint32_t)strtoul(c[13], NULL, 16),
                         vb    = (uint32_t)strtoul(c[12], NULL, 16);
                uint32_t vroll = (vraw & ~mask) | (vb & mask);
                uint32_t vhdr  = h1[0] | (h1[1] << 8) | (h1[2] << 16) | ((uint32_t)h1[3] << 24);
                HF_CHECK(vhdr == vroll, "header carries the version-rolled value");
                mc_job  mj = row_mcjob(c);
                uint8_t ph[80], pm[32];
                HF_CHECK(mc_build_header(&mj, ph, pm) == 0 && memcmp(ph, h1, 80) == 0 &&
                                memcmp(pm, m1, 32) == 0,
                        "miner_core mc_build_header == oracle (header + merkle byte-identical)");
            }
        }
        free(tsv);
        HF_DONE("mining_notify_header");
    }
    if (!strcmp(id, "mining_share_validate")) {
        hf_fails  = 0;
        char *tsv = hf_read("stratum/happy.shares.tsv");
        HF_CHECK(tsv != NULL, "shares.tsv present");
        if (tsv) {
            int      acc = 0, matched = 0, verified = 0, pverified = 0, dups = 0;
            mc_dedup dd;
            mc_dedup_init(&dd);
            char *sl, *line = strtok_r(tsv, "\n", &sl);
            line = strtok_r(NULL, "\n", &sl);
            while (line) {
                char *c[16] = { 0 };
                if (split_row(line, c) >= 16 && !strcmp(c[15], "1")) {
                    acc++;
                    if (c[1][0] && c[2][0])
                        matched++;
                    HsShare s = row_share(c);
                    if (hs_meets_target(&s))
                        verified++;
                    mc_job mj = row_mcjob(c);
                    if (job_meets_target(&mj, c[14]))
                        pverified++;
                    if (mc_dedup_check(&dd, c[0], c[9], c[10], c[11], NULL))
                        dups++;
                }
                line = strtok_r(NULL, "\n", &sl);
            }
            HF_CHECK(acc >= 1, "accepted shares present (%d)", acc);
            HF_CHECK(matched == acc, "match: every share bound to a captured job (%d/%d)", matched,
                    acc);
            HF_CHECK(verified == acc,
                    "hash-verify (oracle): every accepted share <= target (%d/%d)", verified, acc);
            HF_CHECK(pverified == acc,
                    "hash-verify (miner_core): every accepted share <= target (%d/%d)", pverified,
                    acc);
            HF_CHECK(dups == 0,
                    "dedup (miner_core): no duplicate (job,xnonce2,ntime,nonce) (%d dup)", dups);
        }
        free(tsv);
        HF_DONE("mining_share_validate");
    }
    if (!strcmp(id, "mining_share_submit")) {
        hf_fails  = 0;
        char *tsv = hf_read("stratum/happy.shares.tsv");
        HF_CHECK(tsv != NULL, "shares.tsv present");
        if (tsv) {
            int      total = 0, rolled = 0, wellformed = 1;
            mc_tally tally;
            mc_tally_init(&tally);
            char *sl, *line = strtok_r(tsv, "\n", &sl);
            line = strtok_r(NULL, "\n", &sl);
            while (line) {
                char *cols[16] = { 0 };
                int   i        = 0;
                char *st2, *tok = strtok_r(line, "\t", &st2);
                while (tok && i < 16) {
                    cols[i++] = tok;
                    tok       = strtok_r(NULL, "\t", &st2);
                }
                if (i >= 16) {
                    total++;
                    mc_tally_record(&tally, !strcmp(cols[15], "1"));
                    if (strcmp(cols[10], cols[7]) != 0)
                        rolled++;
                    if (strlen(cols[11]) != 8 || strlen(cols[10]) != 8)
                        wellformed = 0;
                } else {
                    wellformed = 0;
                }
                line = strtok_r(NULL, "\n", &sl);
            }
            HF_CHECK(total >= 1, "at least one submit captured (%d)", total);
            HF_CHECK(tally.accepted > 0, "a/r tally (miner_core): %u/%d accepted", tally.accepted,
                    total);
            HF_CHECK(tally.accepted + tally.rejected == (uint32_t)total, "tally totals reconcile");
            HF_CHECK(wellformed, "submit nonce/ntime are 8-hex, every row complete");
            HF_CHECK(rolled >= 1, "ntime roll observed (submit ntime != job ntime) in %d share(s)",
                    rolled);
        }
        free(tsv);
        HF_DONE("mining_share_submit");
    }

    HARNESS_UNKNOWN(id);
}
