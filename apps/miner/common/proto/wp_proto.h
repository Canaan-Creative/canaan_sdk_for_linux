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

#ifndef WP_PROTO_H
#define WP_PROTO_H

#include <stdint.h>

#define WP_PKG_LEN     2104 /* max wallpaper chunk length, hex chars */
#define WP_MAGIC       "nano"
#define WP_VER         1
#define WP_SIZE_MIN    1024   /* min wallpaper file size, bytes */
#define WP_SIZE_MAX    307200 /* max wallpaper file size, bytes */
#define WP_SHA_LEN     32     /* wallpaper SHA-256 digest length, bytes */
#define WP_SIGN_LEN    128    /* wallpaper RSA signature length, bytes */
#define WP_HDR_LEN     ((int)sizeof(struct wp_header))
#define WP_PAYLOAD_MAX (WP_PKG_LEN / 2 - WP_HDR_LEN)

struct __attribute__((packed)) wp_header {
    uint8_t  magic[8];   /* WP_MAGIC */
    uint32_t format_ver; /* WP_VER */
    uint8_t  header_len; /* header length, bytes */
    uint8_t  reserved1[1];
    uint16_t payload_len; /* chunk payload length, bytes */
    uint16_t uid;
    uint16_t idx;
    uint32_t file_size; /* whole wallpaper file size, bytes */
    uint32_t offset;    /* payload offset in the file, bytes */
};

_Static_assert(sizeof(struct wp_header) == 28, "wp_header is 28 bytes");

#endif /* WP_PROTO_H */
