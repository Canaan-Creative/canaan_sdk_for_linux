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

#include "aup.h"

static int aup_field_eq(const char *field, size_t n, const char *want)
{
    size_t l = strlen(want);

    if (l >= n)
        return 0;
    return memcmp(field, want, l) == 0 && field[l] == '\0';
}

int aup_verify_bin_sha256(const struct aup_bin *b, const uint8_t digest[AUP_SHA_LEN])
{
    if (b == NULL || digest == NULL)
        return UPGRADE_CHECK_SHA_ERR;

    if (memcmp(digest, b->sha256, AUP_SHA_LEN) != 0) {
        AUP_WARN("bin '%.16s' sha256 mismatch", b->name);
        return UPGRADE_CHECK_SHA_ERR;
    }
    return 0;
}

int aup_hdr_parse(const void *buf, size_t len, struct aup_hdr *out)
{
    struct aup_hdr h;
    int32_t        nbins;
    int32_t        i;

    if (buf == NULL || out == NULL || len < AUP_HDR_SIZE)
        return UPGRADE_HEAER_ERR;

    memcpy(&h, buf, sizeof h);

    if (aup_be32(h.format) != AUP_FORMAT)
        return UPGRADE_HEAER_ERR;
    if (!aup_field_eq(h.mainctroler, sizeof h.mainctroler, AUP_MAINCTROLER))
        return UPGRADE_HEAER_ERR;
    if (!aup_field_eq(h.upgrade_type, sizeof h.upgrade_type, AUP_UPGRADE_TYPE))
        return UPGRADE_HEAER_ERR;

    nbins = aup_be32(h.totalbins);
    if (nbins <= 0 || nbins > AUP_MAX_BINS)
        return UPGRADE_HEAER_ERR;

    for (i = 0; i < nbins; i++) {
        if (aup_be32(h.f[i].size) <= 0)
            return UPGRADE_HEAER_ERR;
    }

    *out = h;
    return 0;
}
