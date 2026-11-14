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

#ifndef HEX_H
#define HEX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int hex_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static inline int hex_valid(const char *src, size_t nchars)
{
    for (size_t i = 0; i < nchars; i++)
        if (hex_nibble((unsigned char)src[i]) < 0)
            return 0;
    return 1;
}

static inline int hex_decode(uint8_t *dst, const char *src, size_t nbytes)
{
    for (size_t i = 0; i < nbytes; i++) {
        int hi = hex_nibble((unsigned char)src[2 * i]);
        if (hi < 0)
            return -1;
        int lo = hex_nibble((unsigned char)src[2 * i + 1]);
        if (lo < 0)
            return -1;
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static inline int hex_decode_str(uint8_t *dst, size_t cap, const char *src)
{
    if (!src)
        return 0;
    size_t len = strlen(src);
    if (len % 2 || len / 2 > cap || !hex_valid(src, len))
        return -1;
    hex_decode(dst, src, len / 2);
    return (int)(len / 2);
}

static inline void hex_encode(char *dst, const void *src, size_t nbytes)
{
    const uint8_t *p = src;

    for (size_t i = 0; i < nbytes; i++) {
        dst[2 * i]     = "0123456789abcdef"[p[i] >> 4];
        dst[2 * i + 1] = "0123456789abcdef"[p[i] & 0x0f];
    }
    dst[2 * nbytes] = '\0';
}

static inline void hex_encode_upper(char *dst, const void *src, size_t nbytes)
{
    const uint8_t *p = src;

    for (size_t i = 0; i < nbytes; i++) {
        dst[2 * i]     = "0123456789ABCDEF"[p[i] >> 4];
        dst[2 * i + 1] = "0123456789ABCDEF"[p[i] & 0x0f];
    }
    dst[2 * nbytes] = '\0';
}

#endif /* HEX_H */
