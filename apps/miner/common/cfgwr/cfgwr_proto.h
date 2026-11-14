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

#ifndef CFGWR_PROTO_H
#define CFGWR_PROTO_H

#include <stdint.h>

#include "mc_paths.h"
#include "mc_sizes.h"
#include "pool.h"

#define MM_CFGWR_SOCK_PATH    "/tmp/sockcfgwr"
#define MM_CFGWR_MAGIC        0xc0f19420u
#define MM_CFGWR_VERSION      4u
#define MM_CFGWR_IO_TIMEOUT_S 5 /* socket send and receive timeout, both ends, s */
#define MM_CFGWR_WP_STAGE_DIR "/tmp/wallpaper"
/* staged wallpaper path: directory, upload ID, image format */
#define MM_CFGWR_WP_STAGE_FMT "%s/wallpaper_%u.%s.tmp"
/* published wallpaper path: directory, upload ID, image format */
#define MM_CFGWR_WP_PUB_FMT "%s/wallpaper_%u.%s"
#define MM_CFGWR_WP_FMT_MAX 8 /* wallpaper image format buffer, bytes, including NUL */

enum {
    MM_CFGWR_POOLS           = 1,
    MM_CFGWR_WEBPASS         = 2, /* web password SHA-256 */
    MM_CFGWR_UPDATE_POOLS    = 3, /* pool set read-modify-write request */
    MM_CFGWR_HWCFG           = 4, /* product and model IDs */
    MM_CFGWR_ACTIVATION      = 5, /* device activation, sets the activated flag to 1 */
    MM_CFGWR_WALLPAPER       = 6, /* publish a staged wallpaper */
    MM_CFGWR_WALLPAPER_CLEAR = 7,
    MM_CFGWR_READ_POOLS      = 8,
};

typedef struct {
    uint32_t uid;
    char     format[MM_CFGWR_WP_FMT_MAX]; /* image format, "png" or "gif" */
} mm_cfgwr_wp;

typedef struct {
    char prod[PROD_NAME_CAP];
    char model[MODEL_NAME_CAP];
} mm_cfgwr_hwcfg;

typedef struct {
    uint32_t magic;     /* MM_CFGWR_MAGIC */
    uint32_t version;   /* MM_CFGWR_VERSION */
    uint32_t kind;      /* request kind, MM_CFGWR_* */
    char     path[512]; /* target config file path, or empty; always empty for wallpaper kinds */
    union {             /* payload selected by kind */
        pool_set pools; /* pool set, MM_CFGWR_POOLS */
        char     webpass[WEBPASS_HEX_LEN + 1]; /* web password SHA-256, hex, MM_CFGWR_WEBPASS */
        mm_cfgwr_hwcfg hwcfg;                  /* MM_CFGWR_HWCFG */
        mm_cfgwr_wp    wallpaper;              /* MM_CFGWR_WALLPAPER */
    } u;
} mm_cfgwr_req;

typedef struct {
    uint32_t magic;  /* MM_CFGWR_MAGIC */
    int32_t  status; /* 0 = persisted or read, <0 = failed or rejected */
} mm_cfgwr_rep;

_Static_assert(sizeof(mm_cfgwr_req) == 2128, "mm_cfgwr_req is 2128 bytes");
_Static_assert(sizeof(mm_cfgwr_rep) == 8, "mm_cfgwr_rep is 8 bytes");

#endif /* CFGWR_PROTO_H */
