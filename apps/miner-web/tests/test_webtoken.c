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
#include "osif_logpath.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#define T_WALL   1800000000LL /* 2027-01-15, inside the trusted range */
#define T_UP     1000LL
#define T_HEADER "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
/* Counts a failed cond in fails and prints msg as FAIL or ok */
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static char      g_dir[] = "/tmp/test_webtoken_XXXXXX";
static char      g_key_path[PATH_MAX];
static char      g_key_tmp_path[PATH_MAX];
static char      g_boot_path[PATH_MAX];
static long long g_wall   = T_WALL;
static long long g_up     = T_UP;
static int       g_synced = 1;
static uid_t     g_uid_skew;
static int       fails;

static int test_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id == CLOCK_REALTIME || id == CLOCK_BOOTTIME) {
        ts->tv_sec  = (time_t)(id == CLOCK_REALTIME ? g_wall : g_up);
        ts->tv_nsec = 0;
        return 0;
    }
    return clock_gettime(id, ts);
}

static int test_adjtimex(struct timex *tx)
{
    (void)tx;
    return g_synced ? TIME_OK : TIME_ERROR;
}

static uid_t test_geteuid(void)
{
    return geteuid() + g_uid_skew;
}

#undef WEB_TOKEN_KEY
#undef WEB_TOKEN_KEY_TMP
#undef WEB_TOKEN_BOOT_ID
#undef MINER_WEB_LOG_PATH
#define WEB_TOKEN_KEY      g_key_path
#define WEB_TOKEN_KEY_TMP  g_key_tmp_path
#define WEB_TOKEN_BOOT_ID  g_boot_path
#define MINER_WEB_LOG_PATH "/dev/null"
#define clock_gettime      test_clock_gettime
#define adjtimex           test_adjtimex
#define geteuid            test_geteuid
#undef _DEFAULT_SOURCE
#include "webtoken.c"
#undef geteuid
#undef adjtimex
#undef clock_gettime

static mc_webcred creds(const char *user, const char *pass)
{
    mc_webcred c;

    memset(&c, 0, sizeof c);
    snprintf(c.user, sizeof c.user, "%s", user);
    snprintf(c.pass, sizeof c.pass, "%s", pass);
    c.loaded = 1;
    return c;
}

static void boot_set(const char *id)
{
    FILE *f = fopen(g_boot_path, "w");

    if (f) {
        fprintf(f, "%s\n", id);
        fclose(f);
    }
}

static void clocks_reset(void)
{
    g_wall   = T_WALL;
    g_up     = T_UP;
    g_synced = 1;
    boot_set("6f1c2d3e-0000-4000-8000-000000000001");
}

static void key_forget(void)
{
    explicit_bzero(g_key, sizeof g_key);
    explicit_bzero(g_fp, sizeof g_fp);
    g_key_ok = 0;
}

static int file_get(const char *path, unsigned char *out)
{
    FILE  *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return 0;
    n = fread(out, 1, TOKEN_KEY_FILE + 1, f);
    fclose(f);
    return n == TOKEN_KEY_FILE;
}

static int holds(const unsigned char *data, size_t n, const char *str)
{
    size_t sn = strlen(str);

    for (size_t i = 0; sn && i + sn <= n; i++)
        if (!memcmp(data + i, str, sn))
            return 1;
    return 0;
}

static void file_put(const char *path, const unsigned char *data, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static long long file_size(const char *path, mode_t *mode)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    if (mode)
        *mode = st.st_mode & 07777;
    return (long long)st.st_size;
}

static int payload_of(const char *token, char *out, size_t cap)
{
    const char *d1 = strchr(token, '.');
    const char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    int         n;

    if (!d2)
        return -1;
    n = b64url_decode((unsigned char *)out, cap - 1, d1 + 1, (size_t)(d2 - d1 - 1));
    if (n < 0)
        return -1;
    out[n] = '\0';
    return 0;
}

static void forge(const mc_webcred *c, const char *header, const char *payload, char *out)
{
    size_t n = b64url_encode(out, (const unsigned char *)header, strlen(header));

    out[n++] = '.';
    n += b64url_encode(out + n, (const unsigned char *)payload, strlen(payload));
    out[n] = '.';
    if (!token_mac(c, out, n, out + n + 1))
        out[n + 1] = '\0';
}

static void test_key_file(void)
{
    mc_webcred c = creds("admin", "aa");
    char       t[WEB_TOKEN_MAX];
    mode_t     mode = 0;
    FILE      *f;

    clocks_reset();
    key_forget();
    unlink(g_key_path);
    CHECK(web_token_issue(&c, t, sizeof t) == 0, "a token is issued with no key file yet");
    CHECK(file_size(g_key_path, &mode) == TOKEN_KEY_FILE && mode == 0600,
            "the first token creates a 64-byte key file, mode 0600");
    key_forget();
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK,
            "the key is read back from the file: a token survives a miner_web restart");

    f = fopen(g_key_path, "w");
    if (f) {
        fputs("short", f);
        fclose(f);
    }
    key_forget();
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "a key file of the wrong length is replaced: older tokens are refused");
    CHECK(file_size(g_key_path, &mode) == TOKEN_KEY_FILE && mode == 0600,
            "the replaced key file holds 64 bytes, mode 0600");
    CHECK(web_token_issue(&c, t, sizeof t) == 0 && web_token_check(&c, t) == WEB_TOKEN_OK,
            "a token signed with the new key is accepted");

    snprintf(g_key_path, sizeof g_key_path, "%s/absent/key", g_dir);
    key_forget();
    unlink(g_key_tmp_path);
    CHECK(web_token_issue(&c, t, sizeof t) == 0 &&
                    file_size(g_key_tmp_path, &mode) == TOKEN_KEY_FILE && mode == 0600,
            "when the key cannot be kept, the fallback key file is made, mode 0600");
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK, "a token signed with the fallback key passes");
    snprintf(g_key_tmp_path, sizeof g_key_tmp_path, "%s/absent/tmpkey", g_dir);
    key_forget();
    CHECK(web_token_issue(&c, t, sizeof t) == -1 && t[0] == '\0',
            "no token is issued when no key file can be made");
    CHECK(web_token_check(&c, T_HEADER ".e30.AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") ==
                    WEB_TOKEN_INVALID,
            "no token is accepted when no key file can be made");
    snprintf(g_key_path, sizeof g_key_path, "%s/key", g_dir);
    snprintf(g_key_tmp_path, sizeof g_key_tmp_path, "%s/tmpkey", g_dir);
    key_forget();
}

static void test_issue_shape(void)
{
    mc_webcred c = creds("admin", "aa");
    char       t[WEB_TOKEN_MAX], p[WEB_TOKEN_MAX], want[256];

    clocks_reset();
    CHECK(web_token_issue(&c, t, sizeof t) == 0, "a token is issued");
    CHECK(!strncmp(t, T_HEADER ".", strlen(T_HEADER) + 1),
            "the header is base64url of {\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    CHECK(strlen(strrchr(t, '.') + 1) == TOKEN_MAC_B64 && !strchr(t, '='),
            "the signature is 43 base64url characters, no padding");
    snprintf(want, sizeof want,
            "{\"User\":\"admin\",\"Iat\":%lld,\"Expiration\":%lld,"
            "\"Boot\":\"6f1c2d3e-0000-4000-8000-000000000001\",\"Up\":%lld,\"Synced\":1}",
            T_WALL, T_WALL + WEB_TOKEN_TTL_S, T_UP + WEB_TOKEN_TTL_S);
    CHECK(payload_of(t, p, sizeof p) == 0 && !strcmp(p, want),
            "the payload carries User, Iat, Expiration, Boot, Up and Synced");
    CHECK(!strstr(p, c.pass), "the payload does not carry the password digest");
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK, "the token is accepted");

    g_synced = 0;
    CHECK(web_token_issue(&c, t, sizeof t) == 0 && payload_of(t, p, sizeof p) == 0 &&
                    strstr(p, "\"Synced\":0}"),
            "an unsynchronized clock gives Synced 0");
    g_synced = 1;
    g_wall   = TOKEN_WALL_MIN - 1;
    CHECK(web_token_issue(&c, t, sizeof t) == 0 && payload_of(t, p, sizeof p) == 0 &&
                    strstr(p, "\"Synced\":0}"),
            "a wall clock before 2026 gives Synced 0");
    g_wall = TOKEN_WALL_MAX;
    CHECK(web_token_issue(&c, t, sizeof t) == 0 && payload_of(t, p, sizeof p) == 0 &&
                    strstr(p, "\"Synced\":0}"),
            "a wall clock from 2100 gives Synced 0");

    clocks_reset();
    CHECK(web_token_issue(&c, t, WEB_TOKEN_MAX - 1) == -1, "a short output buffer is refused");
    c.loaded = 0;
    CHECK(web_token_issue(&c, t, sizeof t) == -1, "no token without loaded credentials");
    CHECK(web_token_issue(NULL, t, sizeof t) == -1, "no token without credentials");
    c = creds("admin", "aa");
    unlink(g_boot_path);
    CHECK(web_token_issue(&c, t, sizeof t) == -1, "no token without a boot id");
    clocks_reset();
}

static void test_forged(void)
{
    mc_webcred c = creds("admin", "aa");
    char       t[WEB_TOKEN_MAX], f[WEB_TOKEN_MAX + 1], pl[512], longt[WEB_TOKEN_MAX + 8];
    char      *seg;

    clocks_reset();
    web_token_issue(&c, t, sizeof t);
    snprintf(f, sizeof f, "%s", t);
    seg  = strchr(f, '.') + 3;
    *seg = *seg == 'A' ? 'B' : 'A';
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a changed payload is refused");
    snprintf(f, sizeof f, "%s", t);
    seg  = strrchr(f, '.') + 5;
    *seg = *seg == 'A' ? 'B' : 'A';
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a changed signature is refused");
    snprintf(f, sizeof f, "%s", t);
    f[strlen(f) - 1] = '\0';
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a short signature is refused");
    snprintf(f, sizeof f, "%sA", t);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a long signature is refused");
    snprintf(f, sizeof f, "%s.", t);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a fourth segment is refused");
    snprintf(f, sizeof f, "%.*s", (int)(strrchr(t, '.') - t), t);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a token without signature is refused");

    payload_of(t, pl, sizeof pl);
    forge(&c, "{\"alg\":\"none\",\"typ\":\"JWT\"}", pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "alg none is refused, even signed");
    seg = f + snprintf(f, sizeof f, "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.");
    seg += b64url_encode(seg, (const unsigned char *)pl, strlen(pl));
    snprintf(seg, (size_t)(f + sizeof f - seg), ".");
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "an unsigned alg none token is refused");
    forge(&c, "{\"typ\":\"JWT\",\"alg\":\"HS256\"}", pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID,
            "a header other than the fixed one is refused, even signed");
    forge(&c, "{\"alg\":\"HS256\",\"typ\":\"JWT\"} ", pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a header with a trailing byte is refused");
    forge(&c, TOKEN_HEADER, pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_OK, "the forge helper signs a valid token");

    snprintf(pl, sizeof pl,
            "{\"User\":\"admin\",\"Iat\":%lld,\"Expiration\":%lld,\"Boot\":\"x\",\"Up\":%lld,"
            "\"Synced\":1}",
            T_WALL, T_WALL + WEB_TOKEN_TTL_S + 1, T_UP + 10);
    forge(&c, TOKEN_HEADER, pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID,
            "a signed token whose Expiration is not Iat + 24 h is refused");
    snprintf(pl, sizeof pl,
            "{\"User\":\"root\",\"Iat\":%lld,\"Expiration\":%lld,"
            "\"Boot\":\"6f1c2d3e-0000-4000-8000-000000000001\",\"Up\":%lld,\"Synced\":1}",
            T_WALL, T_WALL + WEB_TOKEN_TTL_S, T_UP + 10);
    forge(&c, TOKEN_HEADER, pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID,
            "a signed token for another User is refused");
    snprintf(pl, sizeof pl,
            "{\"User\":\"admin\",\"Iat\":%lld,\"Expiration\":%lld,"
            "\"Boot\":\"6f1c2d3e-0000-4000-8000-000000000001\",\"Up\":%lld,\"Synced\":1}",
            T_WALL, T_WALL + WEB_TOKEN_TTL_S, T_UP + WEB_TOKEN_TTL_S + 1);
    forge(&c, TOKEN_HEADER, pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID,
            "a signed token whose Up is more than 24 h ahead is refused");
    snprintf(pl, sizeof pl,
            "{\"User\":\"admin\",\"Iat\":%lld,\"Expiration\":%lld,"
            "\"Boot\":\"6f1c2d3e-0000-4000-8000-000000000001\",\"Up\":%lld}",
            T_WALL, T_WALL + WEB_TOKEN_TTL_S, T_UP + 10);
    forge(&c, TOKEN_HEADER, pl, f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a signed token without Synced is refused");
    forge(&c, TOKEN_HEADER, "[1]", f);
    CHECK(web_token_check(&c, f) == WEB_TOKEN_INVALID, "a signed non-object payload is refused");

    memset(longt, 'A', sizeof longt - 1);
    longt[sizeof longt - 1] = '\0';
    CHECK(web_token_check(&c, longt) == WEB_TOKEN_INVALID, "an over-long token is refused");
    CHECK(web_token_check(&c, "") == WEB_TOKEN_INVALID, "an empty token is refused");
    CHECK(web_token_check(&c, "a.b.c") == WEB_TOKEN_INVALID, "a short token is refused");
    CHECK(web_token_check(&c, NULL) == WEB_TOKEN_INVALID, "no token is refused");
    CHECK(web_token_check(NULL, t) == WEB_TOKEN_INVALID, "no credentials accept no token");
}

static void test_same_boot(void)
{
    mc_webcred c = creds("admin", "aa");
    char       t[WEB_TOKEN_MAX];

    clocks_reset();
    web_token_issue(&c, t, sizeof t);
    g_up = T_UP + WEB_TOKEN_TTL_S - 1;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK, "same boot: accepted 1 s before 24 h uptime");
    g_wall   = 0;
    g_synced = 0;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK,
            "same boot: the wall clock and its sync state do not matter");
    g_wall = T_WALL + 10 * WEB_TOKEN_TTL_S;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK,
            "same boot: a wall clock set ahead does not expire the token");
    g_up = T_UP + WEB_TOKEN_TTL_S;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID, "same boot: refused after 24 h of uptime");
    clocks_reset();
}

static void test_other_boot(void)
{
    mc_webcred c = creds("admin", "aa");
    char       t[WEB_TOKEN_MAX], u[WEB_TOKEN_MAX];

    clocks_reset();
    web_token_issue(&c, t, sizeof t);
    g_synced = 0;
    web_token_issue(&c, u, sizeof u);
    g_synced = 1;

    boot_set("6f1c2d3e-0000-4000-8000-000000000002");
    g_up   = 5;
    g_wall = T_WALL + 100;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK,
            "after a reboot a synced token is accepted within 24 h of wall time");
    CHECK(web_token_check(&c, u) == WEB_TOKEN_INVALID,
            "after a reboot a token issued with Synced 0 is refused");
    g_wall = T_WALL + WEB_TOKEN_TTL_S - 1;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_OK, "after a reboot: accepted 1 s before expiry");
    g_wall = T_WALL + WEB_TOKEN_TTL_S;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID, "after a reboot: refused at Expiration");
    g_wall = T_WALL - 1;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "after a reboot: refused when the wall clock is before Iat");

    g_wall   = T_WALL + 100;
    g_synced = 0;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_CLOCK,
            "after a reboot with the clock not synchronized: clock not ready");
    CHECK(web_token_check(&c, u) == WEB_TOKEN_INVALID,
            "...but a Synced 0 token is refused outright");
    g_synced = 1;
    g_wall   = 100;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_CLOCK,
            "after a reboot with a wall clock before 2026: clock not ready");
    g_wall = TOKEN_WALL_MAX;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_CLOCK,
            "after a reboot with a wall clock from 2100: clock not ready");
    unlink(g_boot_path);
    g_wall = T_WALL + 100;
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID, "no boot id: refused");
    clocks_reset();
}

static void test_cred_change(void)
{
    mc_webcred c = creds("admin", "aa");
    mc_webcred p = creds("admin", "bb");
    mc_webcred u = creds("root", "aa");
    char       t[WEB_TOKEN_MAX], v[WEB_TOKEN_MAX];

    clocks_reset();
    web_token_issue(&c, t, sizeof t);
    web_token_issue(&c, v, sizeof v);
    CHECK(web_token_check(&p, t) == WEB_TOKEN_INVALID &&
                    web_token_check(&p, v) == WEB_TOKEN_INVALID,
            "a password change refuses every token");
    CHECK(web_token_check(&u, t) == WEB_TOKEN_INVALID &&
                    web_token_check(&u, v) == WEB_TOKEN_INVALID,
            "a user name change refuses every token");
    CHECK(web_token_issue(&p, v, sizeof v) == 0 && web_token_check(&p, v) == WEB_TOKEN_OK,
            "a token issued for the new password is accepted");
}

static void test_key_renew(void)
{
    mc_webcred    c    = creds("admin", "0123456789abcdef0123456789abcdef");
    mc_webcred    p    = creds("admin", "fedcba9876543210fedcba9876543210");
    mc_webcred    same = creds("admin", "0123456789abcdef0123456789abcdef");
    unsigned char a[TOKEN_KEY_FILE + 1], b[TOKEN_KEY_FILE + 1], fp[TOKEN_MAC_LEN];
    unsigned char k[TOKEN_MAC_LEN], mac[TOKEN_MAC_LEN];
    char          t[WEB_TOKEN_MAX], v[WEB_TOKEN_MAX], sig[TOKEN_MAC_B64 + 1];
    const char   *dot;
    unsigned int  ml   = 0;
    mode_t        mode = 0;

    clocks_reset();
    key_forget();
    unlink(g_key_path);
    CHECK(web_token_issue(&c, t, sizeof t) == 0 && file_get(g_key_path, a),
            "a token is issued and the key file is written");
    CHECK(cred_mac(a, cred_tag_fp, &c, fp) && !memcmp(a + WEB_TOKEN_KEY_LEN, fp, sizeof fp),
            "the key file holds the key and the fingerprint of the credentials");
    dot = strrchr(t, '.');
    CHECK(cred_mac(a, cred_tag_token, &c, k) && dot &&
                    HMAC(EVP_sha256(), k, sizeof k, (const unsigned char *)t, (size_t)(dot - t),
                            mac, &ml) &&
                    ml == TOKEN_MAC_LEN && b64url_encode(sig, mac, sizeof mac) == TOKEN_MAC_B64 &&
                    !strcmp(dot + 1, sig),
            "the token is signed with the key derived for tokens");
    CHECK(memcmp(a + WEB_TOKEN_KEY_LEN, k, sizeof k),
            "the fingerprint in the key file differs from the token signing key");
    CHECK(!holds(a, TOKEN_KEY_FILE, c.pass) && !holds(a, TOKEN_KEY_FILE, c.user),
            "the key file holds neither the user name nor the password digest");

    CHECK(web_token_check(&same, t) == WEB_TOKEN_OK,
            "setting the same password keeps an issued token");
    CHECK(file_get(g_key_path, b) && !memcmp(a, b, TOKEN_KEY_FILE),
            "setting the same password keeps the key file");

    CHECK(web_token_issue(&p, v, sizeof v) == 0 && web_token_check(&p, t) == WEB_TOKEN_INVALID,
            "a password change refuses an issued token");
    CHECK(file_get(g_key_path, b) && memcmp(a, b, WEB_TOKEN_KEY_LEN) &&
                    file_size(g_key_path, &mode) == TOKEN_KEY_FILE && mode == 0600,
            "a password change writes a new key file, mode 0600");
    CHECK(web_token_check(&p, v) == WEB_TOKEN_OK, "a token for the new password is accepted");
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "changing the password back still refuses the token issued before");
    CHECK(web_token_check(&p, v) == WEB_TOKEN_INVALID,
            "changing the password back refuses the token for the other password");
    CHECK(file_get(g_key_path, a) && memcmp(a, b, WEB_TOKEN_KEY_LEN),
            "changing the password back writes a new key file");

    CHECK(web_token_issue(&c, t, sizeof t) == 0, "a token is issued before a restart");
    key_forget();
    CHECK(web_token_check(&p, t) == WEB_TOKEN_INVALID,
            "a password changed while miner_web was stopped refuses an issued token");
    key_forget();
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "...and the token stays refused after changing the password back");

    CHECK(web_token_issue(&c, t, sizeof t) == 0 && file_get(g_key_path, a),
            "a token is issued with the current key file");
    file_put(g_key_path, a, WEB_TOKEN_KEY_LEN);
    key_forget();
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "a 32-byte key file is replaced: tokens signed with its key are refused");
    CHECK(file_size(g_key_path, &mode) == TOKEN_KEY_FILE && mode == 0600 &&
                    file_get(g_key_path, b) && memcmp(a, b, WEB_TOKEN_KEY_LEN),
            "the 32-byte key file is rewritten with a new key, 64 bytes, mode 0600");

    snprintf(g_key_path, sizeof g_key_path, "%s/absent/key", g_dir);
    key_forget();
    unlink(g_key_tmp_path);
    CHECK(web_token_issue(&c, t, sizeof t) == 0 && file_get(g_key_tmp_path, a),
            "the fallback key file is written when the key cannot be kept");
    CHECK(web_token_check(&p, t) == WEB_TOKEN_INVALID && file_get(g_key_tmp_path, b) &&
                    memcmp(a, b, WEB_TOKEN_KEY_LEN) &&
                    file_size(g_key_tmp_path, &mode) == TOKEN_KEY_FILE && mode == 0600,
            "a password change rewrites the fallback key file, mode 0600");
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "changing the password back refuses the token signed with the old fallback key");
    snprintf(g_key_path, sizeof g_key_path, "%s/key", g_dir);
    unlink(g_key_tmp_path);
    key_forget();
}

static void test_key_owner(void)
{
    mc_webcred    c = creds("admin", "0123456789abcdef0123456789abcdef");
    unsigned char a[TOKEN_KEY_FILE + 1], b[TOKEN_KEY_FILE + 1];
    char          t[WEB_TOKEN_MAX];
    mode_t        mode = 0;

    clocks_reset();
    key_forget();
    unlink(g_key_path);
    CHECK(web_token_issue(&c, t, sizeof t) == 0 && file_get(g_key_path, a),
            "a token is issued and the key file is written");
    chmod(g_key_path, 0644);
    key_forget();
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "a key file readable by others is replaced: tokens signed with its key are refused");
    CHECK(file_size(g_key_path, &mode) == TOKEN_KEY_FILE && mode == 0600 &&
                    file_get(g_key_path, b) && memcmp(a, b, WEB_TOKEN_KEY_LEN),
            "the key file readable by others is rewritten with a new key, mode 0600");

    CHECK(web_token_issue(&c, t, sizeof t) == 0 && file_get(g_key_path, a),
            "a token is issued with the rewritten key file");
    g_uid_skew = 1;
    key_forget();
    CHECK(web_token_check(&c, t) == WEB_TOKEN_INVALID,
            "a key file owned by another user is replaced: tokens signed with its key are refused");
    CHECK(file_get(g_key_path, b) && memcmp(a, b, WEB_TOKEN_KEY_LEN),
            "the key file owned by another user is rewritten with a new key");
    g_uid_skew = 0;
    key_forget();
    unlink(g_key_path);
}

int main(void)
{
    if (!mkdtemp(g_dir)) {
        fprintf(stderr, "  FAIL: cannot create %s\n", g_dir);
        return 1;
    }
    snprintf(g_key_path, sizeof g_key_path, "%s/key", g_dir);
    snprintf(g_key_tmp_path, sizeof g_key_tmp_path, "%s/tmpkey", g_dir);
    snprintf(g_boot_path, sizeof g_boot_path, "%s/boot_id", g_dir);

    test_key_file();
    test_issue_shape();
    test_forged();
    test_same_boot();
    test_other_boot();
    test_cred_change();
    test_key_renew();
    test_key_owner();

    unlink(g_key_path);
    unlink(g_key_tmp_path);
    unlink(g_boot_path);
    rmdir(g_dir);
    if (fails) {
        fprintf(stderr, "webtoken: %d check(s) failed\n", fails);
        return 1;
    }
    printf("webtoken: all tests passed\n");
    return 0;
}
