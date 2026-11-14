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

#include "mc_api_identity.h"
#include "hex.h"
#include "boardconf.h"
#include "net_hwaddr.h"
#include <string.h>
#include <stdio.h>

void mc_api_identity_dna(const uint8_t dna[MM_DNA_LEN], char out[MM_DNA_LEN * 2 + 1])
{
    hex_encode(out, dna, MM_DNA_LEN);
}

int mc_api_identity_fill(mc_api_stats *s, mc_api_id_buf *store)
{
    s->prod   = PROD_NAME;
    s->model  = MODEL;
    s->hwtype = HW_TYPE;
    s->swtype = SW_TYPE;
    snprintf(store->ver, sizeof store->ver, "%s", CORE_MINER_VERSION);
    s->version = store->ver;
    mc_api_identity_mac(store->mac);
    s->mac = store->mac;
    strcpy(store->dna, "0000000000000000");
    s->dna = store->dna;
    return 0;
}
