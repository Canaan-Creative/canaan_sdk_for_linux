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

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "osif.h"
#include "ini.h"
#include "mm_tz.h"

#include "../../tests/include/tst_check.h"

#define ROOT TZ_TEST_ROOT /* Fixture root directory in the build tree */
#define ZDIR ROOT "/zoneinfo"
#define LT   ROOT "/localtime"

#undef MM_TZ_ZONEINFO_DIR
#define MM_TZ_ZONEINFO_DIR ZDIR /* Overridden to the fixture zoneinfo directory */
#undef MM_TZ_LOCALTIME_PATH
#define MM_TZ_LOCALTIME_PATH LT /* Overridden to the fixture localtime path */
#define TST_CFG_PATH         ROOT "/systemcfg.ini"

#define EPOCH ((time_t)1704067200) /* 2024-01-01 00:00:00 UTC, unix seconds */

#include "mm_tz.c"

static char tst_log[512]; /* Text of every osif_log() call, concatenated */

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    va_list ap;
    size_t  n = strlen(tst_log);
    va_start(ap, fmt);
    vsnprintf(tst_log + n, sizeof tst_log - n, fmt, ap);
    va_end(ap);
}

static const char *host_zoneinfo(void)
{
    static const char *cand[] = { "/usr/share/zoneinfo", "/usr/lib/zoneinfo" };
    struct stat        st;
    for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s/Asia/Shanghai", cand[i]);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
            return cand[i];
    }
    return NULL;
}

static void install_zone(const char *host, const char *zone)
{
    char src[256], dst[256], dir[256];
    snprintf(src, sizeof src, "%s/%s", host, zone);
    snprintf(dst, sizeof dst, "%s/%s", ZDIR, zone);
    snprintf(dir, sizeof dir, "%s", dst);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        for (char *p = dir + 1; *p; p++)
            if (*p == '/') {
                *p = 0;
                mkdir(dir, 0755);
                *p = '/';
            }
        mkdir(dir, 0755);
    }
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    if (!in || !out) {
        fprintf(stderr, "  FAIL: fixture: cannot copy %s -> %s\n", src, dst);
        fails++;
    } else {
        char   buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
    }
    if (in)
        fclose(in);
    if (out)
        fclose(out);
}

static const char *installed(void)
{
    static char buf[512];
    ssize_t     n              = readlink(LT, buf, sizeof buf - 1);
    buf[n > 0 ? (size_t)n : 0] = 0;
    return buf;
}

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int hour_via(const char *path)
{
    char tz[300];
    snprintf(tz, sizeof tz, ":%s", path);
    setenv("TZ", tz, 1);
    tzset();
    time_t    t  = EPOCH;
    struct tm lt = *localtime(&t);
    unsetenv("TZ");
    tzset();
    return lt.tm_hour;
}

static int file_has(const char *path, const char *needle)
{
    char  buf[2048];
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

static const char *stored(void)
{
    static char held[MM_TZ_NAME_CAP];
    ini_t      *ini = ini_load(TST_CFG_PATH);
    held[0]         = 0;
    if (!ini)
        return held;
    const char *v = ini_get(ini, "syscfg", "timezone");
    if (v)
        snprintf(held, sizeof held, "%s", v);
    ini_free(ini);
    return held;
}

static void write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "  FAIL: cannot write %s\n", path);
        fails++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

int main(void)
{
    const char *host = host_zoneinfo();
    if (!host) {
        fprintf(stderr,
                "FAILED: no host zoneinfo (install tzdata) — this suite "
                "needs real TZif files; a synthetic one falls back to UTC\n");
        return 1;
    }
    mkdir(ROOT, 0755);
    mkdir(ZDIR, 0755);
    unlink(LT);
    install_zone(host, "Asia/Shanghai");
    install_zone(host, "America/Los_Angeles");
    install_zone(host, "Etc/GMT+8");
    install_zone(host, "America/Argentina/Buenos_Aires");
    install_zone(host, "Etc/UTC");
    printf("fixture: host zoneinfo %s -> %s\n", host, ZDIR);

    setenv("TZ", "UTC0", 1);
    tzset();
    CHECK(mm_apply_settz("Asia/Shanghai") == MC_SET_OK, "a zone the image ships is applied");
    CHECK(strstr(tst_log, "settz Asia/Shanghai applied but NOT persisted"),
            "a settz the unopened store refuses is logged (log=\"%s\")", tst_log);
    CHECK(!strcmp(installed(), ZDIR "/Asia/Shanghai"),
            "localtime is a symlink to the zone's TZif (-> %s)", installed());
    CHECK(getenv("TZ") == NULL,
            "TZ is cleared — it outranks /etc/localtime, so leaving it set would "
            "pin this process to the previous zone and defeat the settz");
    CHECK(!exists(LT ".new"),
            "no temp link left behind (the swap is symlink+rename, so there is "
            "never a moment with no localtime at all)");
    CHECK(hour_via(LT) == 8,
            "glibc reading that link puts 2024-01-01T00:00Z at 08:00 (UTC+8), so "
            "the target is a real TZif and not a footer glibc ignores (got %d)",
            hour_via(LT));

    CHECK(mm_apply_settz("America/Los_Angeles") == MC_SET_OK &&
                    !strcmp(installed(), ZDIR "/America/Los_Angeles") && hour_via(LT) == 16,
            "a second apply replaces the existing link in place and re-localises "
            "to PST (16:00, got %d)",
            hour_via(LT));

    CHECK(mm_apply_settz("Etc/GMT+8") == MC_SET_OK && !strcmp(installed(), ZDIR "/Etc/GMT+8"),
            "'+' in a zone name is accepted (Etc/GMT+8)");
    CHECK(mm_apply_settz("America/Argentina/Buenos_Aires") == MC_SET_OK &&
                    !strcmp(installed(), ZDIR "/America/Argentina/Buenos_Aires"),
            "a three-component zone name is accepted");

    const char *keep = ZDIR "/America/Argentina/Buenos_Aires";
    static const struct {
        const char *name, *why, *what;
    } bad[] = {
        { "/etc/localtime", "illegal zone name", "an absolute path" },
        { "../../etc/passwd", "illegal zone name", "a relative escape" },
        { "Asia/../../etc/passwd", "illegal zone name", "a '..' component mid-path" },
        { ".", "illegal zone name", "a bare dot" },
        { "Asia//Shanghai", "illegal zone name", "an empty component" },
        { "Asia/Shanghai/", "illegal zone name", "a trailing slash" },
        { "Asia/Shang hai", "illegal zone name", "a space" },
        { "Asia/Shanghai;reboot", "illegal zone name", "a shell metacharacter" },
        { "Asia/Shanghai\n", "illegal zone name", "a newline" },
        { "Asia/Shanghai%s", "illegal zone name", "a format specifier" },
        { "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "illegal zone name",
                "an over-long name" },
        { "Not/AZone", "no such zone in the image",
                "a well-formed name the image has no TZif for" },
        { "Etc", "no such zone in the image", "a directory rather than a TZif" },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        tst_log[0]      = 0;
        mc_set_result r = mm_apply_settz(bad[i].name);
        CHECK(r == MC_SET_ERR_ARG && strstr(tst_log, bad[i].why) && !strcmp(installed(), keep) &&
                        !exists(LT ".new"),
                "%s is refused as \"%s\" and leaves the installed zone alone "
                "(rc=%d, log=\"%s\")",
                bad[i].what, bad[i].why, (int)r, tst_log);
    }
    CHECK(mm_apply_settz("") == MC_SET_ERR_ARG && mm_apply_settz(NULL) == MC_SET_ERR_ARG &&
                    !strcmp(installed(), keep),
            "empty and NULL names are refused");
    CHECK(!exists(ROOT "/../etc") && !exists(ROOT "/etc"),
            "no path outside the zoneinfo dir was created by the traversal attempts");

    char name[MM_TZ_NAME_CAP];
    unlink(TST_CFG_PATH);
    CHECK(mm_cfgstore_open(TST_CFG_PATH) == 0, "the store opens over a missing systemcfg");
    mm_tz_get(name, sizeof name);
    CHECK(name[0] == 0, "missing file -> empty zone");
    mm_tz_restore();
    CHECK(mm_apply_settz("Asia/Shanghai") == MC_SET_OK &&
                    !strcmp(installed(), ZDIR "/Asia/Shanghai"),
            "settz switches the zone at once");
    CHECK(!exists(TST_CFG_PATH), "...but does not write systemcfg");
    mm_tz_get(name, sizeof name);
    CHECK(!strcmp(name, "Asia/Shanghai"), "...and the RAM copy holds the new zone (got \"%s\")",
            name);
    CHECK(mm_cfgstore_flush() == 1, "the store flush writes");
    CHECK(!strcmp(stored(), "Asia/Shanghai"), "flush writes the zone (got \"%s\")", stored());
    CHECK(file_has(TST_CFG_PATH, "[syscfg]"), "timezone lands in [syscfg]");
    unlink(TST_CFG_PATH);
    CHECK(mm_cfgstore_flush() == 0 && !exists(TST_CFG_PATH),
            "a flush with nothing pending writes nothing");
    CHECK(mm_apply_settz("America/Los_Angeles") == MC_SET_OK &&
                    mm_apply_settz("Asia/Shanghai") == MC_SET_OK && mm_cfgstore_flush() == 0 &&
                    !exists(TST_CFG_PATH),
            "a zone changed and changed back to the saved one is not written");

    write_file(TST_CFG_PATH,
            "[syscfg]\nledmode = 1\ntimezone = America/Los_Angeles\n"
            "[led]\neffect = 2\n[wifi]\nssid = home\n");
    CHECK(mm_cfgstore_reload() == 0, "the store reloads a populated file");
    mm_tz_restore();
    mm_tz_get(name, sizeof name);
    CHECK(!strcmp(name, "America/Los_Angeles") && !strcmp(installed(), ZDIR "/America/Los_Angeles"),
            "restore reads the stored zone and installs it (got \"%s\")", name);
    CHECK(mm_cfgstore_flush() == 0, "...and leaves nothing pending");
    CHECK(mm_apply_settz("Asia/Shanghai") == MC_SET_OK && mm_cfgstore_flush() == 1 &&
                    !strcmp(stored(), "Asia/Shanghai"),
            "a zone saved over a populated file is read back");
    CHECK(file_has(TST_CFG_PATH, "ledmode") && file_has(TST_CFG_PATH, "ssid"),
            "foreign keys survive a tz save");
    CHECK(file_has(TST_CFG_PATH, "[wifi]") && file_has(TST_CFG_PATH, "[led]"),
            "the atomic rewrite kept the foreign SECTIONS, not just their keys");
    CHECK(!exists(TST_CFG_PATH ".tmp"), "no .tmp left behind after the commit");
    CHECK(exists(TST_CFG_PATH ".lock"),
            "the flock sibling exists (never deleted — "
            "removing a lock file races its next taker)");

    CHECK(mm_cfgstore_open("/proc/nonexistent/dir/x.ini") == 0,
            "the store opens over an unwritable directory");
    CHECK(mm_apply_settz("America/Los_Angeles") == MC_SET_OK && mm_cfgstore_flush() < 0,
            "a zone under an unwritable directory is not written (at the .lock create)");
    CHECK(mm_tz_apply_default("Asia/Shanghai") == MC_SET_OK,
            "the factory default zone still applies under an unwritable directory");
    CHECK(!strcmp(installed(), ZDIR "/Asia/Shanghai"), "...and switches the zone");

    unlink(TST_CFG_PATH);
    CHECK(mm_cfgstore_open(TST_CFG_PATH) == 0, "the store reopens over a missing file");
    mm_tz_restore();
    mm_cfgstore_freeze(1);
    tst_log[0] = 0;
    CHECK(mm_apply_settz("Asia/Shanghai") == MC_SET_OK &&
                    !strcmp(installed(), ZDIR "/Asia/Shanghai"),
            "frozen: settz still switches the zone and replies OK");
    CHECK(!strstr(tst_log, "NOT persisted"),
            "frozen: the unpersisted settz is not logged (log=\"%s\")", tst_log);
    CHECK(mm_cfgstore_flush() < 0 && !file_has(TST_CFG_PATH, "timezone"),
            "frozen: settz is not persisted, not even by a flush");
    mm_tz_get(name, sizeof name);
    CHECK(name[0] == 0, "frozen: settz leaves the stored zone alone (got \"%s\")", name);
    CHECK(mm_tz_apply_default("America/Los_Angeles") == MC_SET_OK,
            "frozen: the factory default zone applies");
    mm_tz_get(name, sizeof name);
    CHECK(!strcmp(name, "America/Los_Angeles") && !file_has(TST_CFG_PATH, "timezone"),
            "frozen: the factory default zone is held in RAM, not written (got \"%s\")", name);
    CHECK(mm_cfgstore_flush() < 0 && !file_has(TST_CFG_PATH, "timezone"),
            "frozen: a flush does not write the factory default zone");
    CHECK(mm_cfgstore_sync() == 1 && !strcmp(stored(), "America/Los_Angeles"),
            "frozen: the factory's sync persists the factory default zone (got \"%s\")", stored());
    mm_cfgstore_freeze(0);
    CHECK(mm_apply_settz("Asia/Shanghai") == MC_SET_OK, "thawed: settz ok");
    CHECK(!strcmp(stored(), "America/Los_Angeles"),
            "thawed: settz waits for the flush (got \"%s\")", stored());
    mm_cfgstore_freeze(1);
    CHECK(mm_cfgstore_flush() < 0, "frozen: a zone staged before the freeze is held back");
    CHECK(!strcmp(stored(), "America/Los_Angeles"),
            "...and systemcfg keeps the factory default zone (got \"%s\")", stored());
    mm_cfgstore_freeze(0);
    CHECK(mm_cfgstore_flush() == 1, "thawed: flush ok");
    CHECK(!strcmp(stored(), "Asia/Shanghai"), "thawed: the flush persists the zone");

    write_file(TST_CFG_PATH, "[syscfg]\ntimezone = Etc/UTC\n");
    CHECK(mm_cfgstore_reload() == 0, "the store reloads the factory default zone");
    tst_log[0] = 0;
    mm_tz_restore();
    CHECK(!strcmp(installed(), ZDIR "/Etc/UTC") && hour_via(LT) == 0 &&
                    strstr(tst_log, "timezone restored -> Etc/UTC"),
            "restore relinks the factory default Etc/UTC and logs it (log=\"%s\")", tst_log);

    unlink(TST_CFG_PATH);
    unlink(TST_CFG_PATH ".lock");
    unlink(LT);
    if (fails)
        printf("FAILED (%d)\n", fails);
    else
        printf("PASSED\n");
    return fails ? 1 : 0;
}
