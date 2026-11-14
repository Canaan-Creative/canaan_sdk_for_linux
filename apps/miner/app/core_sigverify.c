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

#include "core_sigverify.h"

#include <string.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#ifdef PIC_PUBKEY_HEADER
#include PIC_PUBKEY_HEADER
#else
/* RSA-1024 public key, PEM */
const char core_pic_public_pem[] =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDevMGpYnx6dswRHQvsdJi/rtzF\n"
        "pNwJiI4i8SLj3iM3s2mQMG5v9yTSXUyNHUJ/H/aSntBpUIXpaQ642UKs6Pu5C6nr\n"
        "WjxtnlsytfnxD/G73+Fx7iePq9FOFC/Q7fpdhja5alkqHCs6n6W6EVxAwXLQB9Wh\n"
        "0WKGGCjfW1ALr2kY3QIDAQAB\n"
        "-----END PUBLIC KEY-----\n";
#endif

int core_sig_verify(const char *what, const char *pem, int pss, const uint8_t sig[CORE_SIG_LEN],
        const uint8_t hash[CORE_SIG_HASH_LEN])
{
    (void)what;
    BIO          *bio = BIO_new_mem_buf(pem, (int)strlen(pem));
    EVP_PKEY     *pk  = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    EVP_PKEY_CTX *ctx = NULL;
    int           ok  = 0;
    if (pk && EVP_PKEY_get_base_id(pk) == EVP_PKEY_RSA &&
            (ctx = EVP_PKEY_CTX_new(pk, NULL)) != NULL && EVP_PKEY_verify_init(ctx) > 0 &&
            EVP_PKEY_CTX_set_rsa_padding(ctx, pss ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING) >
                    0 &&
            EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) > 0 &&
            (!pss ||
                    (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) > 0 &&
                            EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, RSA_PSS_SALTLEN_AUTO) > 0))) {
        ok = (EVP_PKEY_verify(ctx, sig, CORE_SIG_LEN, hash, CORE_SIG_HASH_LEN) == 1);
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pk);
    BIO_free(bio);
    return ok;
}
