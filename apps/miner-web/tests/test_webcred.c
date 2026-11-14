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

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "mc_paths.h"
#include "osif_logpath.h"
#include "webcred.h"

#define TEST_MONO_BASE_S 1000 /* CLOCK_MONOTONIC the guard sees at offset 0, seconds */

static char    g_factory[] = "/tmp/webcred_factory_XXXXXX";
static int64_t g_mono_off_ms;
static int     g_load_calls;
static int     g_load_fail;                      /* non-zero makes mc_api_load_creds fail */
static char    g_user[MC_WEBUSER_MAX] = "admin"; /* user name mc_api_load_creds returns */

static int test_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id != CLOCK_MONOTONIC)
        return clock_gettime(id, ts);
    ts->tv_sec  = TEST_MONO_BASE_S + g_mono_off_ms / 1000;
    ts->tv_nsec = (long)(g_mono_off_ms % 1000) * 1000000;
    return 0;
}

#undef MC_SYSTEMCFG_FACTORY
#undef MINER_WEB_LOG_PATH
#define MC_SYSTEMCFG_FACTORY g_factory
#define MINER_WEB_LOG_PATH   "/dev/null"
#define clock_gettime        test_clock_gettime
#include "webcred.c"
#undef clock_gettime

int mc_api_load_creds(mc_webcred *c, const char *path)
{
    (void)path;
    g_load_calls++;
    if (g_load_fail) {
        c->loaded = 0;
        return -1;
    }
    c->loaded = 1;
    snprintf(c->user, sizeof c->user, "%s", g_user);
    snprintf(c->pass, sizeof c->pass, "deadbeef");
    return 0;
}

int mc_api_check(const mc_webcred *c, const char *user, const char *pass)
{
    return c->loaded && !strcmp(user, c->user) && !strcmp(pass, c->pass);
}

int mc_api_check_cookie(const mc_webcred *c, const char *cookie_auth)
{
    return c->loaded && !strcmp(cookie_auth, c->pass);
}

static void test_lockout(void)
{
    const char *A = "10.0.0.1";
    mc_webcred  c = { .user = "admin", .pass = "deadbeef", .loaded = 1 };

    for (int i = 0; i < 4; i++)
        assert(!web_auth_password(&c, "admin", "wrong", A));
    assert(web_auth_password(&c, "admin", "deadbeef", A));

    for (int i = 0; i < 5; i++)
        assert(!web_auth_login(&c, "wrong", A));
    assert(!web_auth_password(&c, "admin", "deadbeef", A));
    assert(!web_auth_login(&c, "deadbeef", A));
    g_mono_off_ms += 59000;
    assert(!web_auth_login(&c, "deadbeef", A));
    g_mono_off_ms += 1000;
    assert(web_auth_login(&c, "deadbeef", A));
    assert(web_auth_password(&c, "admin", "deadbeef", A));

    for (int i = 0; i < 4; i++)
        assert(!web_auth_password(&c, "admin", "wrong", A));
    g_mono_off_ms += 60000;
    assert(!web_auth_password(&c, "admin", "wrong", A));
    assert(web_auth_password(&c, "admin", "deadbeef", A));
}

static void test_lockout_per_addr(void)
{
    mc_webcred  c = { .user = "admin", .pass = "deadbeef", .loaded = 1 };
    const char *A = "192.168.1.10", *B = "192.168.1.11";
    char        addr[48];

    for (int i = 0; i < 5; i++)
        assert(!web_auth_login(&c, "wrong", A));
    assert(!web_auth_login(&c, "deadbeef", A));
    assert(!web_auth_password(&c, "admin", "deadbeef", A));
    assert(web_auth_login(&c, "deadbeef", B));
    assert(web_auth_password(&c, "admin", "deadbeef", B));
    g_mono_off_ms += 59999;
    assert(!web_auth_login(&c, "deadbeef", A));
    g_mono_off_ms += 1;
    assert(web_auth_login(&c, "deadbeef", A));

    for (int i = 0; i < 5; i++)
        assert(!web_auth_password(&c, "admin", "wrong", A));
    assert(!web_auth_password(&c, "admin", "deadbeef", A));
    for (int n = 0; n < WEB_AUTH_GUARD_N; n++) {
        g_mono_off_ms += 1;
        snprintf(addr, sizeof addr, "10.1.0.%d", n);
        assert(!web_auth_password(&c, "admin", "wrong", addr));
    }
    assert(!web_auth_password(&c, "admin", "deadbeef", A));
    g_mono_off_ms += 60000;
    assert(web_auth_password(&c, "admin", "deadbeef", A));
}

static void set_mtime(const char *p, time_t t)
{
    struct timeval tv[2] = { { t, 0 }, { t, 0 } };
    assert(utimes(p, tv) == 0);
}

static void test_proof_guard(void)
{
    mc_webcred  c = { .user = "admin", .pass = "deadbeef", .loaded = 1 };
    const char *P = "10.2.0.1", *Q = "10.2.0.2";

    for (int i = 0; i < 4; i++) {
        web_proof_guard_fail(P);
        assert(web_proof_guard_open(P));
    }
    web_proof_guard_fail(P);
    assert(!web_proof_guard_open(P));
    assert(web_proof_guard_open(Q));
    assert(web_auth_login(&c, "deadbeef", P));
    g_mono_off_ms += 59999;
    assert(!web_proof_guard_open(P));
    g_mono_off_ms += 1;
    assert(web_proof_guard_open(P));

    for (int i = 0; i < 4; i++)
        web_proof_guard_fail(P);
    g_mono_off_ms += 60000;
    web_proof_guard_fail(P);
    assert(web_proof_guard_open(P));

    for (int i = 0; i < 5; i++)
        assert(!web_auth_login(&c, "wrong", Q));
    assert(!web_auth_login(&c, "deadbeef", Q));
    assert(web_proof_guard_open(Q));
    g_mono_off_ms += 60000;
}

static void test_guard_full(void)
{
    mc_webcred  c = { .user = "admin", .pass = "deadbeef", .loaded = 1 };
    const char *N = "10.4.0.1";
    char        addr[48];

    for (int n = 0; n < WEB_AUTH_GUARD_N; n++) {
        snprintf(addr, sizeof addr, "10.3.0.%d", n);
        for (int i = 0; i < 5; i++) {
            assert(!web_auth_login(&c, "wrong", addr));
            web_proof_guard_fail(addr);
        }
    }
    assert(!web_auth_login(&c, "deadbeef", N));
    assert(!web_auth_password(&c, "admin", "deadbeef", N));
    assert(!web_proof_guard_open(N));
    g_mono_off_ms += 59999;
    for (int i = 0; i < 5; i++) {
        assert(!web_auth_login(&c, "wrong", N));
        web_proof_guard_fail(N);
    }
    assert(!web_auth_login(&c, "deadbeef", N));
    assert(!web_proof_guard_open(N));
    g_mono_off_ms += 1;
    assert(web_proof_guard_open(N));
    assert(web_auth_login(&c, "deadbeef", N));
    for (int n = 0; n < WEB_AUTH_GUARD_N; n++) {
        snprintf(addr, sizeof addr, "10.3.0.%d", n);
        assert(web_proof_guard_open(addr));
    }

    for (int i = 0; i < 5; i++)
        web_proof_guard_fail(N);
    for (int n = 0; n < WEB_AUTH_GUARD_N; n++) {
        g_mono_off_ms += 1;
        snprintf(addr, sizeof addr, "10.5.0.%d", n);
        web_proof_guard_fail(addr);
    }
    assert(!web_proof_guard_open(N));
    g_mono_off_ms += 60000;
    assert(web_proof_guard_open(N));
}

int main(void)
{
    char path[] = "/tmp/webcred_test_XXXXXX";
    int  fd     = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "x", 1) == 1);
    close(fd);
    set_mtime(path, 1000);

    mc_webcred c;

    assert(web_creds_load(path, &c) == 0);
    assert(g_load_calls == 1);
    assert(strcmp(c.user, "admin") == 0);

    assert(web_creds_load(path, &c) == 0);
    assert(g_load_calls == 1);

    set_mtime(path, 2000);
    snprintf(g_user, sizeof g_user, "%s", "root");
    assert(web_creds_load(path, &c) == 0);
    assert(g_load_calls == 2);
    assert(strcmp(c.user, "root") == 0);

    set_mtime(path, 3000);
    g_load_fail = 1;
    assert(web_creds_load(path, &c) == -1);
    assert(g_load_calls == 3);
    g_load_fail = 0;
    assert(web_creds_load(path, &c) == 0);
    assert(g_load_calls == 4);

    unlink(path);
    fd = mkstemp(g_factory);
    assert(fd >= 0);
    assert(write(fd, "[syscfg]\nwebpass = 0\n", 21) == 21);
    close(fd);
    assert(web_creds_load(path, &c) == 0);
    assert(g_load_calls == 5);
    char buf[64] = "";
    fd           = open(path, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, buf, sizeof buf - 1) == 21);
    close(fd);
    assert(strcmp(buf, "[syscfg]\nwebpass = 0\n") == 0);

    unlink(path);
    unlink(g_factory);
    struct stat st;
    web_creds_load(path, &c);
    assert(stat(path, &st) != 0);

    char lock[sizeof path + 5];
    snprintf(lock, sizeof lock, "%s.lock", path);
    unlink(lock);
    test_lockout();
    test_lockout_per_addr();
    test_proof_guard();
    test_guard_full();
    printf("webcred: all tests passed\n");
    return 0;
}
