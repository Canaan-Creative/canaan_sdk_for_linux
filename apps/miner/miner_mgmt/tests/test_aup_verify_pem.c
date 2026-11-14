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

#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#if !defined(AUP_TEST_KEY) || !defined(AUP_DEV_KEY)
#error "AUP_TEST_KEY and AUP_DEV_KEY must name the private keys this test signs with"
#endif

#define CHECK(c, ...) \
    do { \
        if (c) { \
            printf("  ok: "); \
        } else { \
            fails++; \
            printf("  FAIL: "); \
        } \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } while (0)

static int fails;
static int skips;

static EVP_PKEY *load_key(const char *path)
{
    FILE     *f = fopen(path, "rb");
    EVP_PKEY *k;

    if (f == NULL)
        return NULL;
    k = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return k;
}

static int sign_header(struct aup_hdr *h, EVP_PKEY *key)
{
    uint8_t       hash[SHA256_DIGEST_SIZE];
    EVP_PKEY_CTX *ctx  = EVP_PKEY_CTX_new(key, NULL);
    size_t        slen = AUP_SIG_LEN;
    int           ok;

    sha256(h, AUP_SIGNED_LEN, hash);
    ok = ctx != NULL && EVP_PKEY_sign_init(ctx) > 0 &&
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) > 0 &&
            EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) > 0 &&
            EVP_PKEY_sign(ctx, h->signature, &slen, hash, sizeof hash) > 0 && slen == AUP_SIG_LEN;
    EVP_PKEY_CTX_free(ctx);
    return ok ? 0 : -1;
}

int main(void)
{
    struct aup_hdr h;
    EVP_PKEY      *test = load_key(AUP_TEST_KEY);
    EVP_PKEY      *dev  = load_key(AUP_DEV_KEY);

    printf("test_aup_verify_pem: aup_verify.c built with AUP_VERIFY_PUBKEY_PEM\n");
    CHECK(test != NULL, "the test private key loads from %s", AUP_TEST_KEY);
    if (test == NULL)
        return 1;

    memset(&h, 0, sizeof h);
    memset(&h, 0x5a, AUP_SIGNED_LEN);
    CHECK(sign_header(&h, test) == 0, "the test key signs the header");
    CHECK(aup_verify_signature(&h) == 0,
            "a header signed by the private half of the configured PEM verifies");
    ((unsigned char *)&h)[0] ^= 0xffu;
    CHECK(aup_verify_signature(&h) != 0, "flipping a byte in the signed range breaks verification");
    ((unsigned char *)&h)[0] ^= 0xffu;

    if (dev != NULL) {
        CHECK(sign_header(&h, dev) == 0, "the development key signs the header");
        CHECK(aup_verify_signature(&h) != 0,
                "a header signed by the development key is rejected: the configured "
                "PEM replaces the built-in key");
    } else {
        printf("  SKIP: no development key at %s\n", AUP_DEV_KEY);
        skips++;
    }

    EVP_PKEY_free(test);
    EVP_PKEY_free(dev);
    printf(fails ? "test_aup_verify_pem: FAILED (%d)\n" : "test_aup_verify_pem: all pass\n", fails);
    return fails ? 1 : skips ? TST_SKIP : 0;
}
