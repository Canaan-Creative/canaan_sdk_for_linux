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

#ifndef SHA256_REF_H
#define SHA256_REF_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32 /* SHA-256 digest size, bytes */

void sha256_ref(const uint8_t *msg, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

void sha256d_ref(const uint8_t *msg, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

#endif /* SHA256_REF_H */
