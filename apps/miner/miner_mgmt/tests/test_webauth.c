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

#include "webauth.h"
#include <stdio.h>
#include <string.h>

#ifndef API_FIXTURES_DIR
/* Directory holding config/systemcfg.ini; default is the working directory */
#define API_FIXTURES_DIR "."
#endif

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

/* Web password matching the all-zero webpass in the systemcfg.ini fixture, 64 hex digits */
#define PASS64 "0000000000000000000000000000000000000000000000000000000000000000"

static int fails;

int main(void)
{
    mc_webcred c;
    int        rc = mc_api_load_creds(&c, API_FIXTURES_DIR "/config/systemcfg.ini");
    CHECK(rc == 0 && c.loaded, "load creds from systemcfg.ini");
    if (c.loaded) {
        CHECK(mc_api_check(&c, "admin", PASS64) == 1, "correct user+pass accepted");
        CHECK(mc_api_check(&c, "admin", "deadbeef") == 0, "wrong pass rejected");
        CHECK(mc_api_check(&c, "root", PASS64) == 0, "wrong user rejected");
        CHECK(mc_api_check(&c, "admin", "") == 0, "empty pass rejected");
        CHECK(mc_api_webpass_unset(&c) == 1, "all-zero webpass = unset");
        CHECK(mc_api_check_cookie(&c, "aabbccdd000000000000000000000000") == 0,
                "cookie rejected while webpass unset");
    }

    mc_webcred p = { .user = "admin", .loaded = 1 };
    strcpy(p.pass, "d30122d73dce746fd1ac33631c18bb75756c0a38ae69cae4187b931b6f3c2690");
    CHECK(mc_api_webpass_unset(&p) == 0, "provisioned webpass = set");
    CHECK(mc_api_check_cookie(&p, "4d43ac86d30122d73dce746fd1ac3363") == 1,
            "valid cookie (nonce+proof) accepted");
    CHECK(mc_api_check_cookie(&p, "ffffffffd30122d73dce746fd1ac3363") == 1,
            "nonce value is ignored (any 8-hex prefix)");
    CHECK(mc_api_check_cookie(&p, "4d43ac86D30122D73DCE746FD1AC3363") == 1,
            "proof compare is case-insensitive");
    CHECK(mc_api_check_cookie(&p, "4d43ac86deadbeefdeadbeefdeadbeef") == 0, "wrong proof rejected");
    CHECK(mc_api_check_cookie(&p, "d30122d73dce746fd1ac3363") == 0,
            "missing nonce (too short) rejected");
    CHECK(mc_api_check_cookie(&p, NULL) == 0, "NULL cookie rejected");

    if (fails) {
        fprintf(stderr, "test_webauth: %d FAIL\n", fails);
        return 1;
    }
    printf("test_webauth: all pass\n");
    return 0;
}
