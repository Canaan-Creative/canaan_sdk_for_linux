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

#include <time.h>
#include "mc_api_setter.h"
#include "ctrl_cmds.h"
#include "sha256.h"
#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../tests/include/tst_check.h"
#include "../../tests/include/tst_rsa_sign.h"

int tst_clock_gettime(clockid_t id, struct timespec *ts);
#define clock_gettime tst_clock_gettime
#include "core_wallpaper.c"
#undef clock_gettime

/* Staging roots live under the test's working directory and are removed at exit. */
#define MAX_ROOTS 32

osif_ipc_t *g_ipc_core_mm; /* IPC handle to mm_miner used by the code under test */

static uint64_t g_now_ms; /* injected monotonic clock, ms */

static int      g_send_n;  /* Messages sent through osif_ipc_send() */
static uint16_t g_send_op; /* op of the last mc_ctrl_cmd sent */

/* Commits and clears made; non-zero makes the commit or the clear fail */
static int         g_commit_n, g_clear_n, g_commit_fail, g_clear_fail;
static unsigned    g_commit_uid;    /* uid of the last committed wallpaper */
static char        g_commit_fmt[8]; /* Format of the last committed wallpaper */
static const char *g_stage_root;    /* Staging directory the committed .tmp file is removed from */

static const char HEXD[] = "0123456789abcdef";
static char       g_roots[MAX_ROOTS][PATH_MAX];
static int        g_nroots;

int tst_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec  = (time_t)(g_now_ms / 1000u);
    ts->tv_nsec = (long)(g_now_ms % 1000u) * 1000000L;
    return 0;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)type;
    (void)len;
    const mc_ctrl_cmd *r = data;
    g_send_op            = r->op;
    g_send_n++;
    return OSIF_OK;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}

int cfgwr_write_wallpaper(unsigned uid, const char *format)
{
    if (g_commit_fail)
        return -1;
    char staged[512];
    g_commit_n++;
    g_commit_uid = uid;
    snprintf(g_commit_fmt, sizeof g_commit_fmt, "%s", format);
    snprintf(staged, sizeof staged, "%s/wallpaper_%u.%s.tmp", g_stage_root, uid, format);
    remove(staged);
    return 0;
}

int cfgwr_clear_wallpaper(void)
{
    if (g_clear_fail)
        return -1;
    g_clear_n++;
    return 0;
}

static void wp_reset(const char *stage)
{
    static char stage_buf[256];
    g_uid_cur    = 0;
    g_offset_cur = 0;
    memset(g_data_sha, 0, sizeof g_data_sha);
    g_fmt_cur[0] = '\0';
    snprintf(stage_buf, sizeof stage_buf, "%s", stage);
    g_stage = stage_buf;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}
static void hexcat(char *dst, const uint8_t *src, size_t n)
{
    size_t o = strlen(dst);
    for (size_t i = 0; i < n; i++) {
        dst[o++] = HEXD[src[i] >> 4];
        dst[o++] = HEXD[src[i] & 0xf];
    }
    dst[o] = 0;
}

static void build_chunk(char *hex, size_t hexcap, uint32_t format_ver, const char *magic,
        uint16_t payload_len, uint16_t uid, uint16_t idx, uint32_t file_size, uint32_t offset,
        const uint8_t *payload)
{
    (void)hexcap;
    uint8_t hdr[28] = { 0 };
    memcpy(hdr, magic, strlen(magic) > 8 ? 8 : strlen(magic));
    put_le32(hdr + 8, format_ver);
    hdr[12] = 28;
    put_le16(hdr + 14, payload_len);
    put_le16(hdr + 16, uid);
    put_le16(hdr + 18, idx);
    put_le32(hdr + 20, file_size);
    put_le32(hdr + 24, offset);
    hex[0] = 0;
    hexcat(hex, hdr, 28);
    if (payload && payload_len)
        hexcat(hex, payload, payload_len);
}

static void sign_c0(uint8_t *c0, const uint8_t *sha)
{
    memcpy(c0, sha, 32);
    if (tst_rsa_sign(PIC_SIGN_KEY, 1, sha, 32, c0 + 32) != 0) {
        fprintf(stderr, "cannot sign with %s\n", PIC_SIGN_KEY);
        exit(2);
    }
}

static mc_set_result run_wp(const char *format, const char *hex, char *msg, size_t cap)
{
    static char  req[8192];
    mc_api_stats s = { 0 };
    s.prod         = "Avalon Nano3s";
    s.version      = "v";
    s.dna          = "d";
    s.mac          = "m";
    snprintf(req, sizeof req, "ascset|0,wallpaper,%s%s%s", format, hex ? "," : "", hex ? hex : "");
    return mc_api_setter_run(req, &s, msg, cap);
}

static void rm_tree(const char *dir)
{
    DIR           *dp = opendir(dir);
    struct dirent *e;
    char           path[PATH_MAX];

    if (dp) {
        while ((e = readdir(dp))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            if (e->d_type == DT_DIR)
                rm_tree(path);
            else
                remove(path);
        }
        closedir(dp);
    }
    rmdir(dir);
}

static void rm_roots(void)
{
    for (int i = 0; i < g_nroots; i++)
        rm_tree(g_roots[i]);
    g_nroots = 0;
}

static char *newroot(void)
{
    static char t[PATH_MAX];
    char        cwd[200]; /* wp_reset keeps the root in a 256-byte buffer */
    if (!getcwd(cwd, sizeof cwd)) {
        perror("getcwd");
        exit(2);
    }
    snprintf(t, sizeof t, "%s/wp_test_XXXXXX", cwd);
    if (!mkdtemp(t)) {
        perror("mkdtemp");
        exit(2);
    }
    if (g_nroots >= MAX_ROOTS) {
        fprintf(stderr, "too many staging roots\n");
        exit(2);
    }
    snprintf(g_roots[g_nroots++], PATH_MAX, "%s", t);
    wp_reset(t);
    g_stage_root = t;
    g_commit_n = g_clear_n = 0;
    return t;
}

static int count_regular(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return -1;
    struct dirent *e;
    int            n = 0;
    while ((e = readdir(dp)))
        if (e->d_type == DT_REG)
            n++;
    closedir(dp);
    return n;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void)
{
    char msg[256];
    char hex[8192];

    mc_api_setter_reset();
    g_ipc_core_mm = (osif_ipc_t *)0x1;
    CHECK(core_wallpaper_register() == 1, "register wallpaper raw on a board with a panel");

    newroot();
    {
        mc_api_stats s  = { 0 };
        s.prod          = "x";
        s.version       = "v";
        s.dna           = "d";
        s.mac           = "m";
        mc_set_result r = mc_api_setter_run("ascset|0,wallpaper,png", &s, msg, sizeof msg);
        CHECK(r == MC_SET_ERR_ARG && !strcmp(msg, "ASC 0 set failed: input param error"),
                "no comma -> input param error");
        r = mc_api_setter_run("ascset|0,wallpaper", &s, msg, sizeof msg);
        CHECK(r == MC_SET_ERR_ARG && !strcmp(msg, "ASC 0 set failed: missing wallpaper setting"),
                "no setting -> missing wallpaper setting");
    }

    memset(hex, 'a', 2106);
    hex[2106] = 0;
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: input param len greater than 2104"),
            "over-long hex -> len greater than 2104");

    CHECK(run_wp("bmp", "0011", msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: Input param cmd error."),
            "bad format -> Input param cmd error.");

    uint8_t pay[2048];
    for (size_t i = 0; i < sizeof pay; i++)
        pay[i] = (uint8_t)(i * 7 + 1);

    newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 0, 1, 0, 2048, 0, NULL);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 1"),
            "short header -> failed 1");

    newroot();
    build_chunk(hex, sizeof hex, 1, "XXXX", 1, 1, 0, 2048, 0, pay);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 2"),
            "bad magic -> failed 2");

    newroot();
    build_chunk(hex, sizeof hex, 2, "nano", 1, 1, 0, 2048, 0, pay);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 3"),
            "bad version -> failed 3");

    newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 1, 1, 0, 100, 0, pay);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 4"),
            "bad size -> failed 4");

    newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 10, 1, 0, 2048, 0, pay);
    strcat(hex, "ff");
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 6"),
            "payload length mismatch -> failed 6");

    newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 100, 1, 0, 1024, 1000, pay);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
            "offset past end -> failed 5");

    newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 32, 1, 0, 2048, 0, pay);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 8"),
            "chunk0 payload_len != 160 -> failed 8");

    newroot();
    {
        uint8_t c0[160];
        uint8_t data_sha[32];
        sha256(pay, 1024, data_sha);
        memcpy(c0, data_sha, 32);
        memset(c0 + 32, 0xA5, 128);
        build_chunk(hex, sizeof hex, 1, "nano", 160, 1, 0, 1024, 0, c0);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 10"),
                "forged signature -> failed 10 (RSA gate)");

        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_1.png.tmp", g_stage_root);
        g_send_n = 0;
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 1, 1, 1024, 0, pay);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "data chunk after a forged chunk0, no session -> failed 5");
        CHECK(g_commit_n == 0 && g_send_n == 0, "a forged chunk0 sha is never published");
        CHECK(!file_exists(sfile) && count_regular(g_stage_root) == 0,
                "a forged chunk0 stages no file");

        struct stat st;
        FILE       *fp = fopen(sfile, "w");
        if (fp)
            fclose(fp);
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 1, 1, 1024, 0, pay);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "data chunk after a forged chunk0, staged file present -> failed 5");
        CHECK(g_commit_n == 0 && g_send_n == 0,
                "a forged chunk0 sha is never published over a staged file");
        CHECK(stat(sfile, &st) == 0 && st.st_size == 0,
                "a data chunk after a forged chunk0 is not written to the staged file");
    }

    char   *root = newroot();
    uint8_t file[1536];
    for (size_t i = 0; i < sizeof file; i++)
        file[i] = (uint8_t)(i & 0xff);
    uint8_t data_sha[32];
    sha256(file, sizeof file, data_sha);

    uint8_t c0[160];
    sign_c0(c0, data_sha);

    g_send_n = 0;
    build_chunk(hex, sizeof hex, 1, "nano", 160, 7, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO &&
                    strstr(msg, "ASC 0 set info: wallpaper recv ok "),
            "chunk0 accepted -> info recv ok");
    CHECK(g_send_n == 0, "chunk0 does not reload the panel");
    {
        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_7.png.tmp", root);
        CHECK(file_exists(sfile), "chunk0 creates the empty staging file");
    }

    build_chunk(hex, sizeof hex, 1, "nano", 1024, 7, 1, 1536, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO && strstr(msg, "wallpaper recv ok "),
            "mid chunk accepted -> info recv ok");
    CHECK(g_send_n == 0, "mid chunk does not reload the panel");
    CHECK(g_commit_n == 0, "mm is not asked to publish until the final chunk verifies");

    build_chunk(hex, sizeof hex, 1, "nano", 512, 7, 2, 1536, 1024, file + 1024);
    mc_set_result fr = run_wp("png", hex, msg, sizeof msg);
    CHECK(fr == MC_SET_INFO && !strcmp(msg, "ASC 0 set info: wallpaper recv ok 1080"),
            "final chunk -> info recv ok 1080 (byte-exact)");
    CHECK(g_send_n == 1 && g_send_op == CTRL_OP_WP_RELOAD,
            "final chunk reloads the panel (CTRL_OP_WP_RELOAD)");

    CHECK(g_commit_n == 1 && g_commit_uid == 7 && !strcmp(g_commit_fmt, "png"),
            "the verified image is handed to mm as uid 7 / png, never written to /data");
    {
        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_7.png.tmp", root);
        CHECK(!file_exists(sfile), "staging tmp consumed by mm");
    }
    CHECK(count_regular(root) == 0, "core leaves nothing behind in the staging dir");
    build_chunk(hex, sizeof hex, 1, "nano", 512, 7, 2, 1536, 1024, file + 1024);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
            "a published upload ends its session: a resent final chunk -> failed 5");
    CHECK(g_commit_n == 1, "...and is not published again");

    root                 = newroot();
    uint8_t zero_sha[32] = { 0 };
    sign_c0(c0, zero_sha);
    g_send_n = 0;
    build_chunk(hex, sizeof hex, 1, "nano", 160, 8, 0, 1024, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "mismatch: chunk0 ok");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 8, 1, 1024, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 9"),
            "whole-file sha mismatch -> failed 9");
    CHECK(g_send_n == 0 && g_commit_n == 0,
            "a mismatched upload is neither published nor reloaded");
    CHECK(count_regular(root) == 0, "a mismatched upload leaves no staged file");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 8, 1, 1024, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
            "a mismatched upload ends its session: a resent chunk -> failed 5");
    sha256(file, 1024, data_sha);
    sign_c0(c0, data_sha);
    build_chunk(hex, sizeof hex, 1, "nano", 160, 8, 0, 1024, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO,
            "after a mismatch a fresh chunk0 opens a session");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 8, 1, 1024, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO && g_commit_n == 1 &&
                    g_commit_uid == 8 && count_regular(root) == 0,
            "...and the new upload is published, leaving no staged file");
    sha256(file, sizeof file, data_sha);

    root = newroot();
    sign_c0(c0, data_sha);
    build_chunk(hex, sizeof hex, 1, "nano", 160, 15, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "write error: chunk0 ok");
    {
        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_15.png.tmp", root);
        remove(sfile);
        CHECK(symlink("/dev/full", sfile) == 0, "write error: staged file points at /dev/full");
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 15, 1, 1536, 0, file);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 7"),
                "write error -> failed 7");
        struct stat lst;
        CHECK(lstat(sfile, &lst) != 0, "a write error removes the staged file");
    }
    build_chunk(hex, sizeof hex, 1, "nano", 512, 15, 2, 1536, 1024, file + 1024);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
            "a write error ends the session: the next chunk -> failed 5");
    build_chunk(hex, sizeof hex, 1, "nano", 160, 15, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO,
            "after a write error a fresh chunk0 opens a session");
    {
        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_15.png.tmp", root);
        remove(sfile);
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 15, 1, 1536, 0, file);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 7"),
                "open error -> failed 7");
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 15, 1, 1536, 0, file);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "an open error ends the session: the next chunk -> failed 5");
        CHECK(count_regular(root) == 0, "an open error leaves no staged file");
    }

    root = newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 160, 9, 0, 4096, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "gap: chunk0 ok");
    build_chunk(hex, sizeof hex, 1, "nano", 100, 9, 1, 4096, 500, pay);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
            "offset gap in the stream -> failed 5");

    root = newroot();
    {
        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_5.png.tmp", root);
        g_send_n = 0;
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 5, 1, 1536, 0, file);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "data chunk with no chunk0 and no staged file -> failed 5");
        CHECK(g_commit_n == 0 && g_send_n == 0,
                "an upload with no chunk0 is neither published nor reloaded");
        CHECK(!file_exists(sfile) && count_regular(root) == 0,
                "a data chunk with no chunk0 stages no file");

        struct stat st;
        FILE       *fp = fopen(sfile, "w");
        if (fp)
            fclose(fp);
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 5, 1, 1536, 0, file);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "data chunk with no chunk0, staged file present -> failed 5");
        CHECK(stat(sfile, &st) == 0 && st.st_size == 0,
                "a data chunk with no chunk0 is not written to a pre-existing staged file");
        build_chunk(hex, sizeof hex, 1, "nano", 512, 5, 2, 1536, 1024, file + 1024);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "final chunk with no chunk0 since reset -> failed 5");
        CHECK(g_commit_n == 0 && g_send_n == 0,
                "an upload with no chunk0 over a staged file is neither published nor reloaded");
    }

    root = newroot();
    sign_c0(c0, data_sha);
    build_chunk(hex, sizeof hex, 1, "nano", 160, 6, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "uid6 chunk0 ok");
    build_chunk(hex, sizeof hex, 1, "nano", 32, 6, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 8"),
            "uid6 malformed chunk0 -> failed 8");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 6, 1, 1536, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO,
            "uid6 data chunk after a malformed chunk0 -> recv ok");
    g_send_n = 0;
    build_chunk(hex, sizeof hex, 1, "nano", 512, 6, 2, 1536, 1024, file + 1024);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO &&
                    !strcmp(msg, "ASC 0 set info: wallpaper recv ok 1080"),
            "uid6 final chunk matches the earlier signed sha -> recv ok 1080");
    CHECK(g_commit_n == 1 && g_commit_uid == 6 && g_send_n == 1,
            "uid6 is published under the earlier verified sha");

    root = newroot();
    sign_c0(c0, data_sha);
    build_chunk(hex, sizeof hex, 1, "nano", 160, 4, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "resume: chunk0 ok");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 4, 1, 1536, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "resume: mid ok");
    build_chunk(hex, sizeof hex, 1, "nano", 160, 4, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "resume: chunk0 again ok");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 4, 1, 1536, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "resume: mid again ok");
    g_send_n = 0;
    build_chunk(hex, sizeof hex, 1, "nano", 512, 4, 2, 1536, 1024, file + 1024);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO &&
                    !strcmp(msg, "ASC 0 set info: wallpaper recv ok 1080"),
            "resume: final chunk -> recv ok 1080");
    CHECK(g_commit_n == 1 && g_commit_uid == 4 && g_send_n == 1,
            "resume: the re-verified upload is published");

    root = newroot();
    sign_c0(c0, data_sha);
    build_chunk(hex, sizeof hex, 1, "nano", 160, 12, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "replay: chunk0 ok");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 12, 1, 1536, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "replay: mid ok");
    {
        struct stat st;
        char        sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_12.png.tmp", root);
        build_chunk(hex, sizeof hex, 1, "nano", 160, 12, 0, 1536, 0, c0);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "replay: chunk0 replayed");
        CHECK(run_wp("gif", hex, msg, sizeof msg) == MC_SET_INFO,
                "replay: chunk0 replayed with another format");
        CHECK(stat(sfile, &st) == 0 && st.st_size == 1024 && count_regular(root) == 1,
                "a replayed chunk0 keeps the staged data");
    }
    {
        uint8_t forged[160];
        memcpy(forged, data_sha, 32);
        memset(forged + 32, 0xA5, 128);
        build_chunk(hex, sizeof hex, 1, "nano", 160, 13, 0, 1536, 0, forged);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 10"),
                "replay: a forged chunk0 for another uid -> failed 10");
    }
    build_chunk(hex, sizeof hex, 1, "nano", 512, 14, 1, 1536, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
            "replay: a data chunk with a new uid -> failed 5");
    {
        char other[512];
        snprintf(other, sizeof other, "%s/wallpaper_14.png.tmp", root);
        CHECK(!file_exists(other) && count_regular(root) == 1,
                "a data chunk with a new uid stages nothing");
    }
    g_send_n = 0;
    build_chunk(hex, sizeof hex, 1, "nano", 512, 12, 2, 1536, 1024, file + 1024);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO &&
                    !strcmp(msg, "ASC 0 set info: wallpaper recv ok 1080"),
            "replay: the upload completes after the replays");
    CHECK(g_commit_n == 1 && g_commit_uid == 12 && g_send_n == 1,
            "replay: the upload is published");

    root = newroot();
    {
        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_16.png.tmp", root);
        sign_c0(c0, data_sha);
        build_chunk(hex, sizeof hex, 1, "nano", 160, 16, 0, 1536, 0, c0);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "idle: chunk0 ok");
        g_now_ms += WP_IDLE_MS - 1;
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 16, 1, 1536, 0, file);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO,
                "idle: a chunk just inside the idle limit is accepted");
        g_now_ms += WP_IDLE_MS - 1;
        core_wallpaper_tick();
        CHECK(file_exists(sfile), "idle: the tick keeps an upload with recent data");
        g_now_ms += 1;
        core_wallpaper_tick();
        CHECK(!file_exists(sfile) && count_regular(root) == 0,
                "idle: the tick ends an upload idle for 5 min and removes its staged file");
        build_chunk(hex, sizeof hex, 1, "nano", 512, 16, 2, 1536, 1024, file + 1024);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "idle: a later chunk for the expired upload -> failed 5");

        build_chunk(hex, sizeof hex, 1, "nano", 160, 16, 0, 1536, 0, c0);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "idle: a new chunk0 ok");
        g_now_ms += WP_IDLE_MS;
        build_chunk(hex, sizeof hex, 1, "nano", 1024, 16, 1, 1536, 0, file);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 5"),
                "idle: a request after 5 min idle ends the upload first -> failed 5");
        CHECK(!file_exists(sfile) && g_commit_n == 0,
                "idle: the expired upload leaves no staged file and is not published");
    }

    root = newroot();
    {
        char  stale[512];
        FILE *fp;
        snprintf(stale, sizeof stale, "%s/wallpaper_1.png.tmp", root);
        fp = fopen(stale, "w");
        if (fp) {
            fputs("stale", fp);
            fclose(fp);
        }
        CHECK(count_regular(root) == 1, "none: a stale staged file is present");
    }
    g_send_n = 0;
    CHECK(run_wp("none", "x", msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: input param error"),
            "none with a field after it -> input param error");
    CHECK(run_wp("none", "", msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: input param error"),
            "none with a trailing comma -> input param error");
    CHECK(g_clear_n == 0 && g_send_n == 0 && count_regular(root) == 1,
            "a refused none clears nothing and does not reload the panel");
    CHECK(run_wp("none", NULL, msg, sizeof msg) == MC_SET_INFO &&
                    !strcmp(msg, "ASC 0 set info: wallpaper recv ok 0"),
            "none -> info recv ok");
    CHECK(g_clear_n == 1, "none asks mm to clear the display dir");
    CHECK(count_regular(root) == 0, "none clears the staging dir too");
    CHECK(g_send_n == 1 && g_send_op == CTRL_OP_WP_RELOAD,
            "none reloads the panel (CTRL_OP_WP_RELOAD)");

    g_clear_fail = 1;
    g_send_n     = 0;
    CHECK(run_wp("none", NULL, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 7"),
            "none with mm refusing -> failed 7, not a lied success");
    CHECK(g_send_n == 0, "a refused clear does not reload the panel");
    g_clear_fail = 0;

    root = newroot();
    sign_c0(c0, data_sha);
    build_chunk(hex, sizeof hex, 1, "nano", 160, 11, 0, 1536, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "uid11 chunk0 ok");
    CHECK(run_wp("none", NULL, msg, sizeof msg) == MC_SET_INFO, "uid11 cleared by none");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 11, 1, 1536, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 7"),
            "none removes the staging file: a later uid11 data chunk -> failed 7");

    root = newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 160, 100, 0, 4096, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "uid100 chunk0 ok");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 100, 1, 4096, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "uid100 mid ok (partial)");
    uint8_t data_sha2[32];
    sha256(file, 1024, data_sha2);
    sign_c0(c0, data_sha2);
    build_chunk(hex, sizeof hex, 1, "nano", 160, 200, 0, 1024, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "uid200 chunk0 ok");
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 200, 1, 1024, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO &&
                    !strcmp(msg, "ASC 0 set info: wallpaper recv ok 2104"),
            "uid200 completes -> recv ok 2104 after a new uid reset");

    root = newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 160, 300, 0, 1024, 0, c0);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "uid300 chunk0 ok");
    g_commit_fail = 1;
    g_send_n      = 0;
    build_chunk(hex, sizeof hex, 1, "nano", 1024, 300, 1, 1024, 0, file);
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 7"),
            "mm refusing the publish -> failed 7");
    CHECK(g_send_n == 0, "a refused publish does not reload the panel");
    {
        char sfile[512];
        snprintf(sfile, sizeof sfile, "%s/wallpaper_300.png.tmp", root);
        CHECK(!file_exists(sfile), "a refused publish removes the staged file");
    }
    g_commit_fail = 0;

    root = newroot();
    build_chunk(hex, sizeof hex, 1, "nano", 160, 400, 0, 1024, 0, c0);
    hex[3] = 'g';
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 1"),
            "non-hex header -> failed 1");

    build_chunk(hex, sizeof hex, 1, "nano", 160, 400, 0, 1024, 0, c0);
    hex[28 * 2 + 5] = 'z';
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                    !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 6"),
            "non-hex payload -> failed 6");
    CHECK(count_regular(root) == 0, "a non-hex chunk0 stages nothing");

    build_chunk(hex, sizeof hex, 1, "nano", 160, 400, 0, 1024, 0, c0);
    for (char *p = hex; *p; p++)
        if (*p >= 'a' && *p <= 'f')
            *p = (char)(*p - 'a' + 'A');
    CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_INFO, "upper-case hex chunk0 accepted");

    root = newroot();
    {
        char missing[256];
        snprintf(missing, sizeof missing, "%s/no/such", root);
        wp_reset(missing);
        build_chunk(hex, sizeof hex, 1, "nano", 160, 500, 0, 1024, 0, c0);
        CHECK(run_wp("png", hex, msg, sizeof msg) == MC_SET_ERR_ARG &&
                        !strcmp(msg, "ASC 0 set failed: wallpaper recv failed 7"),
                "uncreatable staging dir -> failed 7");
    }

    rm_roots();
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
