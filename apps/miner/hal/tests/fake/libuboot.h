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

#ifndef LIBUBOOT_H
#define LIBUBOOT_H

struct uboot_env_device;
struct uboot_ctx;

int   libuboot_initialize(struct uboot_ctx **out, struct uboot_env_device *envdevs);
void  libuboot_exit(struct uboot_ctx *ctx);
int   libuboot_read_config(struct uboot_ctx *ctx, const char *config);
int   libuboot_open(struct uboot_ctx *ctx);
void  libuboot_close(struct uboot_ctx *ctx);
int   libuboot_load_file(struct uboot_ctx *ctx, const char *filename);
int   libuboot_env_store(struct uboot_ctx *ctx);
int   libuboot_set_env(struct uboot_ctx *ctx, const char *varname, const char *value);
char *libuboot_get_env(struct uboot_ctx *ctx, const char *varname);

#endif /* LIBUBOOT_H */
