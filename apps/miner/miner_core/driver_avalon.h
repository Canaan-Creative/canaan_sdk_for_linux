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

#ifndef DRIVER_AVALON_H
#define DRIVER_AVALON_H

#include <stdint.h>

#include "toast_work.h"
#include "toast_nonce.h"

/* Work as block-header words; maps 1:1 to the a3197s REG_Work image. */
typedef struct {
    uint32_t version[TOAST_WORK_VERSION_WORDS]; /* version-rolling set */
    uint32_t prev_hash[TOAST_WORK_HASH_WORDS];
    uint32_t merkle_root[TOAST_WORK_HASH_WORDS];
    uint32_t ntime;
    uint32_t nbits;                            /* compact network target */
    uint32_t user_note[TOAST_WORK_NOTE_WORDS]; /* 128-bit user note */
} avalon_job_in;

/* Decoded struct toast_nonce. */
typedef struct {
    uint32_t job_id;          /* job id the work carried */
    uint32_t nonce2;          /* extranonce2 of the work */
    uint32_t last_job_nonce2; /* last extranonce2 of the previous job */
    uint32_t nonce;
    uint32_t version;  /* version hashed; roll bits are version & mask */
    uint16_t asic_id;  /* chip index on the chain */
    uint16_t miner_id; /* hashboard index */
    uint8_t  ntime;    /* roll offset added to the job's ntime */
    uint8_t  mid_id;   /* 4-bit mid_id field of the result */
    uint8_t  valid;    /* 4-bit valid field of the result */
} avalon_nonce_out;

void avalon_build_work(const avalon_job_in *in, struct toast_work *w);
void avalon_decode_nonce(const struct toast_nonce *n, avalon_nonce_out *out);
int  avalon_send_work(uint8_t channel, struct toast_work *w);
int  avalon_set_target(uint8_t channel, uint32_t target_h, uint32_t target_l);
int  avalon_collect_nonces(uint8_t channel, avalon_nonce_out *out, uint8_t max);

#endif /* DRIVER_AVALON_H */
