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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "boardconf.h"
#include "mm_spawn.h"
#include "cali.h"
#include "fan_dev.h"
#include "mm_telem.h"
#include "osif.h"
#include "wdt_dev.h"
#include "tst_fan_dev.h"
#include "../../tests/include/tst_check.h"

#undef ASIC_START_DELAY_S
#define ASIC_START_DELAY_S 0 /* asic_miner spawn delay, s: none */

#define TST_SLEEP_CAP 4096 /* osif_sleep_ms() calls before g_run is forced to 0 */
#define CAP           ((size_t)MINER_LOG_CAP) /* miner.log rotation cap, bytes */
#define OVER          (CAP + 1u)              /* one byte over CAP */
#define WELL_OVER     (CAP + 4096u)           /* 4 KiB over CAP */
#define COPY_BLK      65536u                  /* mm_logrot.c copy buffer size, bytes */

int    tst_stat(const char *path, struct stat *st);
FILE  *tst_fopen(const char *path, const char *mode);
int    tst_truncate(const char *path, off_t len);
size_t tst_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int    tst_fclose(FILE *f);
int    tst_rename(const char *from, const char *to);
int    tst_unlink(const char *path);
void   tst_log(osif_loglvl_t lvl, const char *fmt, ...);
pid_t  tst_fork(void);
int    tst_system(const char *cmd);
int    tst_kill(pid_t pid, int sig);
pid_t  tst_waitpid(pid_t pid, int *status, int options);

#undef stat
#undef fopen
#undef truncate
#undef fwrite
#undef fclose
/* Calls in the included sources, routed to the sandboxed tst_* fakes below */
#define stat(p, b)         tst_stat((p), (b))
#define fopen(p, m)        tst_fopen((p), (m))
#define truncate(p, l)     tst_truncate((p), (l))
#define fwrite(p, s, n, f) tst_fwrite((p), (s), (n), (f))
#define fclose(f)          tst_fclose(f)
#define osif_log(...)      tst_log(__VA_ARGS__)
#define fork()             tst_fork()
#define system(c)          tst_system(c)
#define kill(p, s)         tst_kill((p), (s))
#define waitpid(p, s, o)   tst_waitpid((p), (s), (o))

#include "mm_fan.c"
#include "mm_net.c"
#include "../../hal/platform/linux/otp_dev.c"
#include "../../hal/platform/linux/net_ident.c"
#include "../../hal/platform/linux/net_wired_alt.c"
#include "mm_tz.c"
/* rename() and unlink() in mm_logrot.c, routed into the sandbox */
#define rename(f, t) tst_rename((f), (t))
#define unlink(p)    tst_unlink(p)
#include "mm_logrot.c"
#undef unlink
#undef rename
#include "mm_miner_tu.h"
#undef waitpid
#undef kill
#undef system
#undef fork
#undef osif_log
#undef fclose
#undef fwrite
#undef truncate
#undef fopen
#undef stat

static char tst_dir[192];
/* Sandbox stand-ins for the miner, web and OP logs: live, .1 and .1.tmp */
static char tst_live[256];
static char tst_gen1[256];
static char tst_web[256];
static char tst_webgen1[256];
static char tst_op[256];
static char tst_opgen1[256];
static char tst_tmp[256];
static char tst_webtmp[256];
static char tst_optmp[256];
#if NET_WIRED_ALT
/* Sandbox stand-ins for the wired carrier, flags and udhcpc pid files */
static char tst_carrier[256];
static char tst_flags[256];
static char tst_dhcppid[256];
#endif /* NET_WIRED_ALT */
static int           tst_stats;
static uint64_t      tst_stat_at[12];          /* clock at each of the first stat() calls, ms */
static int           tst_fail_rb, tst_fail_wb; /* nonzero fails read / write fopen() */
static int           tst_short_after;          /* fwrite() calls before writes return 0; -1 never */
static int           tst_fwrites;              /* fwrite() calls counted against tst_short_after */
static int           tst_fail_dst_close;       /* nonzero fails fclose() of the write stream */
static int           tst_fail_truncate;
static int           tst_truncates;
static FILE         *tst_dst_fp; /* stream of the last write fopen() */
static int           tst_fail_rename;
static int           tst_renames;
static char          tst_last_log[512];
static osif_loglvl_t tst_last_lvl;
static int           tst_rot_lines; /* "rotated in-run" log lines */
static uint64_t      tst_now;       /* fake monotonic clock, ms */
/* Passes run and wanted; osif_sleep_ms() calls; TST_SLEEP_CAP overrun flag */
static int   tst_pass, tst_want, tst_sleeps, tst_runaway;
static pid_t tst_next_pid = 4000; /* next fake fork() PID */

static void tst_cleanup(void)
{
    if (!tst_dir[0])
        return;
    unlink(tst_live);
    unlink(tst_gen1);
    unlink(tst_web);
    unlink(tst_webgen1);
    unlink(tst_op);
    unlink(tst_opgen1);
    unlink(tst_tmp);
    unlink(tst_webtmp);
    unlink(tst_optmp);
    rmdir(tst_dir);
}

static const char *tst_map(const char *path)
{
    if (path && !strcmp(path, MINER_LOG_PATH))
        return tst_live;
    if (path && !strcmp(path, MINER_LOG_PATH ".1"))
        return tst_gen1;
    if (path && !strcmp(path, MINER_WEB_LOG_PATH))
        return tst_web;
    if (path && !strcmp(path, MINER_WEB_LOG_PATH ".1"))
        return tst_webgen1;
    if (path && !strcmp(path, MINER_OP_LOG_PATH))
        return tst_op;
    if (path && !strcmp(path, MINER_OP_LOG_PATH ".1"))
        return tst_opgen1;
    if (path && !strcmp(path, MINER_LOG_PATH ".1.tmp"))
        return tst_tmp;
    if (path && !strcmp(path, MINER_WEB_LOG_PATH ".1.tmp"))
        return tst_webtmp;
    if (path && !strcmp(path, MINER_OP_LOG_PATH ".1.tmp"))
        return tst_optmp;
#if NET_WIRED_ALT
    if (path && !strcmp(path, "/sys/class/net/" NET_WIRED_IFACE "/carrier"))
        return tst_carrier;
    if (path && !strcmp(path, "/sys/class/net/" NET_WIRED_IFACE "/flags"))
        return tst_flags;
    if (path && !strcmp(path, "/var/run/udhcpc." NET_WIRED_IFACE ".pid"))
        return tst_dhcppid;
#endif /* NET_WIRED_ALT */
    if (path && !strncmp(path, "/sys/class/net/", 15)) {
        static char absent[512];
        snprintf(absent, sizeof absent, "%s/absent-sysfs", tst_dir);
        return absent;
    }
    fprintf(stderr,
            "  FAIL: interception hole — the code under test reached "
            "\"%s\", outside the sandbox %s. Aborting before anything on "
            "this host is written.\n",
            path ? path : "(null)", tst_dir);
    tst_cleanup();
    _exit(2);
}

int tst_stat(const char *path, struct stat *st)
{
    const char *real = tst_map(path);
    if (tst_stats < (int)(sizeof tst_stat_at / sizeof tst_stat_at[0]))
        tst_stat_at[tst_stats] = osif_now_ms();
    tst_stats++;
    return stat(real, st);
}

FILE *tst_fopen(const char *path, const char *mode)
{
    const char *real = tst_map(path);
    int         w    = mode && strchr(mode, 'w') != NULL;
    if (w ? tst_fail_wb : tst_fail_rb)
        return NULL;
    FILE *f = fopen(real, mode);
    if (w)
        tst_dst_fp = f;
    return f;
}

size_t tst_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (tst_short_after >= 0 && tst_fwrites++ >= tst_short_after)
        return 0;
    return fwrite(p, sz, n, f);
}

int tst_fclose(FILE *f)
{
    int fail = tst_fail_dst_close && f == tst_dst_fp;
    int rc   = fclose(f);
    if (f == tst_dst_fp)
        tst_dst_fp = NULL;
    return fail ? EOF : rc;
}

int tst_truncate(const char *path, off_t len)
{
    const char *real = tst_map(path);
    tst_truncates++;
    if (tst_fail_truncate)
        return -1;
    return truncate(real, len);
}

int tst_rename(const char *from, const char *to)
{
    char real_from[256];
    snprintf(real_from, sizeof real_from, "%s", tst_map(from));
    const char *real_to = tst_map(to);
    tst_renames++;
    if (tst_fail_rename)
        return -1;
    return rename(real_from, real_to);
}

int tst_unlink(const char *path)
{
    return unlink(tst_map(path));
}

void tst_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tst_last_log, sizeof tst_last_log, fmt, ap);
    va_end(ap);
    tst_last_lvl = lvl;
    if (strstr(tst_last_log, "rotated in-run"))
        tst_rot_lines++;
}

uint64_t osif_now_ms(void)
{
    return tst_now;
}
void osif_sleep_ms(uint32_t ms)
{
    tst_now += ms;
    if (++tst_sleeps <= TST_SLEEP_CAP)
        return;
    tst_runaway = 1;
    g_run       = 0;
    if (tst_sleeps > 2 * TST_SLEEP_CAP) {
        fprintf(stderr,
                "  FAIL: the supervisor loop does not exit on a cleared "
                "g_run — aborting\n");
        tst_cleanup();
        _exit(1);
    }
}

void mm_telem_poll(void)
{
    if (++tst_pass >= tst_want)
        g_run = 0;
}
void mm_telem_ingest(const void *d, int l, unsigned c)
{
    (void)d;
    (void)l;
    (void)c;
}
void mm_telem_ingest_core(const void *d, int l)
{
    (void)d;
    (void)l;
}

void mm_telem_get(mm_telem_t *out)
{
    memset(out, 0, sizeof *out);
    out->poll_fresh = 1;
    out->asic_live  = 1;
    out->in_work    = 1;
    out->tmax       = 60;
    out->tavg       = 55;
    if (CAP_HB_MCU)
        out->hb_count = HBOARD_COUNT;
    for (int b = 0; b < HBOARD_COUNT; b++)
        out->hb_outlet[b] = out->hb_inlet[b] = 60;
}

int wdt_dev_init(void)
{
    return 0;
}
int wdt_dev_bootstatus(void)
{
    return 0;
}
int wdt_dev_timeout_s(void)
{
    return 2 * WDT_TIMEOUT_S;
}
int wdt_dev_deadline_s(void)
{
    return WDT_TIMEOUT_S;
}
int wdt_dev_feed_period_ms(void)
{
    return WDT_FEED_PERIOD_MS_MAX;
}
void wdt_dev_disarm(void)
{
}
void wdt_dev_feed(void)
{
}

pid_t tst_fork(void)
{
    return tst_next_pid++;
}
int tst_system(const char *cmd)
{
    (void)cmd;
    return 0;
}
int tst_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    return 0;
}
pid_t tst_waitpid(pid_t pid, int *status, int options)
{
    (void)pid;
    (void)options;
    if (status)
        *status = 0;
    return 0;
}

static unsigned char pat(size_t i, unsigned seed)
{
    return (unsigned char)((i * 31u + seed * 131u) & 0xffu);
}

static void make_file(const char *p, size_t n, unsigned seed)
{
    FILE *f = fopen(p, "wb");
    if (!f) {
        fprintf(stderr, "  FAIL: sandbox unwritable (%s)\n", p);
        tst_cleanup();
        _exit(2);
    }
    for (size_t i = 0; i < n; i++)
        fputc(pat(i, seed), f);
    if (fclose(f) != 0) {
        fprintf(stderr, "  FAIL: sandbox write failed\n");
        tst_cleanup();
        _exit(2);
    }
}

static long fsize(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static int is_pat(const char *p, size_t n, unsigned seed)
{
    if (fsize(p) != (long)n)
        return 0;
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    int ok = 1;
    for (size_t i = 0; i < n && ok; i++) {
        int c = fgetc(f);
        if (c == EOF || (unsigned char)c != pat(i, seed))
            ok = 0;
    }
    if (ok && fgetc(f) != EOF)
        ok = 0;
    fclose(f);
    return ok;
}

static void arm(void)
{
    tst_fail_rb = tst_fail_wb = 0;
    tst_short_after           = -1;
    tst_fail_dst_close = tst_fail_truncate = 0;
    tst_fwrites                            = 0;
    tst_truncates                          = 0;
    tst_rot_lines                          = 0;
    tst_fail_rename                        = 0;
    tst_renames                            = 0;
    tst_dst_fp                             = NULL;
    tst_last_log[0]                        = '\0';
}

static void fire(void)
{
    logrot_rule(NULL, 0);
}

int main(void)
{
    char        probe[256];
    struct stat before, after;
    int         had_real_log, real_before_ok;

    snprintf(tst_dir, sizeof tst_dir, "/tmp/mm_logrot_%s_XXXXXX", BOARDCONF_SECTION);
    if (!mkdtemp(tst_dir)) {
        fprintf(stderr, "  FAIL: mkdtemp failed\n");
        return 1;
    }
    atexit(tst_cleanup);
    snprintf(tst_live, sizeof tst_live, "%s/miner.log", tst_dir);
    snprintf(tst_gen1, sizeof tst_gen1, "%s/miner.log.1", tst_dir);
    snprintf(tst_web, sizeof tst_web, "%s/web.log", tst_dir);
    snprintf(tst_webgen1, sizeof tst_webgen1, "%s/web.log.1", tst_dir);
    snprintf(tst_op, sizeof tst_op, "%s/miner_op.log", tst_dir);
    snprintf(tst_opgen1, sizeof tst_opgen1, "%s/miner_op.log.1", tst_dir);
    snprintf(tst_tmp, sizeof tst_tmp, "%s/miner.log.1.tmp", tst_dir);
    snprintf(tst_webtmp, sizeof tst_webtmp, "%s/web.log.1.tmp", tst_dir);
    snprintf(tst_optmp, sizeof tst_optmp, "%s/miner_op.log.1.tmp", tst_dir);
#if NET_WIRED_ALT
    snprintf(tst_carrier, sizeof tst_carrier, "%s/carrier", tst_dir);
    snprintf(tst_flags, sizeof tst_flags, "%s/flags", tst_dir);
    snprintf(tst_dhcppid, sizeof tst_dhcppid, "%s/dhcp.pid", tst_dir);
#endif /* NET_WIRED_ALT */

    printf("descriptor %s: %d board(s), log %s on %s, cap %u B (%u MiB), "
           "sandbox %s\n",
            BOARDCONF_SECTION, (int)HBOARD_COUNT, MINER_LOG_PATH,
            "ubi2:ubi_data_part (its own 49 MiB volume)", MINER_LOG_CAP, MINER_LOG_CAP >> 20,
            tst_dir);

    real_before_ok = stat(MINER_LOG_PATH, &before) == 0;
    had_real_log   = real_before_ok;

    {
        size_t dlen = strlen(tst_dir);
        CHECK(!strncmp(tst_map(MINER_LOG_PATH), tst_dir, dlen) &&
                        !strncmp(tst_map(MINER_LOG_PATH ".1"), tst_dir, dlen) &&
                        strcmp(tst_map(MINER_LOG_PATH), tst_map(MINER_LOG_PATH ".1")),
                "both production paths are redirected into the per-run sandbox "
                "before a byte is written, and to two DIFFERENT files (%s, %s); "
                "any other path reaching an intercepted seam aborts the run rather "
                "than touching this host",
                tst_live, tst_gen1);
        CHECK(!strncmp(tst_map(MINER_WEB_LOG_PATH), tst_dir, dlen) &&
                        !strncmp(tst_map(MINER_WEB_LOG_PATH ".1"), tst_dir, dlen) &&
                        strcmp(tst_map(MINER_WEB_LOG_PATH), tst_map(MINER_WEB_LOG_PATH ".1")) &&
                        strcmp(tst_map(MINER_WEB_LOG_PATH), tst_map(MINER_LOG_PATH)),
                "...and so is the SECOND file the rule rotates (%s, %s) — "
                "enumerated in tst_map like the wired-net reads, not exempted, so "
                "\"exactly these paths are legal\" still holds",
                tst_web, tst_webgen1);
        CHECK(!strncmp(tst_map(MINER_OP_LOG_PATH), tst_dir, dlen) &&
                        !strncmp(tst_map(MINER_OP_LOG_PATH ".1"), tst_dir, dlen) &&
                        strcmp(tst_map(MINER_OP_LOG_PATH), tst_map(MINER_OP_LOG_PATH ".1")) &&
                        strcmp(tst_map(MINER_OP_LOG_PATH), tst_map(MINER_LOG_PATH)) &&
                        strcmp(tst_map(MINER_OP_LOG_PATH), tst_map(MINER_WEB_LOG_PATH)),
                "...and so is the THIRD file, the OP retention sink (%s, %s)", tst_op, tst_opgen1);
        CHECK(strcmp(MINER_LOG_PATH, MINER_WEB_LOG_PATH) != 0 &&
                        strstr(MINER_WEB_LOG_PATH, "web.log") != NULL &&
                        strcmp(MINER_OP_LOG_PATH, MINER_LOG_PATH) != 0 &&
                        strstr(MINER_OP_LOG_PATH, "miner_op.log") != NULL,
                "the three live paths are distinct: miner_web's web.log and "
                "the OP sink are each their own file, so neither shares "
                "miner.log's ~3.4 h turnover");
    }

    {
        const char *live = MINER_LOG_PATH, *web = MINER_WEB_LOG_PATH;
        const char *ls = strrchr(live, '/'), *ws = strrchr(web, '/');
        CHECK(ls && ws && !strcmp(ws + 1, "web.log") && ls - live == ws - web &&
                        !strncmp(live, web, (size_t)(ls - live)),
                "web.log is named web.log and lives in miner.log's directory (%s), "
                "where POST /runlog looks it up",
                web);
    }

    {
        const char *live = MINER_LOG_PATH, *gen = MINER_LOG_PATH ".1";
        const char *ls = strrchr(live, '/'), *gs = strrchr(gen, '/');
        size_t      ln = ls ? (size_t)(ls - live) : 0, gn = gs ? (size_t)(gs - gen) : 0;
        CHECK(ls && gs && ln == gn && !strncmp(live, gen, ln),
                "the rotated generation shares the live log's DIRECTORY (%.*s), so "
                "it is on the same filesystem — the copy cannot spill onto another "
                "volume",
                (int)ln, live);
    }

    CHECK(strncmp(MINER_LOG_PATH, APPROOT, strlen(APPROOT)) != 0,
            "the log is NOT under the app volume %s — that mount holds the "
            "binaries, and its free space is irrelevant to this rotation",
            APPROOT);

    CHECK(MINER_LOG_CAP == 4u * 1024u * 1024u,
            "the in-run cap is %u B — the same 4194304 the init script checks at "
            "service start (avalon-miner.init): one threshold, two triggers",
            MINER_LOG_CAP);

    CHECK((unsigned long long)MINER_LOG_CAP * 2ull == 8ull * 1024 * 1024,
            "at rest the rotation holds 2 x %u MiB = 8 MiB on that filesystem — "
            "the figure to compare against free space",
            MINER_LOG_CAP >> 20);

    CHECK(AUX_LOG_CAP == 2u * 1024u * 1024u && AUX_LOG_CAP < MINER_LOG_CAP,
            "web.log and miner_op.log rotate at their own %u B cap, below "
            "miner.log's",
            AUX_LOG_CAP);

    unlink(tst_gen1);
    make_file(tst_live, CAP, 1);
    arm();
    fire();
    CHECK(fsize(tst_live) == (long)CAP && fsize(tst_gen1) == -1 && tst_truncates == 0 &&
                    tst_rot_lines == 0,
            "a log EXACTLY at the cap (%zu B) does NOT rotate — no .1, no "
            "truncate, no log line: the test is `<=`, so the cap itself stands",
            CAP);

    unlink(tst_gen1);
    make_file(tst_live, CAP - 1, 1);
    arm();
    fire();
    CHECK(fsize(tst_live) == (long)(CAP - 1) && fsize(tst_gen1) == -1 && tst_truncates == 0,
            "...and one byte under it (%zu B) does not either", CAP - 1);

    unlink(tst_gen1);
    make_file(tst_live, OVER, 2);
    arm();
    fire();
    CHECK(fsize(tst_live) == 0 && is_pat(tst_gen1, OVER, 2) && tst_truncates == 1 &&
                    tst_rot_lines == 1,
            "one byte OVER the cap (%zu B) is the smallest log that rotates — all "
            "%zu bytes reach .1 and the live file is left at 0",
            OVER, OVER);

    arm();
    fire();
    CHECK(fsize(tst_live) == 0 && tst_truncates == 0 && tst_rot_lines == 0,
            "the tick right after a rotation does nothing: a 0-byte log is under "
            "the cap, so rotation is self-limiting rather than latched");

    unlink(tst_live);
    unlink(tst_gen1);
    arm();
    fire();
    CHECK(fsize(tst_live) == -1 && fsize(tst_gen1) == -1 && tst_truncates == 0 &&
                    tst_rot_lines == 0,
            "an ABSENT log is a clean no-op — stat fails and the rule returns "
            "before fopen, so a missing /data cannot be made worse");

    unlink(tst_gen1);
    make_file(tst_live, WELL_OVER, 3);
    arm();
    fire();
    CHECK(is_pat(tst_gen1, WELL_OVER, 3),
            ".1 holds the log's %zu bytes EXACTLY — every byte, in order, as they "
            "stood before the truncate; a truncate-then-copy would leave this "
            "empty",
            WELL_OVER);
    CHECK(fsize(tst_live) == 0,
            "...and only THEN is the live log truncated to 0, which is what lets "
            "the three O_APPEND writers carry on at the new EOF");
    CHECK(fsize(tst_tmp) == -1 && tst_renames == 1,
            "the copy is staged in .1.tmp and renamed onto .1: no .1.tmp is left "
            "behind after a rotation");

    make_file(tst_gen1, WELL_OVER, 9);
    make_file(tst_live, OVER, 4);
    arm();
    fire();
    CHECK(is_pat(tst_gen1, OVER, 4),
            "an existing .1 is REPLACED wholesale — the older generation's %zu "
            "bytes are gone, neither appended to nor partially overwritten (.1 is "
            "now the current log's %zu)",
            WELL_OVER, OVER);

    snprintf(probe, sizeof probe, "%s/miner.log.2", tst_dir);
    CHECK(fsize(probe) == -1,
            "...and there is no .2: exactly one generation is kept, which is what "
            "bounds the resting cost at 2 x the cap");

    make_file(tst_gen1, WELL_OVER, 9);
    make_file(tst_live, OVER, 5);
    arm();
    tst_short_after = 0;
    fire();
    CHECK(is_pat(tst_gen1, WELL_OVER, 9) && fsize(tst_tmp) == -1 && is_pat(tst_live, OVER, 5) &&
                    tst_truncates == 0 && tst_renames == 0,
            "a rotation whose first write fails leaves the old .1 whole (%zu "
            "bytes), removes the partial .1.tmp and keeps the live log",
            WELL_OVER);

    make_file(tst_gen1, COPY_BLK, 9);
    make_file(tst_live, OVER, 6);
    arm();
    tst_fail_rb = 1;
    fire();
    CHECK(is_pat(tst_live, OVER, 6) && is_pat(tst_gen1, COPY_BLK, 9) && tst_truncates == 0 &&
                    tst_rot_lines == 0,
            "SOURCE UNREADABLE: the copy never happens (.1 still holds its OLD "
            "%u bytes) and the live log is NOT truncated — it stays whole for "
            "the next tick to retry",
            COPY_BLK);

    make_file(tst_gen1, COPY_BLK, 9);
    make_file(tst_live, OVER, 7);
    arm();
    tst_fail_wb = 1;
    fire();
    CHECK(is_pat(tst_live, OVER, 7) && is_pat(tst_gen1, COPY_BLK, 9) && tst_truncates == 0 &&
                    tst_rot_lines == 0,
            "DESTINATION UNOPENABLE (a full volume at create time): .1 is left at "
            "its stale %u bytes and the live log is kept whole, not truncated",
            COPY_BLK);

    unlink(tst_gen1);
    make_file(tst_live, OVER, 8);
    arm();
    tst_short_after = 1;
    fire();
    CHECK(fsize(tst_gen1) == -1 && fsize(tst_tmp) == -1 && is_pat(tst_live, OVER, 8) &&
                    tst_truncates == 0 && tst_rot_lines == 0,
            "PARTIAL COPY (ENOSPC mid-write after %u bytes): the partial .1.tmp "
            "is removed, no .1 appears, and the live log is NOT truncated — all "
            "%zu bytes stay in it",
            COPY_BLK, OVER);

    unlink(tst_gen1);
    make_file(tst_live, OVER, 10);
    arm();
    tst_fail_dst_close = 1;
    fire();
    CHECK(is_pat(tst_live, OVER, 10) && tst_truncates == 0 && tst_rot_lines == 0 &&
                    fsize(tst_tmp) == -1 && fsize(tst_gen1) == -1,
            "CLOSE FAILS: fclose(dst) is the call a buffered ENOSPC is reported "
            "on, so its failure keeps the live log whole and reports no rotation");

    unlink(tst_gen1);
    make_file(tst_live, OVER, 11);
    arm();
    tst_fail_truncate = 1;
    fire();
    CHECK(fsize(tst_live) == (long)OVER && is_pat(tst_gen1, OVER, 11) && tst_rot_lines == 0,
            "TRUNCATE FAILS: the only return value the rule checks — no success "
            "line, and the live log is left whole, so the next 60 s tick retries "
            "(and re-copies the same bytes over .1)");

    make_file(tst_gen1, COPY_BLK, 9);
    make_file(tst_live, OVER, 17);
    arm();
    tst_fail_rename = 1;
    fire();
    CHECK(is_pat(tst_gen1, COPY_BLK, 9) && fsize(tst_tmp) == -1 && is_pat(tst_live, OVER, 17) &&
                    tst_truncates == 0 && tst_rot_lines == 0 && tst_last_lvl == OSIF_LOG_WARN,
            "RENAME FAILS: the old .1 is kept, .1.tmp is removed and the live "
            "log is not truncated");

    make_file(tst_gen1, COPY_BLK, 9);
    make_file(tst_live, 2 * CAP, 18);
    arm();
    tst_fail_wb = 1;
    fire();
    CHECK(is_pat(tst_live, 2 * CAP, 18) && tst_truncates == 0 && tst_last_lvl == OSIF_LOG_WARN,
            "a failed copy at exactly 2 x the cap (%zu B) still keeps the live "
            "log whole",
            2 * CAP);

    make_file(tst_live, 2 * CAP + 1, 19);
    arm();
    tst_fail_wb = 1;
    fire();
    CHECK(fsize(tst_live) == 0 && tst_truncates == 1 && is_pat(tst_gen1, COPY_BLK, 9) &&
                    fsize(tst_tmp) == -1 && tst_rot_lines == 0 && tst_last_lvl == OSIF_LOG_ERR &&
                    strstr(tst_last_log, "truncated without rotation") != NULL,
            "one byte over 2 x the cap with the copy still failing: the live log "
            "is truncated anyway to free the space, the old .1 is kept, and the "
            "loss is logged as ERR (\"%s\")",
            tst_last_log);

    {
        long peak   = 0;
        int  forced = 0;
        unlink(tst_gen1);
        make_file(tst_live, OVER, 27);
        for (int i = 0; i < 12; i++) {
            FILE *f = fopen(tst_live, "ab");
            for (size_t j = 0; f && j < CAP / 2; j++)
                fputc('x', f);
            if (f)
                fclose(f);
            if (fsize(tst_live) > peak)
                peak = fsize(tst_live);
            arm();
            tst_short_after = 0;
            fire();
            forced += tst_truncates;
        }
        CHECK(peak <= (long)(2 * CAP + CAP / 2) && forced >= 3 &&
                        fsize(tst_live) <= (long)(2 * CAP) && fsize(tst_gen1) == -1 &&
                        fsize(tst_tmp) == -1,
                "a copy that keeps failing while the log grows by %zu B per tick "
                "never lets the live log pass 2 x the cap plus one tick's writes "
                "(peak %ld B, %d forced truncates over 12 ticks)",
                CAP / 2, peak, forced);
    }

    {
        int healthy, unreadable, unopenable, partial, closefail;

        unlink(tst_gen1);
        make_file(tst_live, OVER, 12);
        arm();
        fire();
        healthy = tst_rot_lines;
        CHECK(healthy == 1 && tst_last_lvl == OSIF_LOG_INFO &&
                        strstr(tst_last_log, "rotated in-run") != NULL,
                "a healthy rotation logs one INFO line (\"%s\")", tst_last_log);

        make_file(tst_live, OVER, 13);
        arm();
        tst_fail_rb = 1;
        fire();
        unreadable = tst_rot_lines == 0 && tst_last_lvl == OSIF_LOG_WARN &&
                strstr(tst_last_log, "not rotated") != NULL;

        make_file(tst_live, OVER, 14);
        arm();
        tst_fail_wb = 1;
        fire();
        unopenable = tst_rot_lines == 0 && tst_last_lvl == OSIF_LOG_WARN &&
                strstr(tst_last_log, "not rotated") != NULL;

        make_file(tst_live, OVER, 15);
        arm();
        tst_short_after = 1;
        fire();
        partial = tst_rot_lines == 0 && tst_last_lvl == OSIF_LOG_WARN &&
                strstr(tst_last_log, "not rotated") != NULL;

        make_file(tst_live, OVER, 16);
        arm();
        tst_fail_dst_close = 1;
        fire();
        closefail = tst_rot_lines == 0 && tst_last_lvl == OSIF_LOG_WARN &&
                strstr(tst_last_log, "not rotated") != NULL;

        CHECK(unreadable && unopenable && partial && closefail,
                "...and a failed copy logs a WARN \"not rotated\" line instead, "
                "never the success line, whether the source could not be read "
                "(%d), the destination could not be opened (%d), the copy ran "
                "out of room part way (%d) or the close failed (%d)",
                unreadable, unopenable, partial, closefail);
    }

    {
        struct stat wb, wa;
        int         ino_ok;

        unlink(tst_live);
        unlink(tst_gen1);
        unlink(tst_webgen1);
        make_file(tst_web, OVER, 20);
        arm();
        fire();
        CHECK(fsize(tst_web) == 0 && is_pat(tst_webgen1, OVER, 20) && tst_truncates == 1 &&
                        tst_rot_lines == 1 && fsize(tst_live) == -1,
                "an over-cap web log rotates while miner.log is ABSENT — the "
                "entries are independent, so the first file's stat failing cannot "
                "return out of the whole rule and leave the second unrotated");

        unlink(tst_web);
        unlink(tst_webgen1);
        unlink(tst_gen1);
        make_file(tst_live, OVER, 21);
        arm();
        fire();
        CHECK(fsize(tst_live) == 0 && is_pat(tst_gen1, OVER, 21) && fsize(tst_web) == -1 &&
                        fsize(tst_webgen1) == -1 && tst_truncates == 1 && tst_rot_lines == 1,
                "...and the other way round: miner.log rotates with the web log "
                "absent, and an absent second entry creates no .1 of its own");

        unlink(tst_gen1);
        unlink(tst_webgen1);
        make_file(tst_live, OVER, 22);
        make_file(tst_web, WELL_OVER, 23);
        arm();
        fire();
        CHECK(fsize(tst_live) == 0 && fsize(tst_web) == 0 && is_pat(tst_gen1, OVER, 22) &&
                        is_pat(tst_webgen1, WELL_OVER, 23) && tst_truncates == 2 &&
                        tst_rot_lines == 2,
                "BOTH over cap on ONE 60 s tick: two truncates, two \"rotated "
                "in-run\" lines, and each .1 holds its OWN file's bytes (the two "
                "differ in size AND content, so a copy that read the wrong source "
                "fails here rather than passing on equal lengths)");

        unlink(tst_live);
        unlink(tst_gen1);
        unlink(tst_webgen1);
        make_file(tst_web, OVER, 24);
        arm();
        fire();
        CHECK(tst_rot_lines == 1 &&
                        !strncmp(tst_last_log, "web.log rotated in-run",
                                sizeof "web.log rotated in-run" - 1),
                "the web log's INFO line NAMES THE WEB LOG (\"%s\") — the basename "
                "comes from strrchr on the entry's own path, so a literal "
                "miner.log in the body would say the wrong file here, "
                "and a name-not-name+1 slip would leave the leading '/'",
                tst_last_log);

        unlink(tst_web);
        unlink(tst_webgen1);
        unlink(tst_gen1);
        make_file(tst_live, OVER, 25);
        arm();
        fire();
        CHECK(tst_rot_lines == 1 &&
                        !strncmp(tst_last_log, "miner.log rotated in-run",
                                sizeof "miner.log rotated in-run" - 1),
                "...and miner.log's line still names miner.log (\"%s\"): the "
                "basename derivation did not change what the original entry "
                "reports",
                tst_last_log);

        unlink(tst_web);
        unlink(tst_webgen1);
        unlink(tst_gen1);
        make_file(tst_web, AUX_LOG_CAP, 27);
        make_file(tst_live, AUX_LOG_CAP + 1u, 28);
        arm();
        fire();
        CHECK(is_pat(tst_web, AUX_LOG_CAP, 27) && fsize(tst_webgen1) == -1 &&
                        is_pat(tst_live, AUX_LOG_CAP + 1u, 28) && fsize(tst_gen1) == -1 &&
                        tst_truncates == 0,
                "each log has its own cap: the web log AT its %u B cap and "
                "miner.log one byte over that are both left alone",
                AUX_LOG_CAP);

        unlink(tst_live);
        make_file(tst_web, AUX_LOG_CAP + 1u, 29);
        arm();
        fire();
        CHECK(fsize(tst_web) == 0 && is_pat(tst_webgen1, AUX_LOG_CAP + 1u, 29) &&
                        tst_truncates == 1,
                "...and the web log one byte over its own cap rotates");

        unlink(tst_web);
        unlink(tst_webgen1);
        make_file(tst_web, OVER, 26);
        ino_ok = stat(tst_web, &wb) == 0;
        arm();
        fire();
        CHECK(ino_ok && stat(tst_web, &wa) == 0 && wa.st_dev == wb.st_dev &&
                        wa.st_ino == wb.st_ino && wa.st_size == 0,
                "the web log is COPY-TRUNCATED, not renamed: same device and same "
                "inode across the rotation, now 0 bytes. miner_web holds that "
                "FILE* open for its whole process lifetime, so a rename would "
                "leave it appending to an unlinked inode for ever and the live "
                "file would never grow again");
        unlink(tst_web);
        unlink(tst_webgen1);
    }

    unlink(tst_live);
    unlink(tst_gen1);
    unlink(tst_web);
    unlink(tst_webgen1);
    unlink(tst_op);
    unlink(tst_opgen1);
    arm();
    tst_stats = 0;
    memset(tst_stat_at, 0, sizeof tst_stat_at);
    tst_now     = 0;
    tst_pass    = 0;
    tst_want    = 121;
    tst_sleeps  = 0;
    tst_runaway = 0;
    memset(&g_softonoff, 0, sizeof g_softonoff);
    memset(&g_enum_hold, 0, sizeof g_enum_hold);
    memset(&g_thermal_st, 0, sizeof g_thermal_st);
    memset(&g_fan_st, 0, sizeof g_fan_st);
    g_fan_st.fixed_duty = -1;
    g_overheat_req      = 0;
    g_api_req           = 0;
    g_run               = 1;
    mm_miner_main_unused();

    CHECK(!tst_runaway && tst_stats == 9,
            "the supervisor reaches logrot 3 times in 121 one-second passes — "
            "registered, and firing on its own clock rather than every tick; 9 "
            "stats because each invocation walks all three table entries (saw %d)",
            tst_stats);
    CHECK(tst_stat_at[0] == 0 && tst_stat_at[3] == 60000 && tst_stat_at[6] == 120000,
            "...at t = 0 / 60000 / 120000 ms: due on the first tick (framework.c "
            "sets next_due_ms = 0 at registration) and every 60 s after, so a log "
            "may exceed the cap by up to a minute's writes before it is noticed "
            "(saw %llu/%llu/%llu)",
            (unsigned long long)tst_stat_at[0], (unsigned long long)tst_stat_at[3],
            (unsigned long long)tst_stat_at[6]);
    CHECK(tst_stat_at[1] == tst_stat_at[0] && tst_stat_at[2] == tst_stat_at[0] &&
                    tst_stat_at[4] == tst_stat_at[3] && tst_stat_at[5] == tst_stat_at[3] &&
                    tst_stat_at[7] == tst_stat_at[6] && tst_stat_at[8] == tst_stat_at[6],
            "...and all three files are checked on the SAME tick, not on three "
            "clocks: one rule, one cadence, whatever the table holds");
    CHECK(fsize(tst_live) == -1 && fsize(tst_gen1) == -1 && fsize(tst_web) == -1 &&
                    fsize(tst_webgen1) == -1 && fsize(tst_op) == -1 && fsize(tst_opgen1) == -1,
            "...and with no log present the whole run wrote nothing");

    {
        int now_ok = stat(MINER_LOG_PATH, &after) == 0;
        int same   = (real_before_ok == now_ok) &&
                (!now_ok || (before.st_size == after.st_size && before.st_mtime == after.st_mtime));
        CHECK(same,
                "the REAL %s is unchanged across the whole suite (%s before, %s "
                "after) — every open, write and truncate went to the sandbox",
                MINER_LOG_PATH, had_real_log ? "present" : "absent", now_ok ? "present" : "absent");
    }

    tst_cleanup();
    tst_dir[0] = '\0';
    return tst_done();
}
