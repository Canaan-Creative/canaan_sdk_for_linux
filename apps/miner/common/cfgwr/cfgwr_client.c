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

#define _DEFAULT_SOURCE 1
#include "cfgwr_client.h"
#include "cfgwr_proto.h"
#include "cfgwr_io.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int cfgwr_send(mm_cfgwr_req *req, pool_config_update_fn update, void *ctx)
{
    struct sockaddr_un servaddr;
    struct timeval     tv = { MM_CFGWR_IO_TIMEOUT_S, 0 };
    mm_cfgwr_rep       rep;
    int                fd, rc = -1;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sun_family = AF_UNIX;
    strncpy(servaddr.sun_path, MM_CFGWR_SOCK_PATH, sizeof servaddr.sun_path - 1);
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0)
        goto done;
    while (connect(fd, (struct sockaddr *)&servaddr, sizeof servaddr) != 0) {
        if (errno != EINTR)
            goto done;
    }
    if (cfgwr_write_full(fd, req, sizeof *req) != 0)
        goto done;
    if ((update || req->kind == MM_CFGWR_READ_POOLS) &&
            cfgwr_read_full(fd, &req->u.pools, sizeof req->u.pools) != 0)
        goto done;
    if (update) {
        if (update(&req->u.pools, ctx) != 0)
            goto done;
        req->kind = MM_CFGWR_POOLS;
        if (cfgwr_write_full(fd, req, sizeof *req) != 0)
            goto done;
    }
    if (cfgwr_read_full(fd, &rep, sizeof rep) == 0 && rep.magic == MM_CFGWR_MAGIC)
        rc = rep.status == 0 ? 0 : -1;

done:
    close(fd);
    return rc;
}

static void req_init(mm_cfgwr_req *req, uint32_t kind)
{
    memset(req, 0, sizeof *req);
    req->magic   = MM_CFGWR_MAGIC;
    req->version = MM_CFGWR_VERSION;
    req->kind    = kind;
}

static int set_path(mm_cfgwr_req *req, const char *path)
{
    if (!path || !*path || strlen(path) >= sizeof req->path)
        return -1;
    strcpy(req->path, path);
    return 0;
}

int cfgwr_write_webpass(const char *hex, const char *path)
{
    mm_cfgwr_req req;

    if (!hex || strlen(hex) != WEBPASS_HEX_LEN)
        return -1;
    req_init(&req, MM_CFGWR_WEBPASS);
    if (set_path(&req, path) != 0)
        return -1;
    snprintf(req.u.webpass, sizeof req.u.webpass, "%s", hex);
    return cfgwr_send(&req, NULL, NULL);
}

int cfgwr_write_hwcfg(const char *prod, const char *model, const char *path)
{
    mm_cfgwr_req req;

    if (!prod || !*prod || strlen(prod) >= PROD_NAME_CAP || !model || !*model ||
            strlen(model) >= MODEL_NAME_CAP)
        return -1;
    req_init(&req, MM_CFGWR_HWCFG);
    if (set_path(&req, path) != 0)
        return -1;
    snprintf(req.u.hwcfg.prod, sizeof req.u.hwcfg.prod, "%s", prod);
    snprintf(req.u.hwcfg.model, sizeof req.u.hwcfg.model, "%s", model);
    return cfgwr_send(&req, NULL, NULL);
}

int cfgwr_write_activation(const char *path)
{
    mm_cfgwr_req req;

    req_init(&req, MM_CFGWR_ACTIVATION);
    if (set_path(&req, path) != 0)
        return -1;
    return cfgwr_send(&req, NULL, NULL);
}

int cfgwr_write_wallpaper(unsigned uid, const char *format)
{
    mm_cfgwr_req req;

    if (!format || strlen(format) >= MM_CFGWR_WP_FMT_MAX)
        return -1;
    req_init(&req, MM_CFGWR_WALLPAPER);
    req.u.wallpaper.uid = uid;
    snprintf(req.u.wallpaper.format, sizeof req.u.wallpaper.format, "%s", format);
    return cfgwr_send(&req, NULL, NULL);
}

int cfgwr_clear_wallpaper(void)
{
    mm_cfgwr_req req;

    req_init(&req, MM_CFGWR_WALLPAPER_CLEAR);
    return cfgwr_send(&req, NULL, NULL);
}

int cfgwr_update_pools(const char *path, pool_config_update_fn update, void *ctx)
{
    mm_cfgwr_req req;

    req_init(&req, MM_CFGWR_UPDATE_POOLS);
    if (!update || set_path(&req, path) != 0)
        return -1;

    return cfgwr_send(&req, update, ctx);
}

int cfgwr_read_pools(const char *path, pool_set *out)
{
    mm_cfgwr_req req;
    int          rc = -1;

    req_init(&req, MM_CFGWR_READ_POOLS);
    if (!out || set_path(&req, path) != 0)
        return -1;
    if (cfgwr_send(&req, NULL, NULL) == 0 && req.u.pools.count >= 0 &&
            req.u.pools.count <= POOL_VALID_NUM) {
        for (int i = 0; i < req.u.pools.count; i++) {
            pool_t *p = &req.u.pools.pools[i];

            p->url[sizeof p->url - 1]   = '\0';
            p->user[sizeof p->user - 1] = '\0';
            p->pass[sizeof p->pass - 1] = '\0';
        }
        *out = req.u.pools;
        rc   = 0;
    }
    explicit_bzero(&req, sizeof req);
    return rc;
}
