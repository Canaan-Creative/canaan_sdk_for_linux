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

#include "aup.h"

#include "sha256.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#ifdef AUP_PUBKEY_HEADER
#include AUP_PUBKEY_HEADER
#else
/* RSA public key that verifies AUP header signatures, PEM */
static const char aup_public_pem[] =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCeX+3CpAdl77Nq88TSUC6Nttyd"
        "dStW319X+g1F2fDAH3f1PT/fmTSrAat5kRpoji9anOfuzIHD0EyInu0UOyAJmvWF"
        "FG7mXo/qdFV7Rw2wSOzXAd8nBVwDJl4NX21KcyiSOQzzjtuF+JBNFtLM5YWDtmlq"
        "i0Th3H4iGLjIa0kP9QIDAQAB\n"
        "-----END PUBLIC KEY-----\n";
#endif

int aup_verify_signature(const struct aup_hdr *h)
{
    unsigned char sig[AUP_SIG_LEN];
    uint8_t       hash[SHA256_DIGEST_SIZE];
    BIO          *bio = NULL;
    EVP_PKEY     *pk  = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    int           rc  = UPGRADE_VERIFY_ERR;

    if (h == NULL)
        return UPGRADE_VERIFY_ERR;

    memcpy(sig, h->signature, sizeof sig);
    sha256(h, AUP_SIGNED_LEN, hash);

    bio = BIO_new_mem_buf(aup_public_pem, (int)(sizeof aup_public_pem - 1));
    if (bio == NULL) {
        AUP_WARN("header signature: buffer alloc failed");
    } else if ((pk = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL)) == NULL) {
        AUP_WARN("header signature: public key parse failed");
    } else if (EVP_PKEY_get_base_id(pk) != EVP_PKEY_RSA) {
        AUP_WARN("header signature: key is not an RSA key");
    } else if ((ctx = EVP_PKEY_CTX_new(pk, NULL)) == NULL || EVP_PKEY_verify_init(ctx) <= 0 ||
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0 ||
            EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) <= 0) {
        AUP_WARN("header signature: verify setup failed");
    } else if (EVP_PKEY_verify(ctx, sig, sizeof sig, hash, sizeof hash) != 1) {
        AUP_WARN("header signature: verify failed over %u bytes", (unsigned)AUP_SIGNED_LEN);
    } else {
        rc = 0;
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pk);
    BIO_free(bio);
    return rc;
}
