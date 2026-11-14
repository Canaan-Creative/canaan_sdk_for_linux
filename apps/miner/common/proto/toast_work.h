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

#ifndef TOAST_WORK_H
#define TOAST_WORK_H

#include <stdint.h>
#include <stddef.h>

#define TOAST_WORK_VERSION_WORDS 8  /* rolled block versions, count    */
#define TOAST_WORK_TAIL_WORDS    3  /* merkle[7], ntime, nbits         */
#define TOAST_WORK_HDR_REV_WORDS 15 /* header words 15..1, reversed    */
#define TOAST_WORK_HASH_WORDS    8  /* 256-bit merkle root / prev hash */
#define TOAST_WORK_NOTE_WORDS    4  /* 128-bit user note               */

/* a3197s work register image, 124 bytes */
struct toast_work {
    uint32_t version[TOAST_WORK_VERSION_WORDS]; /* block versions, byte-swapped */
    uint32_t nonce;
    uint32_t tail[TOAST_WORK_TAIL_WORDS];       /* merkle[7], ntime, nbits, byte-swapped */
    uint32_t hdr_rev[TOAST_WORK_HDR_REV_WORDS]; /* header words 15..1, byte-swapped */
    uint32_t user_note[TOAST_WORK_NOTE_WORDS];  /* user note returned with each nonce */
};

_Static_assert(offsetof(struct toast_work, version) == 0x00, "version @0x00");
_Static_assert(offsetof(struct toast_work, nonce) == 0x20, "nonce @0x20");
_Static_assert(offsetof(struct toast_work, tail) == 0x24, "tail @0x24");
_Static_assert(offsetof(struct toast_work, hdr_rev) == 0x30, "hdr_rev @0x30");
_Static_assert(offsetof(struct toast_work, user_note) == 0x6c, "note @0x6c");
_Static_assert(sizeof(struct toast_work) == 0x7c, "work image is 124 bytes");

#endif /* TOAST_WORK_H */
