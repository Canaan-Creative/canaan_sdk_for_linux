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

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <time.h>

static time_t test_now = 100; /* Simulated monotonic clock returned by test_clock(), s */

static int test_clock(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec  = test_now;
    ts->tv_nsec = 0;
    return 0;
}
#define clock_gettime test_clock
#include "../api/mc_api_qrlogin.c"
#undef clock_gettime
int main(void)
{
    const char *a = "11111111aaaaaaaaaaaaaaaaaaaaaaaa";
    const char *b = "22222222bbbbbbbbbbbbbbbbbbbbbbbb";
    assert(mc_api_qr_login_begin(a));
    assert(mc_api_qr_login_begin(b));
    assert(!mc_api_qr_login_begin("11111111cccccccccccccccccccccccc"));
    assert(!mc_api_qr_login_approve("deadbeef"));
    assert(mc_api_qr_login_approve("11111111"));
    assert(!mc_api_qr_login_take(b));
    assert(!mc_api_qr_login_take("11111111cccccccccccccccccccccccc"));
    assert(mc_api_qr_login_take(a));
    assert(!mc_api_qr_login_take(a));
    assert(!mc_api_qr_login_approve("11111111"));
    assert(mc_api_qr_login_approve("22222222"));
    test_now += 300;
    assert(!mc_api_qr_login_take(b));
    assert(!mc_api_qr_login_pending("22222222"));
    for (int i = 0; i < QR_SESSIONS; i++) {
        char token[33];
        for (int j = 0; j < 32; j++)
            token[j] = "0123456789abcdef"[i];
        token[32] = 0;
        assert(mc_api_qr_login_begin(token));
    }
    assert(!mc_api_qr_login_begin(a));
    assert(mc_api_qr_login_approve("33333333"));
    test_now += 1;
    assert(mc_api_qr_login_begin("01234567aaaaaaaaaaaaaaaaaaaaaaaa"));
    assert(mc_api_qr_login_pending("01234567"));
    assert(!mc_api_qr_login_pending("00000000"));
    assert(mc_api_qr_login_pending("11111111"));
    for (int i = 0; i < QR_SESSIONS; i++) {
        char token[33];
        snprintf(token, sizeof token, "%08x%s", 0xa0000000u + i, "bbbbbbbbbbbbbbbbbbbbbbbb");
        assert(mc_api_qr_login_begin(token));
    }
    assert(mc_api_qr_login_pending("33333333"));
    assert(mc_api_qr_login_take("33333333333333333333333333333333"));
    return 0;
}
