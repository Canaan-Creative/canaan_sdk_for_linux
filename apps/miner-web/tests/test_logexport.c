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
#include <unistd.h>
#include <sys/stat.h>

#include "osif_logpath.h"

#undef MINER_LOG_PATH
#undef MINER_OP_LOG_PATH
#undef MINER_WEB_LOG_PATH
#define MINER_LOG_PATH     "logexport_dir/miner.log"
#define MINER_OP_LOG_PATH  "logexport_dir/miner_op.log"
#define MINER_WEB_LOG_PATH "logexport_weblog/web.log"
#undef _DEFAULT_SOURCE

#include "logexport.c"

/* Counts a failed cond in fails and prints msg as FAIL or ok */
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

static void put(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

int main(void)
{
    static const char *const names[][2] = {
        { "miner.log", "logexport_dir/miner.log" },
        { "miner_op.log", "logexport_dir/miner_op.log" },
        { "web.log", "logexport_dir/web.log" },
    };
    static const char *const misses[] = {
        "notes.txt",
        ".hidden.log",
        "a..log",
        "../logexport_dir/miner.log",
        "sub/x.log",
        ".log",
        "bogus",
        "miner.LOG",
        "MINER.LOG",
        "miner.log ",
        "a\nb.log",
        "a\tb.log",
        "../other/notes.txt",
        "",
        "miner-web.tokenkey",
        "miner-web.pem",
        "systemcfg.ini",
        "pools.conf",
        "/data/factory/miner-web.tokenkey",
        "/tmp/miner-web.tokenkey",
        "../factory/miner-web.tokenkey",
        "..%2ffactory%2fminer-web.tokenkey",
        "miner-web.tokenkey.log",
        "other.log",
    };
    static const char *const files[] = {
        "logexport_dir/miner.log",
        "logexport_dir/miner.log.1",
        "logexport_dir/web.log",
    };
    char path[64], name[LOGEXPORT_NAME_MAX], msg[128], big[LOGEXPORT_NAME_MAX + 8];

    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(msg, sizeof msg, "%s -> %s", names[i][0], names[i][1]);
        CHECK(logexport_lookup(names[i][0], path, sizeof path) == 0 && !strcmp(path, names[i][1]),
                msg);
    }
    for (size_t i = 0; i < sizeof misses / sizeof misses[0]; i++) {
        snprintf(msg, sizeof msg, "\"%s\" -> miss", misses[i]);
        CHECK(logexport_lookup(misses[i], path, sizeof path) != 0, msg);
    }
    CHECK(logexport_lookup(NULL, path, sizeof path) != 0, "NULL name -> miss");
    CHECK(logexport_lookup("miner.log", path, 16) != 0, "path longer than the buffer -> miss");

    CHECK(logexport_body_name("miner.log", name, sizeof name) == 0 && !strcmp(name, "miner.log"),
            "body name taken as is");
    CHECK(logexport_body_name("web.log \t\r\n", name, sizeof name) == 0 && !strcmp(name, "web.log"),
            "trailing space, tab, CR and LF removed");
    CHECK(logexport_body_name(NULL, name, sizeof name) == 0 && !name[0], "no body -> empty name");
    memset(big, 'a', LOGEXPORT_NAME_MAX - 5);
    strcpy(big + LOGEXPORT_NAME_MAX - 5, ".log");
    CHECK(logexport_body_name(big, name, sizeof name) == 0 && !strcmp(name, big),
            "name of LOGEXPORT_NAME_MAX - 1 bytes -> taken");
    strcat(big, "x");
    CHECK(logexport_body_name(big, name, sizeof name) != 0,
            "name of LOGEXPORT_NAME_MAX bytes -> refused, not shortened to match");
    big[LOGEXPORT_NAME_MAX - 1] = '\n';
    CHECK(logexport_body_name(big, name, sizeof name) == 0 &&
                    strlen(name) == LOGEXPORT_NAME_MAX - 1,
            "a trailing LF does not count against the limit");

    logexport_safe_name("web.log", name, sizeof name);
    CHECK(!strcmp(name, "web.log"), "safe name: a printable name is kept");
    logexport_safe_name("web.log \t\r\n", name, sizeof name);
    CHECK(!strcmp(name, "web.log"), "safe name: trailing space, tab, CR and LF removed");
    logexport_safe_name("a\nb\x7f\x80\xff.log", name, sizeof name);
    CHECK(!strcmp(name, "a?b???.log"), "safe name: control, DEL and bytes >= 0x80 -> '?'");
    logexport_safe_name(NULL, name, sizeof name);
    CHECK(!name[0], "safe name: no body -> empty");
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    logexport_safe_name(big, name, sizeof name);
    CHECK(strlen(name) == LOGEXPORT_NAME_MAX - 1 && !strncmp(name, big, LOGEXPORT_NAME_MAX - 4) &&
                    !strcmp(name + LOGEXPORT_NAME_MAX - 4, "..."),
            "safe name: a name too long for the buffer is cut and ends in ...");
    big[LOGEXPORT_NAME_MAX - 1] = '\0';
    logexport_safe_name(big, name, sizeof name);
    CHECK(!strcmp(name, big), "safe name: LOGEXPORT_NAME_MAX - 1 bytes -> kept whole");

    mkdir("logexport_dir", 0755);
    mkdir("logexport_weblog", 0755);
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
        put(files[i]);
    unlink("logexport_dir/web.log.1");
    unlink("logexport_dir/miner_op.log");
    unlink("logexport_dir/miner_op.log.1");

    CHECK(logexport_serve_check("miner.log", "logexport_dir/miner.log", 1) == 200,
            "file and .1, both regular -> 200");
    CHECK(logexport_serve_check("web.log", "logexport_dir/web.log", 1) == 200,
            "file without .1 -> 200");
    CHECK(logexport_serve_check("miner_op.log", "logexport_dir/miner_op.log", 1) == 404,
            "missing file -> 404");
    put("logexport_dir/miner_op.log.1");
    CHECK(logexport_serve_check("miner_op.log", "logexport_dir/miner_op.log", 1) == 404,
            "only the .1 present -> 404");
    unlink("logexport_dir/miner_op.log.1");
    CHECK(symlink("miner.log", "logexport_dir/miner_op.log") == 0 &&
                    logexport_serve_check("miner_op.log", "logexport_dir/miner_op.log", 1) == 404,
            "file is a symbolic link -> 404");
    unlink("logexport_dir/miner_op.log");
    CHECK(mkdir("logexport_dir/miner_op.log", 0755) == 0 &&
                    logexport_serve_check("miner_op.log", "logexport_dir/miner_op.log", 1) == 404,
            "file is a directory -> 404");
    rmdir("logexport_dir/miner_op.log");
    CHECK(symlink("miner.log", "logexport_dir/web.log.1") == 0 &&
                    logexport_serve_check("web.log", "logexport_dir/web.log", 1) == 404,
            ".1 is a symbolic link -> 404");
    CHECK(logexport_serve_check("web.log", "logexport_dir/web.log", 0) == 200,
            ".1 is not checked when it is not served");
    unlink("logexport_dir/web.log.1");
    if (geteuid() != 0) {
        CHECK(chmod("logexport_dir/web.log", 0) == 0 &&
                        logexport_serve_check("web.log", "logexport_dir/web.log", 1) == 500,
                "regular file that cannot be opened -> 500");
        chmod("logexport_dir/web.log", 0644);
        put("logexport_dir/web.log.1");
        CHECK(chmod("logexport_dir/web.log.1", 0) == 0 &&
                        logexport_serve_check("web.log", "logexport_dir/web.log", 1) == 500,
                ".1 regular but cannot be opened -> 500");
        unlink("logexport_dir/web.log.1");
    } else {
        printf("SKIP: regular file that cannot be opened -> 500 (root)\n");
        printf("SKIP: .1 regular but cannot be opened -> 500 (root)\n");
    }
    unlink("logexport_dir/web.log");
    CHECK(mkfifo("logexport_dir/web.log", 0644) == 0 &&
                    logexport_serve_check("web.log", "logexport_dir/web.log", 1) == 404,
            "file is a FIFO -> 404, without blocking");
    unlink("logexport_dir/web.log");

    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
        unlink(files[i]);
    rmdir("logexport_dir");
    unlink(MINER_WEB_LOG_PATH);
    rmdir("logexport_weblog");

    char dig[65];
    logexport_pass_digest("admin", dig);
    CHECK(!strcmp(dig, "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918"),
            "sha256(\"admin\") -> 64 hex");

    printf(fails ? "FAILED (%d)\n" : "logexport OK\n", fails);
    return fails ? 1 : geteuid() == 0 ? TST_SKIP : 0;
}
