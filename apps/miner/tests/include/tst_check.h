/* SPDX-License-Identifier: MIT */
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

#ifndef TST_CHECK_H
#define TST_CHECK_H

#include <stdio.h>

/*
 * Assertion helper shared by the host test suites. A failure prints
 * "  FAIL: " to stderr, which test/scripts/host_tests.sh greps for; a pass
 * prints "  ok: " to stdout. main() ends with "return tst_done();".
 */
#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "  FAIL: " __VA_ARGS__); \
            fprintf(stderr, "\n"); \
            fails++; \
        } else { \
            printf("  ok: " __VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static int fails;

static inline int tst_done(void)
{
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}

#endif /* TST_CHECK_H */
