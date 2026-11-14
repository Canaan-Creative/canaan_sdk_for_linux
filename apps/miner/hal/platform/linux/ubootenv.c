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

#include "ubootenv.h"
#include "osif.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <libuboot.h>

#define UBOOTENV_CONFIG "/etc/fw_env.config"
/* read fallback on any open failure; write fallback only on -ENODATA */
#define UBOOTENV_DEFAULTS "/etc/u-boot-initial-env"

static int env_ctx_new(struct uboot_ctx **out)
{
    struct uboot_ctx *ctx = NULL;

    *out = NULL;
    if (libuboot_initialize(&ctx, NULL) < 0 || ctx == NULL) {
        osif_log(OSIF_LOG_ERR, "ubootenv: libubootenv initialisation failed");
        return -ENODEV;
    }
    if (libuboot_read_config(ctx, UBOOTENV_CONFIG) < 0) {
        osif_log(OSIF_LOG_ERR, "ubootenv: cannot read %s", UBOOTENV_CONFIG);
        libuboot_exit(ctx);
        return -EINVAL;
    }
    *out = ctx;
    return 0;
}

static void env_ctx_free(struct uboot_ctx *ctx)
{
    libuboot_close(ctx);
    libuboot_exit(ctx);
}

static int env_acquire_readable(struct uboot_ctx *ctx)
{
    if (libuboot_open(ctx) == 0)
        return 0;
    return libuboot_load_file(ctx, UBOOTENV_DEFAULTS);
}

static int env_acquire_writable(struct uboot_ctx *ctx)
{
    int rc = libuboot_open(ctx);

    if (rc == 0)
        return 0;
    if (rc != -ENODATA) {
        osif_log(OSIF_LOG_ERR,
                "ubootenv: stored environment unreadable (%d), "
                "refusing to replace it",
                rc);
        return -EIO;
    }
    rc = libuboot_load_file(ctx, UBOOTENV_DEFAULTS);
    if (rc < 0)
        osif_log(OSIF_LOG_ERR,
                "ubootenv: stored environment blank and %s "
                "unavailable (%d)",
                UBOOTENV_DEFAULTS, rc);
    return rc;
}

char *ubootenv_get(const char *name)
{
    struct uboot_ctx *ctx;
    char             *val = NULL;

    if (name == NULL || env_ctx_new(&ctx) < 0)
        return NULL;
    if (env_acquire_readable(ctx) == 0)
        val = libuboot_get_env(ctx, name);
    env_ctx_free(ctx);
    return val;
}

int ubootenv_apply_list(const char *filename)
{
    struct uboot_ctx *ctx;
    int               rc;

    if (filename == NULL)
        return -EINVAL;
    rc = env_ctx_new(&ctx);
    if (rc < 0)
        return rc;
    rc = env_acquire_writable(ctx);
    if (rc == 0) {
        rc = libuboot_load_file(ctx, filename);
        if (rc < 0)
            osif_log(OSIF_LOG_ERR, "ubootenv: cannot load the variable list (%d)", rc);
    }
    if (rc == 0) {
        rc = libuboot_env_store(ctx);
        if (rc < 0)
            osif_log(OSIF_LOG_ERR, "ubootenv: storing the environment failed (%d)", rc);
    }
    env_ctx_free(ctx);
    return rc;
}
