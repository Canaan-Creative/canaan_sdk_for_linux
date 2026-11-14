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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AUP_PKG_GOOD) || !defined(AUP_PKG_BAD)
#error "AUP_PKG_GOOD and AUP_PKG_BAD must name the gen_aup.py packages this test verifies"
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

static unsigned char *load(const char *path, size_t *len)
{
    FILE          *f = fopen(path, "rb");
    unsigned char *b;
    long           n;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
            (b = malloc((size_t)n + 1)) == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int pkg_verify(const unsigned char *buf, size_t len)
{
    struct aup_hdr h;
    int            rc = aup_hdr_parse(buf, len, &h);

    if (rc != 0)
        return rc;
    return aup_verify_signature(&h);
}

static void check_flip(unsigned char *buf, size_t len, size_t off, int accept)
{
    buf[off] ^= 0xffu;
    CHECK((pkg_verify(buf, len) == 0) == accept, "flipping byte %zu %s", off,
            accept ? "still verifies" : "is rejected");
    buf[off] ^= 0xffu;
}

static void test_good(void)
{
    unsigned char *buf;
    size_t         len = 0;

    if (AUP_PKG_GOOD[0] == '\0') {
        printf("  SKIP: no package signed with the key this verifier embeds\n");
        skips++;
        return;
    }
    buf = load(AUP_PKG_GOOD, &len);
    CHECK(buf != NULL && len >= AUP_HDR_SIZE, "%s loads (%zu bytes)", AUP_PKG_GOOD, len);
    if (buf == NULL || len < AUP_HDR_SIZE) {
        free(buf);
        return;
    }
    CHECK(pkg_verify(buf, len) == 0,
            "a package gen_aup.py signed with the key this verifier embeds verifies");
    check_flip(buf, len, AUP_SIGNED_LEN - 1, 0);
    check_flip(buf, len, sizeof(struct aup_hdr) - 1, 0);
    check_flip(buf, len, sizeof(struct aup_hdr), 1);
    CHECK(pkg_verify(buf, len) == 0, "the restored package verifies again");
    free(buf);
}

static void test_bad(void)
{
    unsigned char *buf;
    size_t         len = 0;

    if (AUP_PKG_BAD[0] == '\0') {
        printf("  SKIP: no package signed with another key\n");
        skips++;
        return;
    }
    buf = load(AUP_PKG_BAD, &len);
    CHECK(buf != NULL && len >= AUP_HDR_SIZE, "%s loads (%zu bytes)", AUP_PKG_BAD, len);
    if (buf == NULL || len < AUP_HDR_SIZE) {
        free(buf);
        return;
    }
    CHECK(pkg_verify(buf, len) == UPGRADE_VERIFY_ERR,
            "a package gen_aup.py signed with another key parses and is rejected "
            "with UPGRADE_VERIFY_ERR");
    free(buf);
}

int main(void)
{
    printf("test_aup_package: gen_aup.py packages through aup_hdr_parse and "
           "aup_verify_signature\n");

    test_good();
    test_bad();

    printf(fails ? "test_aup_package: FAILED (%d)\n" : "test_aup_package: all pass\n", fails);
    return fails ? 1 : skips ? TST_SKIP : 0;
}
