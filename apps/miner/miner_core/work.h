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

#ifndef WORK_H
#define WORK_H

#include <stdint.h>

/*
 * Chip target cap. The chip only pre-filters; the software verify uses the
 * true pool difficulty, so a higher pool diff just yields extra nonces.
 */
#define MC_ASIC_DIFF_MAX 4096

/* A job + candidate reduced to the SV1/stratum hex fields (as delivered by the
 * sv1 client). `merkle` is the comma-separated branch list;
 * `nonce`/`ntime`/`version_bits`/`mask` describe the candidate being
 * built/verified. */
typedef struct {
    const char *version;      /* block version (hex)                    */
    const char *prevhash;     /* 32-byte prev hash (hex)                */
    const char *coinb1;       /* coinbase part 1 (hex)                  */
    const char *coinb2;       /* coinbase part 2 (hex)                  */
    const char *merkle;       /* comma-separated 32-byte branches (hex) */
    const char *nbits;        /* difficulty bits (hex)                  */
    const char *ntime;        /* timestamp for this candidate (hex)     */
    const char *xnonce1;      /* extranonce1 (hex)                      */
    const char *xnonce2;      /* extranonce2 (hex)                      */
    const char *nonce;        /* nonce (hex)                            */
    const char *version_bits; /* version-rolling bits (hex, may be "")  */
    const char *mask;         /* version-rolling mask (hex, may be "")  */
} mc_job;

int      mc_build_header(const mc_job *j, uint8_t header[80], uint8_t merkle_root[32]);
int      mc_header_tmpl(const mc_job *j, uint8_t tmpl[80]);
int      mc_merkle_root(const mc_job *j, uint8_t root[32]);
void     mc_header_apply(uint8_t hdr[80], const uint8_t tmpl[80], const uint8_t merkle_root[32],
            uint32_t ntime, uint32_t nonce, uint32_t version_bits, uint32_t mask);
void     mc_header_hash(const uint8_t hdr[80], uint8_t h_be[32]);
int      mc_meets_target_hash(const uint8_t h_be[32], const char *diffstr);
double   mc_share_diff_hash(const uint8_t h_be[32]);
uint32_t mc_share_hash_h32(const uint8_t h_be[32]);
void     mc_target_from_diff(const char *diffstr, uint8_t tgt[32]);
uint32_t mc_coinbase_height(const char *coinb1_hex);
void     mc_block_hash_hex(const char *prevhash_hex, char out[65]);
double   mc_net_diff_from_nbits(const char *nbits_hex);
void     mc_asic_target(const char *diffstr, uint32_t *reg_target_h, uint32_t *reg_target_l);

#endif /* WORK_H */
