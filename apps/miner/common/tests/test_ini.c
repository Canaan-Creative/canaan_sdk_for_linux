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

#include "ini.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef COMMON_FIXTURES_DIR
#define COMMON_FIXTURES_DIR "."
#endif

static int fails;

static void expect_str(const char *name, const char *got, const char *want)
{
    if (got == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "  %-24s FAIL: got '%s' want '%s'\n", name, got ? got : "(null)", want);
        fails++;
    } else {
        printf("  %-24s ok\n", name);
    }
}

static void expect_null(const char *name, const char *got)
{
    if (got != NULL) {
        fprintf(stderr, "  %-24s FAIL: expected absent, got '%s'\n", name, got);
        fails++;
    } else {
        printf("  %-24s ok\n", name);
    }
}

static void test_inline(void)
{
    static const char sample[] = "# a comment\n"
                                 "; another\n"
                                 "\n"
                                 "[syscfg]\n"
                                 "work_mode                      = 2\n"
                                 "webuser                        = admin\n"
                                 "empty_val                      =\n"
                                 "\n"
                                 "[hwcfg]\n"
                                 "model                          = Nano3s\n";

    ini_t *ini = ini_parse(sample);
    if (!ini) {
        fprintf(stderr, "  ini_parse returned NULL\n");
        fails++;
        return;
    }

    expect_str("whitespace-trim value", ini_get(ini, "syscfg", "work_mode"), "2");
    expect_str("plain value", ini_get(ini, "syscfg", "webuser"), "admin");
    expect_str("empty value", ini_get(ini, "syscfg", "empty_val"), "");
    expect_str("second section", ini_get(ini, "hwcfg", "model"), "Nano3s");
    expect_null("missing key", ini_get(ini, "syscfg", "nope"));
    expect_null("missing section", ini_get(ini, "nosec", "model"));

    ini_set(ini, "syscfg", "work_mode", "5");
    expect_str("overwrite", ini_get(ini, "syscfg", "work_mode"), "5");
    ini_set(ini, "syscfg", "new_key", "new_val");
    expect_str("insert", ini_get(ini, "syscfg", "new_key"), "new_val");

    ini_free(ini);
}

static void test_fixture_roundtrip(void)
{
    ini_t *a = ini_load(COMMON_FIXTURES_DIR "/config/systemcfg.ini");
    if (!a) {
        fprintf(stderr, "  systemcfg.ini fixture load FAIL\n");
        fails++;
        return;
    }

    expect_str("fixture webuser", ini_get(a, "syscfg", "webuser"), "admin");
    expect_str("fixture model", ini_get(a, "hwcfg", "model"), "Nano3s");
    expect_str("fixture prod", ini_get(a, "hwcfg", "prod"), "Avalon Nano3s");
    const char *wp = ini_get(a, "syscfg", "webpass");
    if (!wp || strlen(wp) != 64) {
        fprintf(stderr, "  fixture webpass          FAIL: len %zu\n", wp ? strlen(wp) : 0);
        fails++;
    } else {
        printf("  %-24s ok\n", "fixture webpass len 64");
    }

    char   buf[4096];
    size_t need = ini_write(a, buf, sizeof buf);
    if (need >= sizeof buf) {
        fprintf(stderr, "  round-trip buffer too small (%zu)\n", need);
        fails++;
    } else {
        ini_t *b = ini_parse(buf);
        if (!b) {
            fprintf(stderr, "  reparse FAIL\n");
            fails++;
        } else {
            expect_str("rt webuser", ini_get(b, "syscfg", "webuser"), "admin");
            expect_str("rt prod (interior space)", ini_get(b, "hwcfg", "prod"), "Avalon Nano3s");
            expect_str("rt led rgb", ini_get(b, "led", "rgb"), "255");
            expect_str("rt webpass", ini_get(b, "syscfg", "webpass"), wp);
            ini_free(b);
        }
    }
    ini_free(a);
}

static void write_sized(const char *path, size_t n)
{
    FILE             *f      = fopen(path, "w");
    static const char head[] = "[s]\nk=v\n#";

    fputs(head, f);
    for (size_t i = sizeof head - 1; i < n - 1; i++)
        fputc('x', f);
    fputc('\n', f);
    fclose(f);
}

static void test_load_bounds(void)
{
    ini_t *ini;

    mkdir("ini_dir.tmp", 0755);
    ini = ini_load("ini_dir.tmp");
    if (ini) {
        fprintf(stderr, "  load directory           FAIL: not NULL\n");
        fails++;
        ini_free(ini);
    } else
        printf("  load directory           ok\n");
    rmdir("ini_dir.tmp");

    write_sized("ini_max.tmp", INI_MAX_BYTES);
    ini = ini_load("ini_max.tmp");
    expect_str("load at the size cap", ini ? ini_get(ini, "s", "k") : NULL, "v");
    ini_free(ini);
    remove("ini_max.tmp");

    write_sized("ini_big.tmp", INI_MAX_BYTES + 1);
    ini = ini_load("ini_big.tmp");
    if (ini) {
        fprintf(stderr, "  load over the size cap   FAIL: not NULL\n");
        fails++;
        ini_free(ini);
    } else
        printf("  load over the size cap   ok\n");
    remove("ini_big.tmp");
}

static void test_fill(void)
{
    ini_t *dst = ini_parse("[syscfg]\nwebuser = me\nwebpass =\nmine = 1\n\n[lcd]\nid = 3\n");
    ini_t *src = ini_parse("[syscfg]\nwebuser = admin\nwebpass = abc\nreason = 1\n"
                           "\n[lcd]\nid = 1\nnote =\n\n[audio]\nvolume = 100\n");
    char   out[256];
    int    n = ini_fill(dst, src);

    if (n != 4) {
        fprintf(stderr, "  fill count               FAIL: %d want 4\n", n);
        fails++;
    } else
        printf("  fill count               ok\n");
    expect_str("fill keeps a value", ini_get(dst, "syscfg", "webuser"), "me");
    expect_str("fill sets an empty value", ini_get(dst, "syscfg", "webpass"), "abc");
    expect_str("fill adds a missing key", ini_get(dst, "syscfg", "reason"), "1");
    expect_str("fill keeps a user key", ini_get(dst, "syscfg", "mine"), "1");
    expect_str("fill keeps a section", ini_get(dst, "lcd", "id"), "3");
    expect_str("fill adds an empty key", ini_get(dst, "lcd", "note"), "");
    expect_str("fill adds a section", ini_get(dst, "audio", "volume"), "100");
    ini_write(dst, out, sizeof out);
    expect_str("fill keeps the order", out,
            "[syscfg]\nwebuser = me\nwebpass = abc\nmine = 1\nreason = 1\n\n"
            "[lcd]\nid = 3\nnote = \n\n[audio]\nvolume = 100\n\n");
    n = ini_fill(dst, src);
    if (n != 0) {
        fprintf(stderr, "  fill again               FAIL: %d want 0\n", n);
        fails++;
    } else
        printf("  fill again               ok\n");
    ini_free(src);
    ini_free(dst);
}

int main(void)
{
    test_inline();
    test_fixture_roundtrip();
    test_load_bounds();
    test_fill();
    if (fails) {
        fprintf(stderr, "test_ini: %d FAIL\n", fails);
        return 1;
    }
    printf("test_ini: all pass\n");
    return 0;
}
