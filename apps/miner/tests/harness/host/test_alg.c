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

#include <stdio.h>
#include <string.h>
#include "harness_todo.h"
#include "sha256_ref.h"
#include "sha256.h"

static int unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > cap)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

static int kat(const char *name, const uint8_t *msg, size_t len, const char *expect_hex)
{
    uint8_t want[SHA256_DIGEST_LEN], oracle[SHA256_DIGEST_LEN], prod[SHA256_DIGEST_LEN];
    if (unhex(expect_hex, want, sizeof want) != SHA256_DIGEST_LEN) {
        fprintf(stderr, "  %-8s bad expected vector\n", name);
        return 1;
    }
    sha256d_ref(msg, len, oracle);
    sha256d(msg, len, prod);
    if (memcmp(oracle, want, SHA256_DIGEST_LEN) != 0) {
        fprintf(stderr, "  %-8s oracle MISMATCH\n", name);
        return 1;
    }
    if (memcmp(prod, want, SHA256_DIGEST_LEN) != 0) {
        fprintf(stderr, "  %-8s common/sha MISMATCH\n", name);
        return 1;
    }
    printf("  %-8s ok\n", name);
    return 0;
}

static int test_sha256d(void)
{
    int     rc = 0;
    uint8_t hdr[80];

    rc |= kat("empty", (const uint8_t *)"", 0,
            "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");
    rc |= kat("abc", (const uint8_t *)"abc", 3,
            "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");

    if (unhex("01000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
              "29ab5f49ffff001d1dac2b7c",
                hdr, sizeof hdr) != 80) {
        fprintf(stderr, "  genesis  bad header vector\n");
        return 1;
    }
    rc |= kat("genesis", hdr, sizeof hdr,
            "6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000");
    return rc;
}

int main(int argc, char **argv)
{
    const char *id = argc > 1 ? argv[1] : "";

    if (!strcmp(id, "sha256d"))
        return test_sha256d();

    HARNESS_UNKNOWN(id);
}
