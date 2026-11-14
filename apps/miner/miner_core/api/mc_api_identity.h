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

#ifndef MC_API_IDENTITY_H
#define MC_API_IDENTITY_H

#include "mc_api_types.h"

typedef struct {
    char mac[18];
    char dna[24]; /* chip DNA, hex */
    char ver[64];
} mc_api_id_buf;

int  mc_api_identity_fill(mc_api_stats *s, mc_api_id_buf *store);
void mc_api_identity_dna(const uint8_t dna[MM_DNA_LEN], char out[MM_DNA_LEN * 2 + 1]);

#endif /* MC_API_IDENTITY_H */
