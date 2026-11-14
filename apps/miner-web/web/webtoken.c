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

#include "webtoken.h"
#include "jread.h"
#include "jwrite.h"
#include "web_logfmt.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#define TOKEN_HEADER      "{\"alg\":\"HS256\",\"typ\":\"JWT\"}"
#define TOKEN_MAC_LEN     32 /* HMAC-SHA256 length, bytes */
#define TOKEN_MAC_B64     43 /* base64url length of the MAC, chars */
#define TOKEN_KEY_FILE    (WEB_TOKEN_KEY_LEN + TOKEN_MAC_LEN)
#define TOKEN_PAYLOAD_MAX 640
#define TOKEN_BOOT_ID_MAX 64
#define TOKEN_WALL_MIN    1767225600LL /* 2026-01-01T00:00:00Z */
#define TOKEN_WALL_MAX    4102444800LL /* 2100-01-01T00:00:00Z */

static const char b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char cred_tag_fp[]    = "fp\n";
static const char cred_tag_token[] = "token\n";

static pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char   g_key[WEB_TOKEN_KEY_LEN];
static unsigned char   g_fp[TOKEN_MAC_LEN];
static int             g_key_ok; /* 1 when g_key and g_fp hold the stored key */

static size_t b64url_encode(char *out, const unsigned char *in, size_t n)
{
    size_t o = 0;

    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;

        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = b64url[(v >> 18) & 63];
        out[o++] = b64url[(v >> 12) & 63];
        if (i + 1 < n)
            out[o++] = b64url[(v >> 6) & 63];
        if (i + 2 < n)
            out[o++] = b64url[v & 63];
    }
    out[o] = '\0';
    return o;
}

static int b64url_decode(unsigned char *out, size_t cap, const char *in, size_t n)
{
    uint32_t v    = 0;
    size_t   o    = 0;
    int      bits = 0;

    if (n % 4 == 1)
        return -1;
    for (size_t i = 0; i < n; i++) {
        const char *p = memchr(b64url, in[i], sizeof b64url - 1);

        if (!p)
            return -1;
        v = (v << 6) | (uint32_t)(p - b64url);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap)
                return -1;
            out[o++] = (unsigned char)(v >> bits);
        }
    }
    return (int)o;
}

static void token_dir_sync(const char *path)
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

static int cred_mac(const unsigned char *s, const char *tag, const mc_webcred *c,
        unsigned char *out)
{
    char         msg[sizeof cred_tag_token + MC_WEBUSER_MAX + MC_WEBPASS_MAX + 1];
    unsigned int n  = 0;
    int          ml = snprintf(msg, sizeof msg, "%s%s\n%s", tag, c->user, c->pass);
    int          ok = ml >= 0 && (size_t)ml < sizeof msg &&
            HMAC(EVP_sha256(), s, WEB_TOKEN_KEY_LEN, (const unsigned char *)msg, (size_t)ml, out,
                    &n) &&
            n == TOKEN_MAC_LEN;

    explicit_bzero(msg, sizeof msg);
    return ok;
}

static int key_read(const char *path, unsigned char *s, unsigned char *f)
{
    unsigned char buf[TOKEN_KEY_FILE];
    struct stat   st;
    int           fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int           ok;

    if (fd < 0)
        return 0;
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
            (st.st_mode & 077) == 0 && st.st_size == TOKEN_KEY_FILE &&
            read(fd, buf, sizeof buf) == TOKEN_KEY_FILE;
    close(fd);
    if (ok) {
        memcpy(s, buf, WEB_TOKEN_KEY_LEN);
        memcpy(f, buf + WEB_TOKEN_KEY_LEN, TOKEN_MAC_LEN);
    }
    explicit_bzero(buf, sizeof buf);
    return ok;
}

static int key_write(const char *path, const unsigned char *s, const unsigned char *f)
{
    unsigned char buf[TOKEN_KEY_FILE];
    char          tmp[PATH_MAX];
    int           fd, ok;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp ||
            (unlink(tmp) != 0 && errno != ENOENT))
        return 0;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    memcpy(buf, s, WEB_TOKEN_KEY_LEN);
    memcpy(buf + WEB_TOKEN_KEY_LEN, f, TOKEN_MAC_LEN);
    ok = write(fd, buf, sizeof buf) == TOKEN_KEY_FILE && fsync(fd) == 0;
    explicit_bzero(buf, sizeof buf);
    ok = close(fd) == 0 && ok && rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
    else
        token_dir_sync(path);
    return ok;
}

static int key_matches(const unsigned char *s, const unsigned char *f, const mc_webcred *c)
{
    unsigned char now[TOKEN_MAC_LEN];
    int           ok = cred_mac(s, cred_tag_fp, c, now) && !CRYPTO_memcmp(now, f, sizeof now);

    explicit_bzero(now, sizeof now);
    return ok;
}

static int key_ensure(const char *path, const mc_webcred *c, unsigned char *s, unsigned char *f)
{
    if (key_read(path, s, f) && key_matches(s, f, c))
        return 1;
    if (RAND_bytes(s, WEB_TOKEN_KEY_LEN) != 1 || !cred_mac(s, cred_tag_fp, c, f) ||
            !key_write(path, s, f)) {
        explicit_bzero(s, WEB_TOKEN_KEY_LEN);
        explicit_bzero(f, TOKEN_MAC_LEN);
        return 0;
    }
    web_logf(stderr, "INFO", "web token key created\n");
    return 1;
}

static int key_get(const mc_webcred *c, unsigned char *out)
{
    int ok;

    pthread_mutex_lock(&g_key_lock);
    if (g_key_ok && !key_matches(g_key, g_fp, c))
        g_key_ok = 0;
    if (!g_key_ok) {
        g_key_ok = key_ensure(WEB_TOKEN_KEY, c, g_key, g_fp);
        if (!g_key_ok) {
            web_logf(stderr, "WARN",
                    "cannot keep the web token key on the data partition; using a temporary one "
                    "until reboot\n");
            g_key_ok = key_ensure(WEB_TOKEN_KEY_TMP, c, g_key, g_fp);
        }
        if (!g_key_ok)
            web_logf(stderr, "ERR", "no web token key; no token is issued or accepted\n");
    }
    ok = g_key_ok;
    if (ok)
        memcpy(out, g_key, WEB_TOKEN_KEY_LEN);
    pthread_mutex_unlock(&g_key_lock);
    return ok;
}

static int cred_key(const mc_webcred *c, unsigned char *k)
{
    unsigned char s[WEB_TOKEN_KEY_LEN];
    int           ok = key_get(c, s) && cred_mac(s, cred_tag_token, c, k);

    explicit_bzero(s, sizeof s);
    return ok;
}

static int token_mac(const mc_webcred *c, const char *signed_part, size_t n, char *out)
{
    unsigned char k[TOKEN_MAC_LEN], mac[TOKEN_MAC_LEN];
    unsigned int  ml = 0;
    int           ok = cred_key(c, k) &&
            HMAC(EVP_sha256(), k, sizeof k, (const unsigned char *)signed_part, n, mac, &ml) &&
            ml == TOKEN_MAC_LEN;

    if (ok)
        b64url_encode(out, mac, sizeof mac);
    explicit_bzero(k, sizeof k);
    explicit_bzero(mac, sizeof mac);
    return ok;
}

static int boot_id(char *out, size_t cap)
{
    FILE  *f = fopen(WEB_TOKEN_BOOT_ID, "r");
    size_t n;

    if (!f)
        return 0;
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n]                    = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

static long long clock_s(clockid_t id)
{
    struct timespec ts;

    if (clock_gettime(id, &ts) != 0)
        return -1;
    return (long long)ts.tv_sec;
}

static int clock_trusted(long long wall)
{
    struct timex tx;

    memset(&tx, 0, sizeof tx);
    return adjtimex(&tx) != TIME_ERROR && wall >= TOKEN_WALL_MIN && wall < TOKEN_WALL_MAX;
}

int web_token_issue(const mc_webcred *c, char *out, size_t cap)
{
    char      payload[TOKEN_PAYLOAD_MAX], boot[TOKEN_BOOT_ID_MAX], num[24];
    char      hdr[64];
    size_t    hn, pn;
    jwriter   w;
    long long wall = clock_s(CLOCK_REALTIME);
    long long up   = clock_s(CLOCK_BOOTTIME);

    if (cap)
        out[0] = '\0';
    if (!c || !c->loaded || cap < WEB_TOKEN_MAX || !boot_id(boot, sizeof boot) || up < 0)
        return -1;
    jw_init(&w, payload, sizeof payload);
    jw_obj_open(&w);
    jw_kv_str(&w, "User", c->user);
    snprintf(num, sizeof num, "%lld", wall);
    jw_kv_raw(&w, "Iat", num);
    snprintf(num, sizeof num, "%lld", wall + WEB_TOKEN_TTL_S);
    jw_kv_raw(&w, "Expiration", num);
    jw_kv_str(&w, "Boot", boot);
    snprintf(num, sizeof num, "%lld", up + WEB_TOKEN_TTL_S);
    jw_kv_raw(&w, "Up", num);
    jw_kv_raw(&w, "Synced", clock_trusted(wall) ? "1" : "0");
    jw_obj_close(&w);
    pn = jw_len(&w);
    if (!jw_ok(&w) || pn >= sizeof payload)
        return -1;

    hn = b64url_encode(hdr, (const unsigned char *)TOKEN_HEADER, strlen(TOKEN_HEADER));
    if (hn + 1 + (pn + 2) / 3 * 4 + 1 + TOKEN_MAC_B64 >= cap)
        return -1;
    memcpy(out, hdr, hn);
    out[hn] = '.';
    pn      = hn + 1 + b64url_encode(out + hn + 1, (const unsigned char *)payload, pn);
    out[pn] = '.';
    if (!token_mac(c, out, pn, out + pn + 1)) {
        explicit_bzero(out, cap);
        return -1;
    }
    return 0;
}

static int json_whole(const json_value *v, long long *out)
{
    if (!json_is(v, JSON_NUMBER) || json_number(v) != (double)json_int(v))
        return 0;
    *out = json_int(v);
    return 1;
}

static web_token_result payload_check(const mc_webcred *c, const char *text)
{
    json_value      *v = json_parse(text);
    char             boot[TOKEN_BOOT_ID_MAX];
    long long        iat, expires, up, synced, now;
    web_token_result r = WEB_TOKEN_INVALID;

    if (!json_is(v, JSON_OBJECT) || !json_is(json_object_get(v, "User"), JSON_STRING) ||
            strcmp(json_string(json_object_get(v, "User")), c->user) ||
            !json_is(json_object_get(v, "Boot"), JSON_STRING) ||
            !json_whole(json_object_get(v, "Iat"), &iat) ||
            !json_whole(json_object_get(v, "Expiration"), &expires) ||
            !json_whole(json_object_get(v, "Up"), &up) ||
            !json_whole(json_object_get(v, "Synced"), &synced) ||
            expires - iat != WEB_TOKEN_TTL_S || !boot_id(boot, sizeof boot))
        goto out;
    if (!strcmp(json_string(json_object_get(v, "Boot")), boot)) {
        now = clock_s(CLOCK_BOOTTIME);
        if (now >= 0 && now < up && up - now <= WEB_TOKEN_TTL_S)
            r = WEB_TOKEN_OK;
        goto out;
    }
    if (synced != 1)
        goto out;
    now = clock_s(CLOCK_REALTIME);
    if (!clock_trusted(now))
        r = WEB_TOKEN_CLOCK;
    else if (iat <= now && now < expires)
        r = WEB_TOKEN_OK;
out:
    json_free(v);
    return r;
}

web_token_result web_token_check(const mc_webcred *c, const char *token)
{
    unsigned char    payload[TOKEN_PAYLOAD_MAX];
    char             hdr[64], mac[TOKEN_MAC_B64 + 1];
    const char      *d1, *d2;
    size_t           n, hn;
    int              pn;
    web_token_result r;

    if (!c || !c->loaded || !token || (n = strnlen(token, WEB_TOKEN_MAX)) >= WEB_TOKEN_MAX)
        return WEB_TOKEN_INVALID;
    hn = b64url_encode(hdr, (const unsigned char *)TOKEN_HEADER, strlen(TOKEN_HEADER));
    if (n <= hn + 1 + TOKEN_MAC_B64 || memcmp(token, hdr, hn) || token[hn] != '.')
        return WEB_TOKEN_INVALID;
    d1 = token + hn;
    d2 = token + n - TOKEN_MAC_B64 - 1;
    if (*d2 != '.' || memchr(d1 + 1, '.', (size_t)(d2 - d1 - 1)) ||
            !token_mac(c, token, (size_t)(d2 - token), mac))
        return WEB_TOKEN_INVALID;
    r = CRYPTO_memcmp(mac, d2 + 1, TOKEN_MAC_B64) ? WEB_TOKEN_INVALID : WEB_TOKEN_OK;
    explicit_bzero(mac, sizeof mac);
    if (r != WEB_TOKEN_OK)
        return r;
    pn = b64url_decode(payload, sizeof payload - 1, d1 + 1, (size_t)(d2 - d1 - 1));
    if (pn <= 0 || memchr(payload, 0, (size_t)pn))
        return WEB_TOKEN_INVALID;
    payload[pn] = '\0';
    return payload_check(c, (const char *)payload);
}
