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

#ifndef OTA_PROTO_H
#define OTA_PROTO_H

#include <stdint.h>

#define MM_OTA_SOCK_PATH    "/tmp/sockupctrl"
#define MM_OTA_MAGIC        0xdeadbeaf
#define MM_OTA_MSG_TYPE     1
#define MM_OTA_PEER_STALL_S 60 /* upload socket stall timeout, s */

typedef union {
    uint32_t data;
    struct {
        int8_t stat;    /* always 0; sent once when the upload ends */
        int8_t percent; /* image received, percent */
        int8_t error;   /* upgrade result code, UPGRADE_* */
        int8_t set;     /* reserved, 0 */
    };
} mm_ota_data;

/* OTA status message, 12 bytes */
typedef struct {
    int         magic;   /* MM_OTA_MAGIC */
    int         type;    /* MM_OTA_MSG_TYPE */
    mm_ota_data upgrade; /* upgrade progress and result, 4 bytes */
} mm_ota_msg;

_Static_assert(sizeof(mm_ota_msg) == 12, "mm_ota_msg is 12 bytes");

#endif /* OTA_PROTO_H */
