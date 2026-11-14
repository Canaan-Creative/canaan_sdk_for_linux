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

#ifndef CORE_API_H
#define CORE_API_H

#include <stddef.h>

/* Per-call bound on connect, send and each read. */
#define CORE_API_TIMEOUT_MS 5000
#define CORE_API_TRUNCATED  (-2) /* the reply did not fit the buffer */

int core_api_call(const char *cmd, const char *param, char *out, size_t cap);

#endif /* CORE_API_H */
