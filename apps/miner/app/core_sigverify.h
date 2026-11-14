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

#ifndef CORE_SIGVERIFY_H
#define CORE_SIGVERIFY_H

#include <stdint.h>

#define CORE_SIG_LEN      128 /* RSA-1024 signature length, bytes */
#define CORE_SIG_HASH_LEN 32  /* SHA-256 digest length, bytes */

extern const char core_pic_public_pem[]; /* RSA-1024 public key, PEM */

int core_sig_verify(const char *what, const char *pem, int pss, const uint8_t sig[CORE_SIG_LEN],
        const uint8_t hash[CORE_SIG_HASH_LEN]);

#endif /* CORE_SIGVERIFY_H */
