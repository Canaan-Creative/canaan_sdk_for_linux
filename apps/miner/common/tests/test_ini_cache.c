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

#include "ini_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#define PATH    "test_ini_cache.ini"
#define THREADS 4
#define ROUNDS  2000 /* set/get rounds per setter thread */

static int         fails;
static ini_cache_t g_c;

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

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static ino_t inode_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_ino : 0;
}

static int has(const char *sec, const char *key, const char *want)
{
    char buf[64];
    return ini_cache_get_copy(&g_c, sec, key, buf, sizeof buf) == 0 && !strcmp(buf, want);
}

static void *setter(void *arg)
{
    long id = (long)arg;
    char key[16];
    char val[16];
    char buf[16];
    snprintf(key, sizeof key, "k%ld", id);
    for (int i = 0; i < ROUNDS; i++) {
        snprintf(val, sizeof val, "%d", i);
        if (ini_cache_set(&g_c, "t", key, val) != 0)
            return (void *)1;
        if (ini_cache_get_copy(&g_c, "t", key, buf, sizeof buf) != 0 || strcmp(buf, val))
            return (void *)1;
        if (i % 500 == 0 && ini_cache_flush(&g_c) < 0)
            return (void *)1;
    }
    return NULL;
}

int main(void)
{
    remove(PATH);
    remove(PATH ".tmp");
    remove(PATH ".lock");

    write_file(PATH, "[led]\nbright = 42\n[sys]\nname = box\n");
    CHECK(ini_cache_open(&g_c, PATH) == 0, "open reads an existing file");
    CHECK(has("led", "bright", "42") && has("sys", "name", "box"), "open loads every key");
    write_file(PATH, "[led]\nbright = 7\n");
    CHECK(has("led", "bright", "42"), "reads come from RAM, not the file");

    char small[2];
    CHECK(ini_cache_get_copy(&g_c, "led", "bright", small, sizeof small) == -1,
            "a value longer than the buffer is refused");
    CHECK(ini_cache_get_copy(&g_c, "led", "none", small, sizeof small) == -1,
            "an absent key is refused");

    CHECK(ini_cache_reload(&g_c) == 0 && has("led", "bright", "7") &&
                    ini_cache_get_copy(&g_c, "sys", "name", small, sizeof small) == -1,
            "reload replaces the RAM copy with the file");

    ino_t ino = inode_of(PATH);
    CHECK(ini_cache_flush(&g_c) == 0 && inode_of(PATH) == ino,
            "flush with nothing changed does not write");
    CHECK(ini_cache_set(&g_c, "led", "bright", "7") == 0 && ini_cache_flush(&g_c) == 0,
            "setting the saved value does not write");

    CHECK(ini_cache_set(&g_c, "led", "bright", "9") == 0, "set changes the RAM copy");
    CHECK(has("led", "bright", "9"), "set is visible to readers at once");
    CHECK(strstr(read_all(PATH), "bright = 7") != NULL, "set does not touch the file");
    CHECK(ini_cache_flush(&g_c) == 1 && inode_of(PATH) != ino, "flush writes a change");
    CHECK(strstr(read_all(PATH), "bright = 9") != NULL, "the file holds the new value");
    CHECK(access(PATH ".tmp", F_OK) != 0, "no tmp litter after a flush");
    ino = inode_of(PATH);
    CHECK(ini_cache_flush(&g_c) == 0 && inode_of(PATH) == ino, "a second flush does not write");

    CHECK(ini_cache_set(&g_c, "led", "bright", "1") == 0 && ini_cache_reload(&g_c) == 0 &&
                    has("led", "bright", "9") && ini_cache_flush(&g_c) == 0,
            "reload drops an unflushed change");
    ini_cache_close(&g_c);

    remove(PATH);
    CHECK(ini_cache_open(&g_c, PATH) == 0, "open accepts a missing file");
    CHECK(ini_cache_flush(&g_c) == 0 && access(PATH, F_OK) != 0,
            "an empty cache over a missing file writes nothing");
    CHECK(ini_cache_set(&g_c, "a", "b", "c") == 0 && ini_cache_flush(&g_c) == 1 &&
                    strstr(read_all(PATH), "b = c") != NULL,
            "flush creates the missing file");

    pthread_t th[THREADS];
    for (long i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, setter, (void *)i);
    int bad = 0;
    for (int i = 0; i < THREADS; i++) {
        void *r;
        pthread_join(th[i], &r);
        bad += r != NULL;
    }
    CHECK(bad == 0, "concurrent set/get/flush from %d threads stay consistent", THREADS);
    CHECK(ini_cache_flush(&g_c) == 1, "the last concurrent sets are written");
    ini_cache_close(&g_c);
    CHECK(ini_cache_open(&g_c, PATH) == 0 && has("t", "k0", "1999") && has("t", "k3", "1999") &&
                    has("a", "b", "c"),
            "the file holds every thread's last value");
    ini_cache_close(&g_c);

    static const char *const keys[] = { "prod", "model" };
    static const char *const vals[] = { "P1", "M1" };
    write_file(PATH, "[led]\nbright = 5\n");
    CHECK(ini_cache_open(&g_c, PATH) == 0 && ini_cache_set(&g_c, "led", "bright", "6") == 0,
            "an unflushed change is pending before a commit");
    CHECK(ini_cache_commit(&g_c, "hwcfg", keys, vals, 2) == 0 && has("hwcfg", "prod", "P1") &&
                    has("hwcfg", "model", "M1"),
            "commit sets every key in RAM");
    CHECK(strstr(read_all(PATH), "prod = P1") != NULL &&
                    strstr(read_all(PATH), "bright = 6") != NULL,
            "commit writes its keys and the pending change at once");
    ino = inode_of(PATH);
    CHECK(ini_cache_flush(&g_c) == 0 && inode_of(PATH) == ino,
            "a flush after a commit has nothing left to write");
    ini_cache_close(&g_c);

    static const char *const n_sects[] = { "syscfg", "led" };
    static const char *const n_keys[]  = { "ledmode", "rgb" };
    static const char *const n_vals[]  = { "1", "7" };
    CHECK(ini_cache_open(&g_c, PATH) == 0 &&
                    ini_cache_set_n(&g_c, n_sects, n_keys, n_vals, 2) == 0 &&
                    has("syscfg", "ledmode", "1") && has("led", "rgb", "7"),
            "set_n sets every pair, across sections, in RAM");
    CHECK(strstr(read_all(PATH), "rgb = 7") == NULL, "set_n does not touch the file");
    CHECK(ini_cache_flush(&g_c) == 1 && strstr(read_all(PATH), "ledmode = 1") != NULL &&
                    strstr(read_all(PATH), "rgb = 7") != NULL,
            "one flush after set_n writes every pair");
    ini_cache_close(&g_c);

    CHECK(ini_cache_open(&g_c, "no_such_dir/" PATH) == 0, "open over a missing directory");
    CHECK(ini_cache_commit(&g_c, "hwcfg", keys, vals, 2) == -1 && !has("hwcfg", "prod", "P1"),
            "a commit that cannot write leaves RAM unchanged");
    ini_cache_close(&g_c);

    CHECK(mkdir(PATH ".dir", 0755) == 0, "sandbox dir for the unreadable case");
    CHECK(ini_cache_open(&g_c, PATH ".dir") == -1, "open refuses a path it cannot parse");
    CHECK(ini_cache_set(&g_c, "a", "b", "c") == -1 && ini_cache_flush(&g_c) == -1,
            "an unreadable cache refuses set and flush, so the path is not replaced");
    CHECK(ini_cache_set_n(&g_c, n_sects, n_keys, n_vals, 2) == -1,
            "an unreadable cache refuses set_n");
    ini_cache_close(&g_c);
    rmdir(PATH ".dir");

    write_file(PATH, "[syscfg]\nwebuser = me\n");
    ini_t *def = ini_parse("[syscfg]\nwebuser = admin\nwebpass = abc\n");
    CHECK(ini_cache_open(&g_c, PATH) == 0, "open for fill");
    CHECK(ini_cache_fill(&g_c, def) == 1 && has("syscfg", "webuser", "me") &&
                    has("syscfg", "webpass", "abc"),
            "fill adds the missing key and keeps the value");
    CHECK(!strcmp(read_all(PATH), "[syscfg]\nwebuser = me\n"), "fill does not write the file");
    CHECK(ini_cache_flush(&g_c) == 1 &&
                    !strcmp(read_all(PATH), "[syscfg]\nwebuser = me\nwebpass = abc\n\n"),
            "the next flush writes the filled key");
    CHECK(ini_cache_fill(&g_c, def) == 0 && ini_cache_flush(&g_c) == 0,
            "a second fill adds nothing and the flush writes nothing");
    ini_cache_close(&g_c);
    CHECK(mkdir(PATH ".dir", 0755) == 0, "sandbox dir for the unreadable fill");
    CHECK(ini_cache_open(&g_c, PATH ".dir") == -1 && ini_cache_fill(&g_c, def) == -1,
            "an unreadable cache refuses fill");
    ini_cache_close(&g_c);
    rmdir(PATH ".dir");
    ini_free(def);

    remove(PATH);
    remove(PATH ".tmp");
    remove(PATH ".lock");
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
