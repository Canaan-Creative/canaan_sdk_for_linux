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

#include "ini_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

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

#define PATH "test_ini_file.ini"

#define BUMPS    100 /* increments per writer process */
#define SRC      "test_ini_file.src"
#define SRC_TEXT "[syscfg]\nwebpass = abc\n"
#define SEEDERS  8

#define WPA_HEAD "ctrl_interface=/var/run/wpa_supplicant\nap_scan=1\nupdate_config=1\n"
#define WPA_NET  "network={\n\tssid=\"lab\"\n\tpsk=\"12345678\"\n}\n"

static int    fails;
static size_t g_fail_alloc_size; /* allocation size malloc() fails on, 0 for none */

extern void *__libc_malloc(size_t n); /* glibc allocator the malloc() override forwards to */

void *malloc(size_t n)
{
    if (g_fail_alloc_size && n == g_fail_alloc_size)
        return NULL;
    return __libc_malloc(n);
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[4096];
    size_t      n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static void utime_past(const char *path)
{
    struct utimbuf t = { .actime = 1000000000, .modtime = 1000000000 };
    utime(path, &t);
}

static int set_two_keys(ini_t *ini, void *ctx)
{
    (void)ctx;
    if (ini_set(ini, "syscfg", "webpass", "cafe") != 0)
        return -1;
    return ini_set(ini, "led", "bright", "42");
}

static int abort_update(ini_t *ini, void *ctx)
{
    (void)ctx;
    ini_set(ini, "syscfg", "webpass", "clobbered");
    return -1;
}

static int bump_counter(ini_t *ini, void *ctx)
{
    const char *key = (const char *)ctx;
    const char *v   = ini_get(ini, "t", key);
    char        buf[16];
    snprintf(buf, sizeof buf, "%d", (v ? atoi(v) : 0) + 1);
    return ini_set(ini, "t", key, buf);
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static time_t mtime_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void test_fill(void)
{
    int re = -1;

    put(SRC, "[syscfg]\nwebuser = admin\nwebpass = abc\n\n[mode0]\ncali_param0 =\n");
    remove(PATH);
    CHECK(ini_file_fill(PATH, SRC, &re) == 3 && !re &&
                    !strcmp(read_all(PATH),
                            "[syscfg]\nwebuser = admin\nwebpass = abc\n\n"
                            "[mode0]\ncali_param0 = \n\n"),
            "fill creates a missing file with every default key");
    put(PATH, "");
    CHECK(ini_file_fill(PATH, SRC, &re) == 3 && !re &&
                    strstr(read_all(PATH), "webpass = abc") != NULL,
            "fill completes an empty file");
    put(PATH, "[syscfg]\nwebpass =\nmine = 1\nwebuser = me\n");
    CHECK(ini_file_fill(PATH, SRC, &re) == 2 && !re &&
                    !strcmp(read_all(PATH),
                            "[syscfg]\nwebpass = abc\nmine = 1\nwebuser = me\n\n"
                            "[mode0]\ncali_param0 = \n\n"),
            "fill sets an empty value and a missing key, keeps user keys, values and order");
    put(PATH, "# kept as is\n[syscfg]\nwebuser = me\nwebpass = x\n[mode0]\ncali_param0 =\n");
    utime_past(PATH);
    time_t before = mtime_of(PATH);
    CHECK(ini_file_fill(PATH, SRC, &re) == 0 && !re && mtime_of(PATH) == before &&
                    strstr(read_all(PATH), "# kept as is") != NULL,
            "fill with nothing missing does not write the file");
    remove(PATH);
    CHECK(mkdir(PATH, 0755) == 0 && ini_file_fill(PATH, SRC, &re) == 3 && re &&
                    strstr(read_all(PATH), "webuser = admin") != NULL,
            "fill recreates a path that is a directory from the default");
    FILE *f = fopen(PATH, "w");
    for (int i = 0; i < INI_MAX_BYTES + 1; i++)
        fputc('#', f);
    fclose(f);
    CHECK(ini_file_fill(PATH, SRC, &re) == 3 && re &&
                    !strcmp(read_all(PATH),
                            "[syscfg]\nwebuser = admin\nwebpass = abc\n\n"
                            "[mode0]\ncali_param0 = \n\n"),
            "fill recreates an oversized file from the default");
    put(PATH, "[syscfg]\nwebuser = me\n");
    g_fail_alloc_size = strlen("[syscfg]\nwebuser = me\n") + 1;
    re                = -1;
    CHECK(ini_file_fill(PATH, SRC, &re) == -1 && !re &&
                    !strcmp(read_all(PATH), "[syscfg]\nwebuser = me\n"),
            "fill leaves the file alone when reading it runs out of memory");
    g_fail_alloc_size = 0;
    remove(SRC);
    put(PATH, "[syscfg]\nwebuser = me\n");
    CHECK(ini_file_fill(PATH, SRC, &re) == -1 && !re &&
                    !strcmp(read_all(PATH), "[syscfg]\nwebuser = me\n"),
            "fill without a default fails and leaves the file alone");
    CHECK(ini_file_fill(NULL, SRC, NULL) == -1 && ini_file_fill("", SRC, NULL) == -1 &&
                    ini_file_fill(PATH, NULL, NULL) == -1,
            "fill rejects a NULL or empty path");
    CHECK(!file_exists(PATH ".tmp"), "no tmp litter after a fill");
    remove(PATH);
}

static void test_head_fill(void)
{
    put(SRC, WPA_HEAD);
    remove(PATH);
    CHECK(ini_file_head_fill(PATH, SRC) == 0 && !file_exists(PATH),
            "head fill leaves a missing file to the seed");
    put(PATH, WPA_HEAD WPA_NET);
    utime_past(PATH);
    time_t before = mtime_of(PATH);
    CHECK(ini_file_head_fill(PATH, SRC) == 0 && mtime_of(PATH) == before,
            "head fill with every header line present does not write");
    put(PATH, "update_config=1\n" WPA_NET);
    CHECK(ini_file_head_fill(PATH, SRC) == 2 &&
                    !strcmp(read_all(PATH),
                            "ctrl_interface=/var/run/wpa_supplicant\nap_scan=1\n"
                            "update_config=1\n" WPA_NET),
            "head fill inserts the missing header lines at the top, network block untouched");
    put(PATH,
            "ctrl_interface=DIR=/run/wpa GROUP=netdev\n" WPA_NET
            "network={\n\tssid=\"x\"\n\tap_scan=2\n}\n");
    CHECK(ini_file_head_fill(PATH, SRC) == 2 &&
                    !strcmp(read_all(PATH),
                            "ap_scan=1\nupdate_config=1\n"
                            "ctrl_interface=DIR=/run/wpa GROUP=netdev\n" WPA_NET
                            "network={\n\tssid=\"x\"\n\tap_scan=2\n}\n"),
            "head fill keeps a header value the user changed and ignores keys inside a block");
    put(PATH, "#network={\n#\tssid=\"old\"\n#}\n" WPA_HEAD WPA_NET);
    utime_past(PATH);
    before = mtime_of(PATH);
    CHECK(ini_file_head_fill(PATH, SRC) == 0 && mtime_of(PATH) == before,
            "head fill ignores braces in comment lines");
    put(PATH, "");
    CHECK(ini_file_head_fill(PATH, SRC) == 3 && !strcmp(read_all(PATH), WPA_HEAD),
            "head fill completes an empty file");
    remove(SRC);
    CHECK(ini_file_head_fill(PATH, SRC) == -1, "head fill without a default fails");
    CHECK(!file_exists(PATH ".tmp"), "no tmp litter after a head fill");
    remove(PATH);
}

int main(void)
{
    remove(PATH);
    remove(PATH ".tmp");
    remove(PATH ".lock");

    {
        ini_t *doc  = ini_file_load(PATH);
        char  *text = doc ? ini_file_serialize(doc) : NULL;
        CHECK(text && !strcmp(text, ""), "load of a missing file is an empty document");
        free(text);
        CHECK(doc && ini_set(doc, "s", "k", "v") == 0 && (text = ini_file_serialize(doc)) &&
                        !strcmp(text, "[s]\nk = v\n\n"),
                "serialize renders the document as ini_write does");
        free(text);
        ini_free(doc);
    }

    CHECK(ini_file_update(PATH, set_two_keys, NULL) == 0, "update creates a missing file");
    ini_t *ini = ini_load(PATH);
    CHECK(ini && !strcmp(ini_get(ini, "syscfg", "webpass"), "cafe") &&
                    !strcmp(ini_get(ini, "led", "bright"), "42"),
            "one update wrote both keys");
    ini_free(ini);
    CHECK(!file_exists(PATH ".tmp"), "no tmp litter after success");

    {
        CHECK(mkdir(PATH ".dir", 0755) == 0, "sandbox dir for the unreadable case");
        CHECK(ini_file_update(PATH ".dir", set_two_keys, NULL) == -1,
                "a path that exists but cannot be parsed is refused, not replaced "
                "-- without this a transient read fault costs every key the "
                "caller did not set");
        CHECK(ini_file_load(PATH ".dir") == NULL,
                "load of a path that exists but cannot be parsed fails");
        struct stat st;
        CHECK(stat(PATH ".dir", &st) == 0 && S_ISDIR(st.st_mode),
                "...and the thing it could not read is left untouched");
        remove(PATH ".dir.lock");
        rmdir(PATH ".dir");
    }

    FILE *f = fopen(PATH, "a");
    fprintf(f, "[lcd]\nonoff = 1\n");
    fclose(f);
    CHECK(ini_file_update(PATH, set_two_keys, NULL) == 0, "update over an existing file");
    ini = ini_load(PATH);
    CHECK(ini && ini_get(ini, "lcd", "onoff") && !strcmp(ini_get(ini, "lcd", "onoff"), "1"),
            "a key the update did not touch is preserved");
    ini_free(ini);

    char before[4096];
    snprintf(before, sizeof before, "%s", read_all(PATH));
    CHECK(ini_file_update(PATH, abort_update, NULL) == -1, "aborted update reports failure");
    CHECK(!strcmp(before, read_all(PATH)), "aborted update leaves the file byte-identical");
    CHECK(!file_exists(PATH ".tmp"), "no tmp litter after an abort");

    pid_t pa = fork();
    if (pa == 0) {
        for (int i = 0; i < BUMPS; i++)
            if (ini_file_update(PATH, bump_counter, (void *)"a") != 0)
                _exit(1);
        _exit(0);
    }
    pid_t pb = fork();
    if (pb == 0) {
        for (int i = 0; i < BUMPS; i++)
            if (ini_file_update(PATH, bump_counter, (void *)"b") != 0)
                _exit(1);
        _exit(0);
    }
    int sta = -1, stb = -1;
    waitpid(pa, &sta, 0);
    waitpid(pb, &stb, 0);
    CHECK(WIFEXITED(sta) && WEXITSTATUS(sta) == 0 && WIFEXITED(stb) && WEXITSTATUS(stb) == 0,
            "both writer processes succeeded");
    ini = ini_load(PATH);
    CHECK(ini && ini_get(ini, "t", "a") && atoi(ini_get(ini, "t", "a")) == BUMPS,
            "writer A lost no update (%s/%d)", ini ? ini_get(ini, "t", "a") : "-", BUMPS);
    CHECK(ini && ini_get(ini, "t", "b") && atoi(ini_get(ini, "t", "b")) == BUMPS,
            "writer B lost no update (%s/%d)", ini ? ini_get(ini, "t", "b") : "-", BUMPS);
    CHECK(ini && !strcmp(ini_get(ini, "syscfg", "webpass"), "cafe"),
            "the concurrent bumps preserved the unrelated keys");
    ini_free(ini);

    CHECK(ini_file_update(NULL, set_two_keys, NULL) == -1, "NULL path rejected");
    CHECK(ini_file_update("", set_two_keys, NULL) == -1, "empty path rejected");
    CHECK(ini_file_update(PATH, NULL, NULL) == -1, "NULL update rejected");

    remove(PATH);
    remove(SRC);
    CHECK(ini_file_seed(PATH, SRC) == -1 && !file_exists(PATH),
            "seed without a default copy fails and creates nothing");
    f = fopen(SRC, "w");
    fputs(SRC_TEXT, f);
    fclose(f);
    CHECK(ini_file_seed(PATH, SRC) == 1, "seed copies a missing file");
    CHECK(!strcmp(read_all(PATH), SRC_TEXT), "the copy is byte-identical to the default");
    CHECK(!file_exists(PATH ".tmp"), "no tmp litter after a seed");
    f = fopen(PATH, "w");
    fputs("[lcd]\nonoff = 1\n", f);
    fclose(f);
    CHECK(ini_file_seed(PATH, SRC) == 0, "seed of an existing file reports present");
    CHECK(!strcmp(read_all(PATH), "[lcd]\nonoff = 1\n"), "an existing file is left untouched");

    remove(PATH);
    pid_t seeders[SEEDERS];
    for (int i = 0; i < SEEDERS; i++) {
        seeders[i] = fork();
        if (seeders[i] == 0)
            _exit(ini_file_seed(PATH, SRC) + 1);
    }
    int copied = 0, present = 0;
    for (int i = 0; i < SEEDERS; i++) {
        int st = -1;
        waitpid(seeders[i], &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 2)
            copied++;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 1)
            present++;
    }
    CHECK(copied == 1 && present == SEEDERS - 1,
            "concurrent seeders: one copies, the rest find the file (%d/%d)", copied, present);
    CHECK(!strcmp(read_all(PATH), SRC_TEXT), "the concurrent seed left the default intact");
    CHECK(ini_file_seed(NULL, SRC) == -1 && ini_file_seed("", SRC) == -1 &&
                    ini_file_seed(PATH, NULL) == -1,
            "seed rejects a NULL or empty path");

    test_fill();
    test_head_fill();

    remove(SRC);
    remove(PATH);
    remove(PATH ".tmp");
    remove(PATH ".lock");
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
