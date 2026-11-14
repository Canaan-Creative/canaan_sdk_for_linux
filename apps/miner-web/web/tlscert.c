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
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Fixed validity: the device clock may still read 1970 when the certificate
 * is made, so neither end is taken from it. */
#define CERT_NOT_BEFORE "20260101000000Z"
#define CERT_NOT_AFTER  "20491231235959Z"

#define CERT_SERIAL_BYTES 16

static int cert_usable(const char *path)
{
    struct stat st;
    X509       *x = NULL;
    EVP_PKEY   *k = NULL;
    FILE       *f;
    int         fd, ok = 0;

    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return 0;
    f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return 0;
    }
    k = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    rewind(f);
    x  = PEM_read_X509(f, NULL, NULL, NULL);
    ok = k && x && X509_check_private_key(x, k) == 1;
    fclose(f);
    X509_free(x);
    EVP_PKEY_free(k);
    return ok;
}

static int add_ext(X509 *x, int nid, const char *value)
{
    X509V3_CTX      v3;
    X509_EXTENSION *ext;
    int             ok;

    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, NULL, NULL, 0);
    ext = X509V3_EXT_conf_nid(NULL, &v3, nid, value);
    ok  = ext && X509_add_ext(x, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

static int set_time(int (*set)(X509 *, const ASN1_TIME *), X509 *x, const char *when)
{
    ASN1_TIME *t  = ASN1_TIME_new();
    int        ok = t && ASN1_TIME_set_string_X509(t, when) == 1 && set(x, t) == 1;

    ASN1_TIME_free(t);
    return ok;
}

static int set_serial(X509 *x)
{
    unsigned char raw[CERT_SERIAL_BYTES];
    BIGNUM       *bn;
    int           ok;

    if (RAND_bytes(raw, sizeof raw) != 1)
        return 0;
    raw[0] &= 0x7f; /* a positive INTEGER */
    raw[0] |= 0x01; /* never zero, full length */
    bn = BN_bin2bn(raw, sizeof raw, NULL);
    ok = bn && BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x)) != NULL;
    BN_free(bn);
    return ok;
}

static X509 *cert_make(EVP_PKEY *key, const char *cn)
{
    char       san[HOST_NAME_MAX + 8];
    X509      *x = X509_new();
    X509_NAME *name;

    if (!x)
        return NULL;
    name = X509_get_subject_name(x);
    snprintf(san, sizeof san, "DNS:%s", cn);
    if (X509_set_version(x, X509_VERSION_3) != 1 || !set_serial(x) ||
            !set_time(X509_set1_notBefore, x, CERT_NOT_BEFORE) ||
            !set_time(X509_set1_notAfter, x, CERT_NOT_AFTER) ||
            X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8,
                    (const unsigned char *)cn, -1, -1, 0) != 1 ||
            X509_set_issuer_name(x, name) != 1 || X509_set_pubkey(x, key) != 1 ||
            !add_ext(x, NID_basic_constraints, "critical,CA:FALSE") ||
            !add_ext(x, NID_key_usage, "critical,digitalSignature") ||
            !add_ext(x, NID_ext_key_usage, "serverAuth") ||
            !add_ext(x, NID_subject_alt_name, san) ||
            !add_ext(x, NID_subject_key_identifier, "hash") ||
            X509_sign(x, key, EVP_sha256()) <= 0) {
        X509_free(x);
        return NULL;
    }
    return x;
}

static int cert_write(const char *tmp, EVP_PKEY *key, X509 *x)
{
    FILE *f;
    int   fd, ok;

    if (unlink(tmp) != 0 && errno != ENOENT)
        return 0;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return 0;
    }
    ok = PEM_write_X509(f, x) == 1 &&
            PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL) == 1 && fflush(f) == 0 &&
            fsync(fd) == 0;
    return fclose(f) == 0 && ok;
}

static void dir_sync(const char *path)
{
    char        dir[PATH_MAX];
    const char *slash = strrchr(path, '/');
    int         fd;

    if (!slash)
        snprintf(dir, sizeof dir, ".");
    else if (slash == path)
        snprintf(dir, sizeof dir, "/");
    else
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
    fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int web_tls_cert_ensure(const char *path, const char *common_name)
{
    char      tmp[PATH_MAX];
    EVP_PKEY *key;
    X509     *x = NULL;
    int       ok;

    if (!path || !common_name || !common_name[0] || strlen(common_name) > HOST_NAME_MAX ||
            snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;
    if (cert_usable(path))
        return 0;
    key = EVP_EC_gen("P-256");
    if (key)
        x = cert_make(key, common_name);
    ok = x && cert_write(tmp, key, x);
    if (ok && rename(tmp, path) != 0)
        ok = 0;
    if (!ok)
        unlink(tmp);
    else
        dir_sync(path);
    X509_free(x);
    EVP_PKEY_free(key);
    return ok ? 0 : -1;
}
