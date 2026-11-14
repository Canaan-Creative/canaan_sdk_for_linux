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

#ifndef HARNESS_FIXTURE_H
#define HARNESS_FIXTURE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HARNESS_FIXTURES_DIR
#define HARNESS_FIXTURES_DIR "fixtures"
#endif

#define HF_CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            hf_fails++; \
            printf("  FAIL: "); \
            printf(__VA_ARGS__); \
            printf("  [%s:%d]\n", __FILE__, __LINE__); \
        } \
    } while (0)

/* Prints the result for case id and returns 1 if any HF_CHECK failed, else 0 */
#define HF_DONE(id) \
    do { \
        if (hf_fails) { \
            printf("%s: %d check(s) failed\n", (id), hf_fails); \
            return 1; \
        } \
        printf("%s: OK\n", (id)); \
        return 0; \
    } while (0)

static int hf_fails;

__attribute__((unused)) static char *hf_read(const char *rel)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", HARNESS_FIXTURES_DIR, rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  FAIL: cannot open fixture %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

__attribute__((unused)) static int hf_count(const char *hay, const char *needle)
{
    int    c  = 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl)
        c++;
    return c;
}

#endif /* HARNESS_FIXTURE_H */
