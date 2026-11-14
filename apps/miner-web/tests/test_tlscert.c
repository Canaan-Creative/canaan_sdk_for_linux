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

#define _DEFAULT_SOURCE

#include "tlscert.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define DIR_OK "tlscert_dir"
#define DIR_RO "tlscert_ro"
#define PEM    DIR_OK "/web.pem"
#define OTHER  DIR_OK "/other.pem"

/* Counts a failed cond in fails and prints msg as FAIL or ok */
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

static X509 *read_cert(const char *path)
{
    FILE *f = fopen(path, "r");
    X509 *x = f ? PEM_read_X509(f, NULL, NULL, NULL) : NULL;

    if (f)
        fclose(f);
    return x;
}

static EVP_PKEY *read_key(const char *path)
{
    FILE     *f = fopen(path, "r");
    EVP_PKEY *k = f ? PEM_read_PrivateKey(f, NULL, NULL, NULL) : NULL;

    if (f)
        fclose(f);
    return k;
}

static int usable(const char *path)
{
    X509     *x  = read_cert(path);
    EVP_PKEY *k  = read_key(path);
    int       ok = x && k && X509_check_private_key(x, k) == 1;

    X509_free(x);
    EVP_PKEY_free(k);
    return ok;
}

static size_t slurp(const char *path, char *buf, size_t cap)
{
    FILE  *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;

    if (f)
        fclose(f);
    buf[n] = '\0';
    return n;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void cleanup(void)
{
    unlink(PEM);
    unlink(PEM ".tmp");
    unlink(OTHER);
    rmdir(DIR_OK);
    rmdir(DIR_RO);
}

static void check_contents(const char *cn)
{
    X509     *x        = read_cert(PEM);
    EVP_PKEY *k        = read_key(PEM);
    char      name[64] = "", group[32] = "";
    size_t    glen = 0;
    int       day, sec;

    CHECK(x && k && X509_check_private_key(x, k) == 1, "key and certificate match");
    if (!x || !k) {
        X509_free(x);
        EVP_PKEY_free(k);
        return;
    }
    X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, name, sizeof name);
    CHECK(!strcmp(name, cn), "subject CN is the device name");
    CHECK(X509_NAME_cmp(X509_get_subject_name(x), X509_get_issuer_name(x)) == 0 &&
                    X509_verify(x, k) == 1,
            "certificate is self-signed");
    CHECK(X509_check_host(x, cn, 0, 0, NULL) == 1, "DNS SAN carries the device name");
    CHECK(EVP_PKEY_get_group_name(k, group, sizeof group, &glen) == 1 &&
                    !strcmp(group, "prime256v1"),
            "key is EC P-256");
    CHECK(ASN1_TIME_diff(&day, &sec, NULL, X509_get0_notBefore(x)) == 1 && (day < 0 || sec < 0),
            "notBefore is in the past");
    {
        ASN1_TIME *end = ASN1_TIME_new();
        CHECK(end && ASN1_TIME_set_string_X509(end, "20491231235959Z") == 1 &&
                        ASN1_TIME_compare(X509_get0_notAfter(x), end) == 0,
                "notAfter is the end of 2049");
        ASN1_TIME_free(end);
    }
    CHECK(X509_get_extension_flags(x) & EXFLAG_XKUSAGE &&
                    X509_get_extended_key_usage(x) & XKU_SSL_SERVER,
            "certificate is for TLS servers");
    X509_free(x);
    EVP_PKEY_free(k);
}

int main(void)
{
    struct stat st, st2;
    char        before[8192], after[8192];

    cleanup();
    mkdir(DIR_OK, 0755);

    CHECK(web_tls_cert_ensure(PEM, "nano3s-test") == 0, "missing file -> created");
    CHECK(stat(PEM, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600,
            "created file is a regular file, mode 0600");
    check_contents("nano3s-test");
    CHECK(access(PEM ".tmp", F_OK) != 0, "no temporary file left behind");

    slurp(PEM, before, sizeof before);
    CHECK(web_tls_cert_ensure(PEM, "nano3s-test") == 0 && stat(PEM, &st2) == 0 &&
                    st2.st_ino == st.st_ino && slurp(PEM, after, sizeof after) &&
                    !strcmp(before, after),
            "usable file -> kept as it is");
    CHECK(web_tls_cert_ensure(PEM, "renamed-host") == 0 && slurp(PEM, after, sizeof after) &&
                    !strcmp(before, after),
            "a new host name does not replace a usable certificate");

    put(PEM, "not a certificate\n");
    CHECK(web_tls_cert_ensure(PEM, "nano3s-test") == 0 && usable(PEM), "garbage -> replaced");

    put(PEM, "");
    CHECK(web_tls_cert_ensure(PEM, "nano3s-test") == 0 && usable(PEM), "empty file -> replaced");

    {
        char cert_only[8192], *cut;

        slurp(PEM, cert_only, sizeof cert_only);
        CHECK(strncmp(cert_only, "-----BEGIN CERTIFICATE-----", 27) == 0,
                "the certificate comes first, the key after it");
        cut = strstr(cert_only, "-----BEGIN PRIVATE KEY-----");
        if (cut)
            *cut = '\0';
        put(PEM, cert_only);
        CHECK(web_tls_cert_ensure(PEM, "nano3s-test") == 0 && usable(PEM),
                "certificate without a key -> replaced");
    }

    {
        char a[8192], b[8192], spliced[16384], *ca, *cb;

        CHECK(web_tls_cert_ensure(OTHER, "nano3s-test") == 0, "second key pair created");
        slurp(PEM, a, sizeof a);
        slurp(OTHER, b, sizeof b);
        ca = strstr(a, "-----BEGIN PRIVATE KEY-----");
        cb = strstr(b, "-----BEGIN PRIVATE KEY-----");
        if (ca && cb) {
            *ca = '\0';
            snprintf(spliced, sizeof spliced, "%s%s", a, cb);
            put(PEM, spliced);
        }
        CHECK(ca && cb && !usable(PEM), "spliced file has a certificate for another key");
        CHECK(web_tls_cert_ensure(PEM, "nano3s-test") == 0 && usable(PEM),
                "certificate for another key -> replaced");
    }

    unlink(PEM);
    CHECK(symlink("other.pem", PEM) == 0, "path made a symlink to a usable file");
    slurp(OTHER, before, sizeof before);
    CHECK(web_tls_cert_ensure(PEM, "nano3s-test") == 0 && lstat(PEM, &st) == 0 &&
                    S_ISREG(st.st_mode) && usable(PEM),
            "symlink -> replaced by a regular file");
    CHECK(slurp(OTHER, after, sizeof after) && !strcmp(before, after),
            "the symlink target is left alone");

    mkdir(DIR_RO, 0555);
    if (geteuid() != 0) {
        CHECK(web_tls_cert_ensure(DIR_RO "/web.pem", "nano3s-test") != 0,
                "unwritable directory -> failure");
        CHECK(access(DIR_RO "/web.pem.tmp", F_OK) != 0, "failure leaves no temporary file");
    } else {
        printf("SKIP: unwritable directory -> failure (root)\n");
    }
    CHECK(web_tls_cert_ensure(DIR_OK "/missing/web.pem", "nano3s-test") != 0,
            "missing directory -> failure");
    CHECK(web_tls_cert_ensure(NULL, "nano3s-test") != 0, "no path -> failure");
    CHECK(web_tls_cert_ensure(PEM, NULL) != 0 && web_tls_cert_ensure(PEM, "") != 0,
            "no common name -> failure");

    cleanup();
    if (fails) {
        fprintf(stderr, "test_tlscert: %d FAIL\n", fails);
        return 1;
    }
    printf("test_tlscert: all pass\n");
    return geteuid() == 0 ? TST_SKIP : 0;
}
