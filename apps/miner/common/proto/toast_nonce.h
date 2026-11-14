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

#ifndef TOAST_NONCE_H
#define TOAST_NONCE_H

#include <stdint.h>

#define TOAST_NONCE_COUNT 8 /* nonce records per read, max count */

/* a3197s nonce result record */
struct toast_nonce {
    volatile uint32_t job_id : 32; /* job seq the work carried, user_note[1] */
    volatile uint32_t nonce2 : 32; /* extranonce2 the work carried, user_note[0] */
    volatile uint32_t nonce : 32;
    volatile uint32_t asic_id : 10; /* chip index on the chain */
    volatile uint32_t miner_id : 6; /* hash board index */
    volatile uint32_t ntime : 8;    /* ntime roll offset */
    volatile uint32_t mid_id : 4;   /* index of the VERSION0..7 word equal to version */
    volatile uint32_t valid : 4;    /* nonzero if the record holds a result */
    volatile uint32_t version : 32; /* block version hashed */
};

#endif /* TOAST_NONCE_H */
