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

#ifndef AUP_H
#define AUP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "osif.h"

#define UPGRADE_PROCESS_OK    0      /* package accepted so far, more data expected */
#define UPGRADE_SUCCEED       1      /* every bin written and verified */
#define UPGRADE_HEAER_ERR     -1     /* malformed header or bin entry */
#define UPGRADE_SW_ERR        -2     /* package SW does not match the board */
#define UPGRADE_HW_ERR        -3     /* package HW does not match the board */
#define UPGRADE_VERIFY_ERR    -4     /* header signature rejected */
#define UPGRADE_PARTITION_ERR -5     /* partition check or env write failed */
#define UPGRADE_CHECK_SHA_ERR -6     /* bin SHA-256 mismatch */
#define UPGRADE_ALREADY_ERR   -7     /* upgrade already finished */
#define UPGRADE_IMAGE_ERR     -8     /* bad input or bin write failure */
#define AUP_HDR_SIZE          2048   /* header block, bytes */
#define AUP_MAX_BINS          14     /* bin entries in a header */
#define AUP_SIG_LEN           128    /* header signature, bytes */
#define AUP_SHA_LEN           32     /* bin SHA-256 digest, bytes */
#define AUP_FORMAT            0x10   /* header format version */
#define AUP_MAINCTROLER       "k230" /* required mainctroler field */
#define AUP_UPGRADE_TYPE      "OTA"  /* required upgrade_type field */
#define AUP_SIGNED_LEN        (sizeof(struct aup_hdr) - AUP_SIG_LEN) /* signed prefix, bytes */
#define AUP_WARN(...)         osif_log(OSIF_LOG_WARN, "aup: " __VA_ARGS__)

/* Bin entry of an AUP header; integers big-endian */
struct aup_bin {
    char    name[16];            /* bin name, MTD label stem */
    int32_t ofst;                /* bin data offset in the package, bytes */
    int32_t size;                /* bin data length, bytes */
    int32_t partofst;            /* slot A flash offset, bytes */
    int32_t partsize;            /* partition size of each slot, bytes */
    int32_t partofst2;           /* slot B flash offset, bytes */
    int32_t partsize2;           /* slot B partition size, bytes; not read on the device */
    uint8_t sha256[AUP_SHA_LEN]; /* SHA-256 of the bin data */
} __attribute__((packed, aligned(1)));

/* AUP package header; integers big-endian */
struct aup_hdr {
    int32_t        format;          /* header format version */
    char           manufacturer[8]; /* manufacturer name, NUL-padded */
    char           product[16];     /* product name, NUL-padded */
    char           mainctroler[16];
    char           time[20];  /* build time, "YYYY-MM-DD HH:MM:SS" */
    int32_t        totalbins; /* bin entries in use */
    int32_t        totalsize; /* package length, bytes */
    char           version[64];
    char           hw[16];                 /* hardware identifier */
    char           sw[16];                 /* software identifier */
    char           upgrade_type[16];       /* package kind, AUP_UPGRADE_TYPE */
    struct aup_bin f[AUP_MAX_BINS];        /* bin entries */
    uint8_t        signature[AUP_SIG_LEN]; /* RSA signature over the first AUP_SIGNED_LEN bytes */
} __attribute__((packed, aligned(1)));

_Static_assert(sizeof(struct aup_bin) == 72, "aup_bin wire size is 72 bytes");
_Static_assert(sizeof(struct aup_hdr) == 1320, "aup_hdr wire size is 1320 bytes");
_Static_assert(AUP_SIGNED_LEN == 1192, "aup_hdr signed range is 1192 bytes");
_Static_assert(sizeof(struct aup_hdr) <= AUP_HDR_SIZE,
        "aup_hdr must fit the 2048-byte header block");

static inline int32_t aup_be32(int32_t v)
{
    unsigned char b[4];

    memcpy(b, &v, sizeof b);
    return (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) |
            (uint32_t)b[3]);
}

int aup_hdr_parse(const void *buf, size_t len, struct aup_hdr *out);
int aup_verify_signature(const struct aup_hdr *h);
int aup_verify_bin_sha256(const struct aup_bin *b, const uint8_t digest[AUP_SHA_LEN]);

#endif /* AUP_H */
