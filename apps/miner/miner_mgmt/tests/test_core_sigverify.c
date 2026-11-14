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

#include "core_activate.h"
#include "core_sigverify.h"
#include "sha256.h"

#include <string.h>
#include <unistd.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "../../tests/include/tst_check.h"
#include "../../tests/include/tst_rsa_sign.h"

#ifndef PIC_KEY_BAD
#error "PIC_KEY_BAD must name a private key whose signatures are refused"
#endif

static const char DNA16[] = "0201000012345678";

#ifndef PIC_KEY_GOOD
static const char API_KEY_N[] = "DEBCC1A9627C7A76CC111D0BEC7498BFAEDCC5A4DC09888E22F122E3DE2337B3"
                                "6990306E6FF724D25D4C8D1D427F1FF6929ED0695085E9690EB8D942ACE8FBB9"
                                "0BA9EB5A3C6D9E5B32B5F9F10FF1BBDFE171EE278FABD14E142FD0EDFA5D8636"
                                "B96A592A1C2B3A9FA5BA115C40C172D007D5A1D162861828DF5B500BAF6918DD";

static int pem_is_api_key(void)
{
    BIO      *bio = BIO_new_mem_buf(core_pic_public_pem, -1);
    EVP_PKEY *pk  = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    BIGNUM   *n = NULL, *e = NULL, *want = NULL;
    int       ok = pk && EVP_PKEY_get_base_id(pk) == EVP_PKEY_RSA &&
            EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_N, &n) &&
            EVP_PKEY_get_bn_param(pk, OSSL_PKEY_PARAM_RSA_E, &e) && BN_hex2bn(&want, API_KEY_N) &&
            BN_cmp(n, want) == 0 && BN_is_word(e, 65537);
    BN_free(want);
    BN_free(e);
    BN_free(n);
    EVP_PKEY_free(pk);
    BIO_free(bio);
    return ok;
}
#endif

static int activation_args(const char *key, const char *dna16, char *args)
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t           md[SHA256_DIGEST_SIZE], sig[TST_RSA_SIG_LEN];

    sha256(dna16, strlen(dna16), md);
    if (tst_rsa_sign(key, 1, md, sizeof md, sig) != 0)
        return -1;
    memset(args, '0', 64);
    for (size_t i = 0; i < sizeof sig; i++) {
        args[64 + 2 * i]     = hexd[sig[i] >> 4];
        args[64 + 2 * i + 1] = hexd[sig[i] & 0xf];
    }
    args[64 + 2 * sizeof sig] = 0;
    return 0;
}

static int pkcs1_ok(const char *key)
{
    static const uint8_t dna[8] = { 0x02, 0x01, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78 };
    uint8_t              hash[CORE_SIG_HASH_LEN], sig[CORE_SIG_LEN];

    if (tst_rsa_sign(key, 0, dna, sizeof dna, sig) != 0)
        return -1;
    sha256(dna, sizeof dna, hash);
    return core_sig_verify("pkcs1", core_pic_public_pem, 0, sig, hash);
}

int main(void)
{
    char args[64 + 2 * TST_RSA_SIG_LEN + 1];
    int  skip = 0;

#ifdef PIC_KEY_GOOD
    printf("test_core_sigverify: signed by %s, refused from %s\n", PIC_KEY_GOOD, PIC_KEY_BAD);
    if (access(PIC_KEY_GOOD, R_OK) != 0) {
        printf("  SKIP: no private key at %s\n", PIC_KEY_GOOD);
        return TST_SKIP;
    }

    CHECK(activation_args(PIC_KEY_GOOD, DNA16, args) == 0, "the key signs the activation");
    CHECK(core_activate_verify(DNA16, args) == 1,
            "an activation signed by the private half of core_pic_public_pem verifies");
    CHECK(core_activate_verify("0201000012345679", args) == 0,
            "the same signature does not activate another DNA");
    args[64] = args[64] == '0' ? '1' : '0';
    CHECK(core_activate_verify(DNA16, args) == 0, "a flipped signature byte is refused");
    CHECK(pkcs1_ok(PIC_KEY_GOOD) == 1,
            "a PKCS#1 v1.5 signature by the same key verifies against core_pic_public_pem");
#else
    printf("test_core_sigverify: built-in key, refused from %s\n", PIC_KEY_BAD);
    CHECK(pem_is_api_key(),
            "the built-in core_pic_public_pem is the API RSA-1024 key, exponent 65537");
#endif

    if (access(PIC_KEY_BAD, R_OK) == 0) {
        CHECK(activation_args(PIC_KEY_BAD, DNA16, args) == 0, "the other key signs the activation");
        CHECK(core_activate_verify(DNA16, args) == 0, "an activation signed by %s is refused",
                PIC_KEY_BAD);
        CHECK(pkcs1_ok(PIC_KEY_BAD) == 0, "a PKCS#1 v1.5 signature by %s is refused", PIC_KEY_BAD);
    } else {
        printf("  SKIP: no private key at %s\n", PIC_KEY_BAD);
        skip = 1;
    }
    return tst_done() ? 1 : skip ? TST_SKIP : 0;
}
