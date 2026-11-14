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

#ifndef TST_RSA_SIGN_H
#define TST_RSA_SIGN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#define TST_RSA_SIG_LEN 128

static inline int tst_rsa_sign(const char *keyfile, int pss, const void *msg, size_t n,
        uint8_t sig[TST_RSA_SIG_LEN])
{
    FILE         *f   = fopen(keyfile, "rb");
    EVP_PKEY     *pk  = f ? PEM_read_PrivateKey(f, NULL, NULL, NULL) : NULL;
    EVP_MD_CTX   *md  = EVP_MD_CTX_new();
    EVP_PKEY_CTX *ctx = NULL;
    size_t        len = TST_RSA_SIG_LEN;
    int           ok  = pk && md && EVP_DigestSignInit(md, &ctx, EVP_sha256(), NULL, pk) > 0 &&
            (!pss ||
                    (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PSS_PADDING) > 0 &&
                            EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) > 0 &&
                            EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, RSA_PSS_SALTLEN_DIGEST) > 0)) &&
            EVP_DigestSign(md, sig, &len, msg, n) > 0 && len == TST_RSA_SIG_LEN;

    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pk);
    if (f)
        fclose(f);
    return ok ? 0 : -1;
}

#endif /* TST_RSA_SIGN_H */
