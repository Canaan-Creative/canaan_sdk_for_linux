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

#include "sha256.h"
#include <stdio.h>
#include <string.h>

static int fails;

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

static void check(const char *name, const uint8_t *got, const char *want_hex)
{
    uint8_t want[SHA256_DIGEST_SIZE];
    if (unhex(want_hex, want, sizeof want) != SHA256_DIGEST_SIZE) {
        fprintf(stderr, "  %-16s bad expected vector\n", name);
        fails++;
        return;
    }
    if (memcmp(got, want, SHA256_DIGEST_SIZE) != 0) {
        fprintf(stderr, "  %-16s MISMATCH\n", name);
        fails++;
        return;
    }
    printf("  %-16s ok\n", name);
}

int main(void)
{
    uint8_t d[SHA256_DIGEST_SIZE];

    sha256("", 0, d);
    check("sha256(empty)", d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha256("abc", 3, d);
    check("sha256(abc)", d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    check("sha256(2-block)", d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    sha256d("", 0, d);
    check("sha256d(empty)", d, "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");
    sha256d("abc", 3, d);
    check("sha256d(abc)", d, "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");

    uint8_t hdr[80];
    if (unhex("01000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
              "29ab5f49ffff001d1dac2b7c",
                hdr, sizeof hdr) != 80) {
        fprintf(stderr, "  genesis          bad header vector\n");
        fails++;
    } else {
        sha256d(hdr, sizeof hdr, d);
        check("sha256d(genesis)", d,
                "6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000");
    }

    {
        const char *msg = "The quick brown fox jumps over the lazy dog";
        uint8_t     one[SHA256_DIGEST_SIZE], strm[SHA256_DIGEST_SIZE];
        sha256(msg, strlen(msg), one);
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, msg, 10);
        sha256_update(&c, msg + 10, strlen(msg) - 10);
        sha256_final(&c, strm);
        if (memcmp(one, strm, SHA256_DIGEST_SIZE) != 0) {
            fprintf(stderr, "  streaming        MISMATCH vs one-shot\n");
            fails++;
        } else {
            printf("  %-16s ok\n", "streaming");
        }
    }

    if (fails) {
        fprintf(stderr, "test_sha256: %d FAIL\n", fails);
        return 1;
    }
    printf("test_sha256: all pass\n");
    return 0;
}
