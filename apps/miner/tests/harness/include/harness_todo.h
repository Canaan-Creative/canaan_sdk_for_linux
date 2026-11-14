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

#ifndef HARNESS_TODO_H
#define HARNESS_TODO_H

#include <stdio.h>
#include <string.h>

/*
 * Dispatcher fallback: an unknown case id is a hard error, so a typo in the
 * CMake registration fails loudly.
 */
#define HARNESS_UNKNOWN(id) \
    do { \
        fprintf(stderr, "harness: unknown case id '%s'\n", (id)); \
        return 2; \
    } while (0)

#endif /* HARNESS_TODO_H */
