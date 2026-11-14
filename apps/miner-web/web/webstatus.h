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

#ifndef WEBSTATUS_H
#define WEBSTATUS_H

#include <stddef.h>

#define WEB_STATUS_BAD_CMD_CODE       14
#define WEB_STATUS_BAD_CMD_MSG        "Invalid command"
#define WEB_STATUS_BAD_CMD_NAME       32
#define WEB_STATUS_OK_CODE            200
#define WEB_STATUS_FAULT_CODE         500
#define WEB_STATUS_TOO_LONG_MSG       "Reply too long: not sent"
#define WEB_STATUS_NO_CORE_MSG        "Command timeout"
#define WEB_STATUS_INTERNAL_MSG       "Internal error"
#define WEB_STATUS_BAD_REQ_CODE       501
#define WEB_STATUS_INVALID_MSG        "Invalid request"
#define WEB_STATUS_JSON_MSG           "JSON request body required"
#define WEB_STATUS_HTTPS_CODE         503
#define WEB_STATUS_HTTPS_MSG          "HTTPS required"
#define WEB_STATUS_REFUSED_CODE       504
#define WEB_STATUS_REFUSED_MSG        "Request refused"
#define WEB_STATUS_METHOD_CODE        505
#define WEB_STATUS_METHOD_MSG         "Method not allowed"
#define WEB_STATUS_LOCKED_CODE        506
#define WEB_STATUS_LOCKED_MSG         "Too many failed attempts, try again later"
#define WEB_STATUS_TOKEN_OK_CODE      610
#define WEB_STATUS_TOKEN_REQ_CODE     601
#define WEB_STATUS_AUTH_CODE          602
#define WEB_STATUS_TOKEN_UNSET_CODE   603
#define WEB_STATUS_TOKEN_UNAVAIL_CODE 604
#define WEB_STATUS_TOKEN_STALE_MSG    "Token expired, request a new one"
#define WEB_STATUS_NO_LOGIN_MSG       "Not logged in"

size_t web_status_json(const char *st, int code, const char *msg, char *out, size_t cap);
size_t web_status_bad_cmd(const char *name, char *out, size_t cap);

#endif /* WEBSTATUS_H */
