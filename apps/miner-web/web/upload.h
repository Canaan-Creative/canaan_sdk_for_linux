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

#ifndef UPLOAD_H
#define UPLOAD_H

#include <stddef.h>
#include <time.h>

#include "ota_proto.h"

#define UPLOAD_WRITE_STALL_S MM_OTA_PEER_STALL_S /* OTA socket send stall limit, s */
#define UPLOAD_STATUS_WAIT_S MM_OTA_PEER_STALL_S /* OTA status message wait limit, s */
#define UPLOAD_BODY_STALL_S  MM_OTA_PEER_STALL_S /* request body stall limit, s */
#define WEB_WALLPAPER_URI    "/api/v1/wallpaper"
/* reply Msg when an upload is in progress */
#define WEB_UPLOAD_BUSY_MSG "Another firmware upgrade is in progress"
/* reply Msg when the OTA socket is unreachable */
#define WEB_UPLOAD_NOSOCK_MSG "Device not ready for an upgrade, try again shortly"

/* Connection to the OTA socket */
typedef struct {
    int                fd;   /* OTA socket, -1 when closed */
    unsigned long long sent; /* image bytes sent, bytes */
} web_upload;

/* OTA result read from the OTA socket */
typedef struct {
    int valid;   /* 1 when a status message was received */
    int percent; /* percent reported by mm_ota */
    int error;   /* upgrade result code, UPGRADE_* */
} web_upload_status;

int    web_upload_open(web_upload *u, const char *sock_path);
int    web_upload_write(web_upload *u, const void *buf, size_t len);
int    web_upload_finish(web_upload *u, web_upload_status *st);
void   web_upload_abort(web_upload *u);
size_t web_upload_reply(const web_upload_status *st, char *out, size_t cap);
size_t web_upload_timeout_reply(const web_upload_status *st, char *out, size_t cap);
int    web_upload_ct_is_multipart(const char *ct);
int    web_upload_body_stalled(time_t last_progress, time_t now);

struct mg_context;
struct mg_connection;

void web_upload_register(struct mg_context *mgc, const char *sock_path, const char *systemcfg_ini);
void web_wallpaper_register(struct mg_context *mgc, const char *systemcfg_ini);

#endif /* UPLOAD_H */
