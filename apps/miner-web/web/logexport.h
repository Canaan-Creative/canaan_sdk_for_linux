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

#ifndef LOGEXPORT_H
#define LOGEXPORT_H

#include <stddef.h>

#include "mc_sizes.h"

#define LOGEXPORT_NAME_MAX 64 /* posted log name buffer, bytes including NUL */

int  logexport_body_name(const char *body, char *name, size_t cap);
void logexport_safe_name(const char *in, char *out, size_t cap);
int  logexport_lookup(const char *name, char *path, size_t cap);
int  logexport_serve_check(const char *name, const char *path, int concat);
void logexport_pass_digest(const char *plaintext, char out_hex[WEBPASS_HEX_LEN + 1]);

#endif /* LOGEXPORT_H */
