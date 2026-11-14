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

#include "ota_state.h"

#include "sha256.h"

/* Streaming state of one package upload */
struct ota_state {
    ota_sink       sink;              /* package destination */
    struct aup_hdr hdr;               /* parsed header */
    sha256_ctx     bin_sha;           /* SHA-256 of the current bin so far */
    unsigned char  raw[AUP_HDR_SIZE]; /* header block as received */
    int32_t        hdr_len;           /* header bytes received */
    int32_t        nbins;
    int32_t        bin_idx;
    int32_t        bin_recv; /* current bin bytes received */
    int8_t         percent;  /* progress, percent */
    uint8_t        parsed;   /* 1 once the header passed every gate */
    uint8_t        done;     /* 1 once the upload succeeded or failed */
};

size_t ota_state_size(void)
{
    return sizeof(struct ota_state);
}

void ota_reset(ota_state_t *st, const ota_sink *sink)
{
    if (st == NULL)
        return;
    memset(st, 0, sizeof *st);
    if (sink != NULL)
        st->sink = *sink;
}

static int ota_fail(struct ota_state *st, int err, int8_t *percent)
{
    st->hdr_len  = 0;
    st->parsed   = 0;
    st->bin_idx  = 0;
    st->bin_recv = 0;
    st->percent  = 0;
    st->done     = 1;
    if (percent != NULL)
        *percent = 0;
    return err;
}

int ota_feed(ota_state_t *st, const void *buf, int len, int8_t *percent)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t               data_len;
    int                  rc;

    if (st == NULL || len < 0 || (buf == NULL && len > 0))
        return UPGRADE_IMAGE_ERR;
    if (st->done)
        return UPGRADE_ALREADY_ERR;

    data_len = (size_t)len;

    if (st->hdr_len < AUP_HDR_SIZE) {
        size_t want = (size_t)(AUP_HDR_SIZE - st->hdr_len);
        size_t n    = (data_len < want) ? data_len : want;

        memcpy(st->raw + st->hdr_len, p, n);
        st->hdr_len += (int32_t)n;
        p += n;
        data_len -= n;
        if (st->hdr_len < AUP_HDR_SIZE) {
            if (percent != NULL)
                *percent = st->percent;
            return UPGRADE_PROCESS_OK;
        }
    }

    if (!st->parsed) {
        st->percent = 0;
        rc          = aup_hdr_parse(st->raw, AUP_HDR_SIZE, &st->hdr);
        if (rc < 0)
            return ota_fail(st, rc, percent);
        if (st->sink.check_hdr != NULL) {
            rc = st->sink.check_hdr(st->sink.ctx, &st->hdr);
            if (rc < 0)
                return ota_fail(st, rc, percent);
        }
        rc = aup_verify_signature(&st->hdr);
        if (rc < 0)
            return ota_fail(st, rc, percent);
        if (st->sink.check_part != NULL) {
            rc = st->sink.check_part(st->sink.ctx, &st->hdr);
            if (rc < 0)
                return ota_fail(st, rc, percent);
        }
        st->nbins  = aup_be32(st->hdr.totalbins);
        st->parsed = 1;
    }

    while (st->bin_idx < st->nbins) {
        const struct aup_bin *b       = &st->hdr.f[st->bin_idx];
        int32_t               bin_len = aup_be32(b->size);
        uint8_t               digest[SHA256_DIGEST_SIZE];
        size_t                want;
        size_t                n;

        if (data_len == 0)
            break;

        if (st->bin_recv == 0) {
            sha256_init(&st->bin_sha);
            if (st->sink.begin != NULL) {
                rc = st->sink.begin(st->sink.ctx, b);
                if (rc < 0)
                    return ota_fail(st, UPGRADE_IMAGE_ERR, percent);
            }
        }

        want = (size_t)(bin_len - st->bin_recv);
        n    = (data_len < want) ? data_len : want;
        if (n > 0) {
            if (st->sink.write != NULL) {
                rc = st->sink.write(st->sink.ctx, b, (long long)st->bin_recv, p, n);
                if (rc < 0)
                    return ota_fail(st, UPGRADE_IMAGE_ERR, percent);
            }
            sha256_update(&st->bin_sha, p, n);
            st->bin_recv += (int32_t)n;
            p += n;
            data_len -= n;
        }

        if (st->bin_recv < bin_len)
            break;

        sha256_final(&st->bin_sha, digest);
        rc = aup_verify_bin_sha256(b, digest);
        if (rc < 0)
            return ota_fail(st, rc, percent);

        if (st->sink.end != NULL) {
            rc = st->sink.end(st->sink.ctx, b);
            if (rc < 0)
                return ota_fail(st, UPGRADE_IMAGE_ERR, percent);
        }

        st->percent = (int8_t)(st->percent + 100 / st->nbins);
        st->bin_idx++;
        st->bin_recv = 0;
    }

    if (st->bin_idx == st->nbins) {
        st->percent = 100;
        st->done    = 1;
        if (percent != NULL)
            *percent = st->percent;
        return UPGRADE_SUCCEED;
    }

    if (percent != NULL)
        *percent = st->percent;
    return UPGRADE_PROCESS_OK;
}
