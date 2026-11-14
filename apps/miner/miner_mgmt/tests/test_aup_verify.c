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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#ifndef AUP_VERIFY_SRC
#error "AUP_VERIFY_SRC must name the product source that carries the embedded key"
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

/* DER body of the rsaEncryption OID 1.2.840.113549.1.1.1 */
static const unsigned char oid_rsa_encryption[9] = {
    0x2a,
    0x86,
    0x48,
    0x86,
    0xf7,
    0x0d,
    0x01,
    0x01,
    0x01,
};

static void fill_pattern(unsigned char *b, size_t n)
{
    uint32_t x = 0x12345678u;
    size_t   i;

    for (i = 0; i < n; i++) {
        x    = x * 1103515245u + 12345u;
        b[i] = (unsigned char)(x >> 16);
    }
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long  n;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    b = malloc((size_t)n + 1);
    if (b == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    b[n] = '\0';
    if (len != NULL)
        *len = (size_t)n;
    return b;
}

static size_t count_occurrences(const char *hay, const char *needle)
{
    size_t      n = 0;
    const char *p = hay;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static size_t literal_concat(const char *decl, const char *end, char *out, size_t cap)
{
    const char *p = decl;
    size_t      o = 0;

    while (p < end) {
        if (*p != '"') {
            p++;
            continue;
        }
        p++;
        while (p < end && *p != '"') {
            char c = *p++;

            if (c == '\\' && p < end) {
                char e = *p++;

                if (e == 'n')
                    c = '\n';
                else if (e == 't')
                    c = '\t';
                else
                    c = e;
            }
            if (o + 1 >= cap)
                return 0;
            out[o++] = c;
        }
        if (p < end)
            p++;
    }
    out[o] = '\0';
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static size_t b64_decode(const char *s, size_t n, unsigned char *out, size_t cap)
{
    size_t   i, o = 0;
    uint32_t acc  = 0;
    int      bits = 0;

    for (i = 0; i < n; i++) {
        int v;

        if (s[i] == '=')
            break;
        v = b64val(s[i]);
        if (v < 0)
            continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap)
                return 0;
            out[o++] = (unsigned char)((acc >> bits) & 0xffu);
        }
    }
    return o;
}

static int der_tlv(const unsigned char *b, size_t len, size_t pos, unsigned *tag, size_t *vpos,
        size_t *vlen)
{
    size_t l;

    if (pos + 2 > len)
        return -1;
    *tag = b[pos];
    l    = b[pos + 1];
    pos += 2;
    if (l & 0x80u) {
        size_t nb = l & 0x7fu, k;

        if (nb == 0 || nb > 4 || pos + nb > len)
            return -1;
        l = 0;
        for (k = 0; k < nb; k++)
            l = (l << 8) | b[pos + k];
        pos += nb;
    }
    if (pos + l > len)
        return -1;
    *vpos = pos;
    *vlen = l;
    return 0;
}

static int dev_key_path(char *out, size_t cap)
{
    static const char src[]  = AUP_VERIFY_SRC;
    static const char tail[] = "/../tools/aup-dev-signing-key.pem";
    size_t            i, cut = 0, seen = 0;

    for (i = sizeof src - 1; i > 0; i--) {
        if (src[i - 1] != '/')
            continue;
        seen++;
        if (seen == 2) {
            cut = i - 1;
            break;
        }
    }
    if (seen < 2 || cut + sizeof tail > cap)
        return -1;
    memcpy(out, src, cut);
    memcpy(out + cut, tail, sizeof tail);
    return 0;
}

static int rsa_priv_modulus(const unsigned char *der, size_t inp, size_t inl, size_t *npos,
        size_t *nlen)
{
    size_t   seqp, seql, p, l, np, nl;
    unsigned tag;

    if (der_tlv(der, inp + inl, inp, &tag, &seqp, &seql) != 0 || tag != 0x30)
        return -1;
    if (der_tlv(der, seqp + seql, seqp, &tag, &p, &l) != 0 || tag != 0x02)
        return -1;
    if (der_tlv(der, seqp + seql, p + l, &tag, &np, &nl) != 0 || tag != 0x02)
        return -1;
    while (nl > 0 && der[np] == 0) {
        np++;
        nl--;
    }
    *npos = np;
    *nlen = nl;
    return 0;
}

static int pem_priv_modulus(const char *pem, unsigned char *der, size_t dercap, size_t *npos,
        size_t *nlen)
{
    static const char p8b[] = "-----BEGIN PRIVATE KEY-----";
    static const char p8e[] = "-----END PRIVATE KEY-----";
    static const char p1b[] = "-----BEGIN RSA PRIVATE KEY-----";
    static const char p1e[] = "-----END RSA PRIVATE KEY-----";
    const char       *b     = strstr(pem, p1b), *e;
    int               pkcs1 = b != NULL;
    size_t            derlen, seqp, seql, algp, algl, oidp, oidl, kp, kl, p, l;
    unsigned          tag;

    if (pkcs1) {
        b += sizeof p1b - 1;
        e = strstr(b, p1e);
    } else {
        b = strstr(pem, p8b);
        if (b == NULL)
            return -1;
        b += sizeof p8b - 1;
        e = strstr(b, p8e);
    }
    if (e == NULL)
        return -1;
    derlen = b64_decode(b, (size_t)(e - b), der, dercap);
    if (derlen == 0)
        return -1;
    if (pkcs1)
        return rsa_priv_modulus(der, 0, derlen, npos, nlen);

    if (der_tlv(der, derlen, 0, &tag, &seqp, &seql) != 0 || tag != 0x30)
        return -1;
    if (der_tlv(der, seqp + seql, seqp, &tag, &p, &l) != 0 || tag != 0x02)
        return -1;
    if (der_tlv(der, seqp + seql, p + l, &tag, &algp, &algl) != 0 || tag != 0x30)
        return -1;
    if (der_tlv(der, algp + algl, algp, &tag, &oidp, &oidl) != 0 || tag != 0x06 ||
            oidl != sizeof oid_rsa_encryption || memcmp(der + oidp, oid_rsa_encryption, oidl) != 0)
        return -1;
    if (der_tlv(der, seqp + seql, algp + algl, &tag, &kp, &kl) != 0 || tag != 0x04)
        return -1;
    return rsa_priv_modulus(der, kp, kl, npos, nlen);
}

static void check_dev_key_pairing(const unsigned char *embedded_n, size_t embedded_nl)
{
    unsigned char der[2048];
    char          path[1024];
    char         *pem;
    size_t        np = 0, nl = 0;

    if (dev_key_path(path, sizeof path) != 0) {
        fails++;
        printf("  FAIL: the development key path does not derive from %s\n", AUP_VERIFY_SRC);
        return;
    }

    pem = slurp(path, NULL);
    if (pem == NULL) {
        printf("  SKIP: no private key at %s — a tree without the development "
               "private key has no pairing to check, which is not a failure "
               "here\n",
                path);
        skips++;
        return;
    }

    if (pem_priv_modulus(pem, der, sizeof der, &np, &nl) != 0) {
        free(pem);
        fails++;
        printf("  FAIL: %s is present but is not an RSA private key this test "
               "can read (PKCS#8 or PKCS#1 PEM)\n",
                path);
        return;
    }
    free(pem);

    CHECK(nl == embedded_nl && memcmp(der + np, embedded_n, nl) == 0,
            "the modulus of the private key committed at %s is BYTE-IDENTICAL to "
            "the public key embedded in the verifier — the two halves that must "
            "agree, pinned against the file itself rather than against a copy of "
            "its modulus in this test (%zu vs %zu bytes)",
            path, nl, embedded_nl);
}

static void test_signed_range_layout(void)
{
    struct aup_hdr h;

    printf("the byte ranges aup_verify_signature depends on\n");
    memset(&h, 0, sizeof h);

    CHECK(AUP_SIG_LEN == 128, "AUP_SIG_LEN is 128 — an RSA-1024 signature, not RSA-2048 (%d)",
            (int)AUP_SIG_LEN);
    CHECK(AUP_SHA_LEN == SHA256_DIGEST_SIZE,
            "AUP_SHA_LEN == SHA256_DIGEST_SIZE, the digest aup_verify hands openssl (%d)",
            (int)AUP_SHA_LEN);
    CHECK(sizeof(struct aup_bin) == 72, "struct aup_bin is 72 wire bytes (%zu)",
            sizeof(struct aup_bin));
    CHECK(sizeof(struct aup_hdr) == 1320, "struct aup_hdr is 1320 wire bytes (%zu)",
            sizeof(struct aup_hdr));
    CHECK(AUP_SIGNED_LEN == 1192, "AUP_SIGNED_LEN is 1192 = sizeof(struct aup_hdr) - 128 (%zu)",
            (size_t)AUP_SIGNED_LEN);
    CHECK(offsetof(struct aup_hdr, signature) == 1192,
            "the signature field starts at byte 1192 (%zu)", offsetof(struct aup_hdr, signature));
    CHECK(offsetof(struct aup_hdr, signature) == AUP_SIGNED_LEN,
            "the signed range ends exactly where the signature begins");
    CHECK(offsetof(struct aup_hdr, signature) + AUP_SIG_LEN == sizeof(struct aup_hdr),
            "the signature occupies bytes 1192..1319 — the whole tail, nothing after it");
    CHECK(offsetof(struct aup_hdr, f) + AUP_MAX_BINS * sizeof(struct aup_bin) ==
                    offsetof(struct aup_hdr, signature),
            "the %d-slot bin table ends where the signature begins (%zu + %d*%zu)", AUP_MAX_BINS,
            offsetof(struct aup_hdr, f), AUP_MAX_BINS, sizeof(struct aup_bin));
    CHECK(sizeof(struct aup_hdr) <= AUP_HDR_SIZE,
            "the 1320-byte header fits the %d-byte header block, %zu bytes of pad", AUP_HDR_SIZE,
            (size_t)AUP_HDR_SIZE - sizeof(struct aup_hdr));
}

static void test_signed_range_behaviour(void)
{
    unsigned char buf[sizeof(struct aup_hdr)];
    uint8_t       base[SHA256_DIGEST_SIZE], d[SHA256_DIGEST_SIZE], whole[SHA256_DIGEST_SIZE];
    size_t        i, changed = 0, unchanged = 0;
    size_t        last_changed = 0, first_unchanged = sizeof buf;
    int           contiguous = 1;

    printf("what the digest actually covers, flipping every byte of the header\n");
    fill_pattern(buf, sizeof buf);
    sha256(buf, AUP_SIGNED_LEN, base);

    for (i = 0; i < sizeof buf; i++) {
        buf[i] = (unsigned char)(buf[i] ^ 0xffu);
        sha256(buf, AUP_SIGNED_LEN, d);
        buf[i] = (unsigned char)(buf[i] ^ 0xffu);
        if (memcmp(d, base, sizeof d) != 0) {
            changed++;
            if (i > last_changed)
                last_changed = i;
            if (i >= (size_t)AUP_SIGNED_LEN)
                contiguous = 0;
        } else {
            unchanged++;
            if (i < first_unchanged)
                first_unchanged = i;
            if (i < (size_t)AUP_SIGNED_LEN)
                contiguous = 0;
        }
    }

    CHECK(changed == (size_t)AUP_SIGNED_LEN,
            "exactly %zu of the %zu header bytes change the digest (%zu did)",
            (size_t)AUP_SIGNED_LEN, sizeof buf, changed);
    CHECK(unchanged == (size_t)AUP_SIG_LEN,
            "exactly %d bytes do not — the signature field cannot sign itself (%zu did not)",
            (int)AUP_SIG_LEN, unchanged);
    CHECK(contiguous, "the two sets are the contiguous split at 1192, not an interleaving");
    CHECK(last_changed == (size_t)AUP_SIGNED_LEN - 1, "the last covered byte is 1191 (%zu)",
            last_changed);
    CHECK(first_unchanged == (size_t)AUP_SIGNED_LEN, "the first uncovered byte is 1192 (%zu)",
            first_unchanged);

    sha256(buf, sizeof buf, whole);
    CHECK(memcmp(whole, base, sizeof whole) != 0,
            "hashing all 1320 bytes gives a different digest from hashing 1192 — "
            "the exclusion is real, not a length that happens not to matter");
}

static void test_embedded_key(void)
{
    size_t               srclen = 0;
    char                *src    = slurp(AUP_VERIFY_SRC, &srclen);
    char                 pem[2048];
    const char          *decl, *semi, *b64s, *b64e, *call, *end, *tok;
    unsigned char        der[512];
    size_t               pemlen, derlen, seqp, seql, algp, algl, oidp, oidl;
    size_t               bitp, bitl, inp, inl, np, nl, ep, el, next;
    unsigned             tag;
    uint32_t             e = 0;
    const unsigned char *n;

    printf("the public key baked into %s\n", AUP_VERIFY_SRC);

    CHECK(src != NULL, "the product source is readable at AUP_VERIFY_SRC");
    if (src == NULL)
        return;

    CHECK(count_occurrences(src, "-----BEGIN PUBLIC KEY-----") == 1,
            "exactly one public key is baked into the verifier (%zu found)",
            count_occurrences(src, "-----BEGIN PUBLIC KEY-----"));
    CHECK(strstr(src, "RSA_PKCS1_PADDING") != NULL,
            "the padding is PKCS#1 v1.5 (RSA_PKCS1_PADDING) — the real verify below "
            "proves it end to end, this pins the source too");
    CHECK(strstr(src, "EVP_sha256") != NULL, "the digest is SHA-256 (EVP_sha256)");

    call = strstr(src, "sha256(");
    end  = call != NULL ? strchr(call, ')') : NULL;
    tok  = call != NULL ? strstr(call, "AUP_SIGNED_LEN") : NULL;
    CHECK(call != NULL && end != NULL && tok != NULL && tok < end,
            "AUP_SIGNED_LEN is the length argument of the verifier's own sha256() call — "
            "hashing sizeof *h instead fails every legitimate package on the device with "
            "this whole host gate still green");

    decl = strstr(src, "aup_public_pem");
    CHECK(decl != NULL, "the key literal is named aup_public_pem");
    if (decl == NULL) {
        free(src);
        return;
    }
    semi = strchr(decl, ';');
    CHECK(semi != NULL, "the declaration terminates");
    if (semi == NULL) {
        free(src);
        return;
    }

    pemlen = literal_concat(decl, semi, pem, sizeof pem);
    free(src);
    CHECK(pemlen > 0, "the concatenated string literals fit the buffer (%zu bytes)", pemlen);
    if (pemlen == 0)
        return;

    CHECK(strncmp(pem, "-----BEGIN PUBLIC KEY-----\n", 27) == 0,
            "the literal opens with the PEM BEGIN line");
    CHECK(pemlen >= 25 && strcmp(pem + pemlen - 25, "-----END PUBLIC KEY-----\n") == 0,
            "...and closes with END plus the trailing newline PEM_read_bio_PUBKEY "
            "needs — dropping it makes every package fail to verify");

    b64s   = pem + 27;
    b64e   = pem + pemlen - 25;
    derlen = b64_decode(b64s, (size_t)(b64e - b64s), der, sizeof der);
    CHECK(derlen > 0, "the base64 body decodes (%zu DER bytes)", derlen);
    if (derlen == 0)
        return;

    CHECK(der_tlv(der, derlen, 0, &tag, &seqp, &seql) == 0 && tag == 0x30 && seqp + seql == derlen,
            "the DER is one SubjectPublicKeyInfo SEQUENCE spanning the whole body");
    if (der_tlv(der, derlen, 0, &tag, &seqp, &seql) != 0)
        return;

    CHECK(der_tlv(der, seqp + seql, seqp, &tag, &algp, &algl) == 0 && tag == 0x30,
            "its first element is the AlgorithmIdentifier SEQUENCE");
    if (der_tlv(der, seqp + seql, seqp, &tag, &algp, &algl) != 0)
        return;
    next = algp + algl;

    CHECK(der_tlv(der, next, algp, &tag, &oidp, &oidl) == 0 && tag == 0x06 &&
                    oidl == sizeof oid_rsa_encryption &&
                    memcmp(der + oidp, oid_rsa_encryption, oidl) == 0,
            "the algorithm OID is rsaEncryption 1.2.840.113549.1.1.1");

    CHECK(der_tlv(der, seqp + seql, next, &tag, &bitp, &bitl) == 0 && tag == 0x03 && bitl > 1 &&
                    der[bitp] == 0,
            "the key sits in a BIT STRING with no unused bits");
    if (der_tlv(der, seqp + seql, next, &tag, &bitp, &bitl) != 0 || bitl < 2)
        return;

    CHECK(der_tlv(der, bitp + bitl, bitp + 1, &tag, &inp, &inl) == 0 && tag == 0x30,
            "the BIT STRING wraps an RSAPublicKey SEQUENCE");
    if (der_tlv(der, bitp + bitl, bitp + 1, &tag, &inp, &inl) != 0)
        return;

    CHECK(der_tlv(der, inp + inl, inp, &tag, &np, &nl) == 0 && tag == 0x02,
            "the modulus is the first INTEGER");
    if (der_tlv(der, inp + inl, inp, &tag, &np, &nl) != 0)
        return;
    CHECK(der_tlv(der, inp + inl, np + nl, &tag, &ep, &el) == 0 && tag == 0x02,
            "the exponent is the second INTEGER");
    if (der_tlv(der, inp + inl, np + nl, &tag, &ep, &el) != 0)
        return;

    n = der + np;
    while (nl > 0 && n[0] == 0) {
        n++;
        nl--;
    }
    CHECK(nl == 128,
            "the modulus is %zu bytes — a 1024-bit RSA key, which is why a 128-byte "
            "signature is the right size",
            nl);
    while (el > 0 && der[ep] == 0) {
        ep++;
        el--;
    }
    if (el <= 4) {
        size_t k;

        for (k = 0; k < el; k++)
            e = (e << 8) | der[ep + k];
    }
    CHECK(e == 65537u,
            "the public exponent is 65537, as the U-Boot verifier expects "
            "(%u)",
            (unsigned)e);

    check_dev_key_pairing(n, nl);
}

static int sign_signed_range(struct aup_hdr *h, EVP_PKEY *key)
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

static EVP_PKEY *load_dev_key(void)
{
    char      path[1024];
    FILE     *f;
    EVP_PKEY *k;

    if (dev_key_path(path, sizeof path) != 0)
        return NULL;
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    k = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return k;
}

static EVP_PKEY *gen_rsa1024(void)
{
    EVP_PKEY     *k = NULL;
    EVP_PKEY_CTX *c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);

    if (c != NULL && EVP_PKEY_keygen_init(c) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(c, 1024) > 0)
        EVP_PKEY_keygen(c, &k);
    EVP_PKEY_CTX_free(c);
    return k;
}

static void test_real_verify(void)
{
    struct aup_hdr h;
    EVP_PKEY      *dev, *other;

    printf("the RSA maths itself: sign a header and run aup_verify_signature()\n");

    other = gen_rsa1024();
    CHECK(other != NULL, "openssl generates an RSA-1024 key for the reject case");
    if (other != NULL) {
        memset(&h, 0, sizeof h);
        fill_pattern((unsigned char *)&h, AUP_SIGNED_LEN);
        if (sign_signed_range(&h, other) == 0)
            CHECK(aup_verify_signature(&h) != 0, "a header signed by a DIFFERENT key is rejected");
        EVP_PKEY_free(other);
    }

    dev = load_dev_key();
    if (dev == NULL) {
        printf("  SKIP: no dev private key on disk — "
               "accept/tamper cases skipped, reject case above still ran\n");
        skips++;
        return;
    }

    memset(&h, 0, sizeof h);
    fill_pattern((unsigned char *)&h, AUP_SIGNED_LEN);
    CHECK(sign_signed_range(&h, dev) == 0, "the dev key signs the header");
    CHECK(aup_verify_signature(&h) == 0,
            "a header signed by the dev key VERIFIES — RSA-1024 PKCS#1 v1.5 over "
            "SHA-256 works on this openssl, the whole point of the port");

    ((unsigned char *)&h)[0] ^= 0xffu;
    CHECK(aup_verify_signature(&h) != 0, "flipping a byte in the signed range breaks verification");
    ((unsigned char *)&h)[0] ^= 0xffu;

    h.signature[0] ^= 0xffu;
    CHECK(aup_verify_signature(&h) != 0, "corrupting a signature byte breaks verification");
    h.signature[0] ^= 0xffu;
    CHECK(aup_verify_signature(&h) == 0, "...and restoring the byte verifies again");

    EVP_PKEY_free(dev);
}

int main(void)
{
    printf("test_aup_verify: the framing, the key, and the RSA maths of the OTA gate\n");

    test_signed_range_layout();
    test_signed_range_behaviour();
    test_embedded_key();
    test_real_verify();

    printf(fails ? "test_aup_verify: FAILED (%d)\n" : "test_aup_verify: all pass\n", fails);
    return fails ? 1 : skips ? TST_SKIP : 0;
}
