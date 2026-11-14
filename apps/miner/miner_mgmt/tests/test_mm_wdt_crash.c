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
#include "board_sensors.h"
#include "ini.h"
#include "mc_api_bootby.h"
#include "mm_cfgstore.h"
#include "mm_system.h"
#include "mm_wdt_sig.h"
#include "osif.h"
#include "webauth.h"
#include "mc_paths.h"
#include "../../tests/include/tst_check.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#undef MC_SYSTEMCFG_DEFAULT
/* Overridden to a scratch systemcfg.ini in the build tree */
#define MC_SYSTEMCFG_DEFAULT TST_BIN_DIR "/wdtsig/systemcfg.ini"
/* Rail-off observation: "reason subreason" at board_power_rail_off() */
#define RAIL_OBS_PATH MC_SYSTEMCFG_DEFAULT ".railobs"
/* Reboot observation: "reason subreason", subreason -1 if no config flush came first */
#define REBOOT_OBS_PATH MC_SYSTEMCFG_DEFAULT ".rebootobs"
/* First config flush observation: "rail_was_off reason" */
#define CFG_OBS_PATH MC_SYSTEMCFG_DEFAULT ".cfgobs"
/* First OP log observation: "rail_was_off cfg_was_flushed" */
#define LOG_OBS_PATH  MC_SYSTEMCFG_DEFAULT ".logobs"
#define CFGN_OBS_PATH MC_SYSTEMCFG_DEFAULT ".cfgnobs" /* Config flush count: "count 0" */
#define NEST_OBS_PATH MC_SYSTEMCFG_DEFAULT ".nestobs" /* Present: mm_cfg_flush() raises SIGSEGV */

#include "mm_cfgstore.c"

volatile sig_atomic_t g_run = 1;
volatile sig_atomic_t g_sig;             /* Signal that cleared g_run, 0 none */
static volatile int   g_overflow_go = 1; /* Non-zero lets stack_overflow() keep recursing */
static int            g_hold_cfg_lock;

static void tst_cleanup(void)
{
    unlink(MC_SYSTEMCFG_DEFAULT);
    unlink(MC_SYSTEMCFG_DEFAULT ".tmp");
    unlink(MC_SYSTEMCFG_DEFAULT ".lock");
    unlink(RAIL_OBS_PATH);
    unlink(REBOOT_OBS_PATH);
    unlink(CFG_OBS_PATH);
    unlink(LOG_OBS_PATH);
    unlink(CFGN_OBS_PATH);
    unlink(NEST_OBS_PATH);
    rmdir(MC_SYSTEMCFG_DEFAULT);
}

static void tst_bail(const char *why)
{
    fprintf(stderr, "  FAIL: %s\n", why);
    tst_cleanup();
    exit(2);
}

static int ini_exists(void)
{
    struct stat st;
    return stat(MC_SYSTEMCFG_DEFAULT, &st) == 0 && S_ISREG(st.st_mode);
}

static long ini_num(const char *path, const char *sect, const char *key)
{
    ini_t      *ini = ini_load(path);
    const char *v;
    long        n = -1;

    if (!ini)
        return -1;
    v = ini_get(ini, sect, key);
    if (v)
        n = strtol(v, NULL, 0);
    ini_free(ini);
    return n;
}

static long rsn(void)
{
    return ini_num(MC_SYSTEMCFG_DEFAULT, MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_REASON);
}
static long sub(void)
{
    return ini_num(MC_SYSTEMCFG_DEFAULT, MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_SUB);
}

static void ini_put(const char *text)
{
    FILE *f = fopen(MC_SYSTEMCFG_DEFAULT, "w");

    if (!f || fputs(text, f) == EOF || fclose(f) != 0)
        tst_bail("cannot write the sandbox systemcfg.ini");
}

static void obs_put(const char *path, long a, long b)
{
    char line[64];
    int  n, wfd;

    n   = snprintf(line, sizeof line, "%ld %ld\n", a, b);
    wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (wfd >= 0) {
        if (n > 0) {
            ssize_t w = write(wfd, line, (size_t)n);
            (void)w;
        }
        close(wfd);
    }
}

static int obs_get(const char *path, long *a, long *b)
{
    char  line[64];
    FILE *f = fopen(path, "r");

    if (!f)
        return 0;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return sscanf(line, "%ld %ld", a, b) == 2;
}

int board_power_rail_off(void)
{
    obs_put(RAIL_OBS_PATH, rsn(), sub());
    return 0;
}

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)fmt;
    if (lvl == OSIF_LOG_OP && access(LOG_OBS_PATH, F_OK) != 0)
        obs_put(LOG_OBS_PATH, access(RAIL_OBS_PATH, F_OK) == 0, access(CFG_OBS_PATH, F_OK) == 0);
}

void mm_cfg_flush(void)
{
    long n = 0, z = 0;

    (void)mm_cfgstore_flush();
    if (access(CFG_OBS_PATH, F_OK) != 0)
        obs_put(CFG_OBS_PATH, access(RAIL_OBS_PATH, F_OK) == 0, rsn());
    (void)obs_get(CFGN_OBS_PATH, &n, &z);
    obs_put(CFGN_OBS_PATH, n + 1, 0);
    if (access(NEST_OBS_PATH, F_OK) == 0)
        raise(SIGSEGV);
}

int mm_system_reboot(uint8_t reason, uint32_t subr)
{
    mm_cfg_flush();
    obs_put(REBOOT_OBS_PATH, reason, access(CFG_OBS_PATH, F_OK) == 0 ? (long)subr : -1L);
    return -1;
}

static void __attribute__((noinline)) stack_overflow(volatile char *prev)
{
    volatile char pad[1024];

    pad[0] = (char)(prev[0] + 1);
    if (g_overflow_go)
        stack_overflow(pad);
    pad[1] = pad[0];
}

static int crash_run(int sig, int overflow, int errfd)
{
    pid_t p;
    int   st = 0;

    fflush(NULL);
    p = fork();
    if (p < 0)
        tst_bail("fork");
    if (p == 0) {
        struct rlimit rl;
        rl.rlim_cur = 0;
        rl.rlim_max = 0;
        (void)setrlimit(RLIMIT_CORE, &rl);
        if (errfd >= 0 && dup2(errfd, STDERR_FILENO) < 0)
            _exit(4);
        (void)mm_cfgstore_open(MC_SYSTEMCFG_DEFAULT);
        mm_wdt_sig_install();
        if (sig == SIGUSR2) {
            struct timespec none = { 0, 0 };
            sigset_t        usr2;

            sigemptyset(&usr2);
            sigaddset(&usr2, SIGUSR2);
            kill(getpid(), SIGUSR2);
            if (sigtimedwait(&usr2, NULL, &none) == SIGUSR2)
                mm_wdt_sig_pretimeout();
            _exit(3);
        }
        if (overflow) {
            volatile char seed[1] = { 0 };

            stack_overflow(seed);
        }
        if (g_hold_cfg_lock)
            pthread_mutex_lock(&g_store.lock);
        raise(sig);
        _exit(3);
    }
    if (waitpid(p, &st, 0) != p)
        tst_bail("waitpid");
    return st;
}

static int crash_child(int sig)
{
    return crash_run(sig, 0, -1);
}

static int crash_capture(int sig, int overflow, char *out, size_t cap)
{
    size_t len = 0;
    int    fds[2];
    int    st;

    if (pipe(fds) != 0)
        tst_bail("pipe");
    st = crash_run(sig, overflow, fds[1]);
    close(fds[1]);
    while (len < cap - 1) {
        ssize_t r = read(fds[0], out + len, cap - 1 - len);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    out[len] = '\0';
    close(fds[0]);
    return st;
}

static int count_lines(const char *s)
{
    int n = 0;

    for (; *s; s++)
        if (*s == '\n')
            n++;
    return n;
}

static int died_of(int st, int sig)
{
    return WIFSIGNALED(st) && WTERMSIG(st) == sig;
}

int main(void)
{
    struct stat st;
    sigset_t    mask;
    char        err[8192];
    long        hr = -1, hs = -1;
    int         wst;

    tst_cleanup();

    if (mkdir(MC_SYSTEMCFG_DEFAULT, 0700) != 0)
        tst_bail("cannot create the unwritable-path fixture");
    wst = crash_child(SIGSEGV);
    CHECK(died_of(wst, SIGSEGV),
            "with systemcfg.ini UNWRITABLE the fatal handler still runs and still "
            "re-raises — the child dies of SIGSEGV exactly as it would with a "
            "writable one, so a missing /data/userconfig cannot change how a "
            "crash ends");
    CHECK(stat(MC_SYSTEMCFG_DEFAULT, &st) == 0 && S_ISDIR(st.st_mode),
            "...and nothing replaced the path: the persist fails and is ignored");
    if (rmdir(MC_SYSTEMCFG_DEFAULT) != 0)
        tst_bail("cannot remove the unwritable-path fixture");

    CHECK(!ini_exists(), "the sandbox starts with no systemcfg.ini");
    mm_wdt_sig_install();
    CHECK(!ini_exists(),
            "installing the handlers writes NOTHING: the boot reason is written "
            "when a signal actually arrives");
    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR2) == 1,
            "...and it leaves SIGUSR2 blocked in the calling thread, so every "
            "thread created after it inherits the block and the pretimeout stays "
            "pending until a thread takes it with sigtimedwait");

    wst = crash_child(SIGSEGV);
    CHECK(died_of(wst, SIGSEGV), "the child really dies of SIGSEGV");
    CHECK(rsn() == MC_BOOTBY_SYS && sub() == SIGSEGV,
            "...and the handler persisted [%s] %s/%s = %u/%u into systemcfg.ini "
            "(saw %ld/%ld)",
            MC_BOOTBY_INI_SECT, MC_BOOTBY_INI_REASON, MC_BOOTBY_INI_SUB, (unsigned)MC_BOOTBY_SYS,
            (unsigned)SIGSEGV, rsn(), sub());

    wst = crash_child(SIGABRT);
    CHECK(died_of(wst, SIGABRT), "a child raised with SIGABRT dies of SIGABRT");
    CHECK(rsn() == MC_BOOTBY_SYS && sub() == SIGABRT,
            "...and the record names SIGABRT (%u), not the SIGSEGV the case above "
            "left behind: the last writer of the key wins (saw %ld/%ld)",
            (unsigned)SIGABRT, rsn(), sub());

    ini_put("[syscfg]\nreason = 13\nsubreason = 0\nledmode = 3\n"
            "[hwcfg]\nwebpass = 8c6976e5\n");
    wst = crash_child(SIGFPE);
    CHECK(died_of(wst, SIGFPE), "the child dies of SIGFPE");
    CHECK(rsn() == MC_BOOTBY_SYS && sub() == SIGFPE,
            "a crash OVERWRITES an already-persisted reason — MC_BOOTBY_UPGRADE "
            "(%u) becomes SYS/%u: the crash "
            "write is simply the last writer to the same key (saw %ld/%ld)",
            (unsigned)MC_BOOTBY_UPGRADE, (unsigned)SIGFPE, rsn(), sub());
    CHECK(ini_num(MC_SYSTEMCFG_DEFAULT, "syscfg", "ledmode") == 3 &&
                    ini_num(MC_SYSTEMCFG_DEFAULT, "hwcfg", "webpass") != -1,
            "...and every OTHER key survives it: the handler writes the settings "
            "store's RAM copy of systemcfg.ini, which holds every other key, so a "
            "crash cannot cost the board its web password");

    unlink(RAIL_OBS_PATH);
    unlink(CFG_OBS_PATH);
    unlink(MC_SYSTEMCFG_DEFAULT);
    wst = crash_child(SIGSEGV);
    CHECK(died_of(wst, SIGSEGV), "a crashing child dies of SIGSEGV");
    CHECK(obs_get(RAIL_OBS_PATH, &hr, &hs) && hr == -1 && hs == -1,
            "the rail is cut BEFORE the ini is written (the rail-off sees no "
            "reason key, saw %ld/%ld): the rail cut is never queued behind a "
            "/data write",
            hr, hs);
    CHECK(rsn() == MC_BOOTBY_SYS && sub() == SIGSEGV,
            "...and by the time the process is gone the reason IS written");
    CHECK(obs_get(CFG_OBS_PATH, &hr, &hs) && hr == 1 && hs == MC_BOOTBY_SYS,
            "...and the config save after the rail cut writes the staged reason "
            "with the pending config in one write "
            "(saw rail %ld, reason %ld)",
            hr, hs);

    unlink(RAIL_OBS_PATH);
    unlink(MC_SYSTEMCFG_DEFAULT);
    wst = crash_capture(SIGSEGV, 0, err, sizeof err);
    CHECK(died_of(wst, SIGSEGV) && count_lines(err) >= 3 && strstr(err, "test_mm_wdt_crash") &&
                    strstr(err, "[0x"),
            "a SIGSEGV writes the call stack to stderr after the report line: "
            "several frames naming this binary with their addresses "
            "(saw %d lines)",
            count_lines(err));
    CHECK(rsn() == MC_BOOTBY_SYS && sub() == SIGSEGV,
            "...and the backtrace does not stop the persist that follows it");

    wst = crash_capture(SIGABRT, 0, err, sizeof err);
    CHECK(died_of(wst, SIGABRT) && count_lines(err) == 1 && strstr(err, "fatal signal") &&
                    !strstr(err, "[0x"),
            "every other fatal signal reports one line and no call stack: only "
            "SIGSEGV prints it (saw %d lines)",
            count_lines(err));

    unlink(RAIL_OBS_PATH);
    unlink(CFG_OBS_PATH);
    unlink(MC_SYSTEMCFG_DEFAULT);
    wst = crash_capture(SIGSEGV, 1, err, sizeof err);
    CHECK(died_of(wst, SIGSEGV), "a child that overflows its stack dies of SIGSEGV");
    CHECK(obs_get(RAIL_OBS_PATH, &hr, &hs) && rsn() == MC_BOOTBY_SYS && sub() == SIGSEGV &&
                    obs_get(CFG_OBS_PATH, &hr, &hs),
            "...and the handler still ran on the alternate signal stack: the "
            "rail is cut, SYS/%u is persisted and the config is saved, none of "
            "which could run on the exhausted thread stack",
            (unsigned)SIGSEGV);
    CHECK(strstr(err, "fatal signal") && count_lines(err) >= 3 && strstr(err, "[0x"),
            "...and it reports the signal and prints the call stack from there "
            "too (saw %d lines)",
            count_lines(err));

    unlink(RAIL_OBS_PATH);
    unlink(CFG_OBS_PATH);
    unlink(MC_SYSTEMCFG_DEFAULT);
    g_hold_cfg_lock = 1;
    {
        struct timespec t0, t1;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        wst = crash_child(SIGSEGV);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        CHECK(died_of(wst, SIGSEGV) && t1.tv_sec - t0.tv_sec < 30,
                "a crash while the crashing thread holds the settings lock does "
                "not hang: the child still dies of SIGSEGV (after %lld s)",
                (long long)(t1.tv_sec - t0.tv_sec));
    }
    g_hold_cfg_lock = 0;
    CHECK(obs_get(RAIL_OBS_PATH, &hr, &hs) && !ini_exists() && obs_get(CFG_OBS_PATH, &hr, &hs) == 0,
            "...the rail is still cut, and the record that needs the held lock "
            "is skipped instead of waited for");

    mm_wdt_sig_publish(0x00070005u);
    unlink(RAIL_OBS_PATH);
    unlink(REBOOT_OBS_PATH);
    unlink(CFG_OBS_PATH);
    unlink(LOG_OBS_PATH);
    unlink(MC_SYSTEMCFG_DEFAULT);
    wst = crash_child(SIGUSR2);
    CHECK(obs_get(REBOOT_OBS_PATH, &hr, &hs) && hr == MC_BOOTBY_WDT && hs == 0x00070005L,
            "a pending SIGUSR2 taken with sigtimedwait reboots through "
            "mm_wdt_sig_pretimeout: mm_system_reboot gets "
            "MC_BOOTBY_WDT (%u) and the published gate 0x00070005 as subreason, "
            "after the config was saved (saw %ld/0x%lx)",
            (unsigned)MC_BOOTBY_WDT, hr, (unsigned long)hs);
    CHECK(obs_get(CFG_OBS_PATH, &hr, &hs) && hr == 1 && hs == -1,
            "...and that config save runs after the rail cut, and the pretimeout "
            "path wrote no reason of its own before it (saw rail %ld, reason %ld)",
            hr, hs);
    CHECK(obs_get(LOG_OBS_PATH, &hr, &hs) && hr == 1 && hs == 0,
            "...and the OP line naming the missing reports is logged after the "
            "rail cut and before the config save (saw rail %ld, config %ld)",
            hr, hs);
    CHECK(died_of(wst, SIGUSR2) && obs_get(RAIL_OBS_PATH, &hr, &hs) && rsn() == MC_BOOTBY_SYS &&
                    sub() == SIGUSR2,
            "...and a reboot() that returns falls through to the fatal exit: "
            "the rail is cut, SYS/%u is persisted and the child dies of SIGUSR2",
            (unsigned)SIGUSR2);

    unlink(RAIL_OBS_PATH);
    unlink(REBOOT_OBS_PATH);
    unlink(CFG_OBS_PATH);
    unlink(CFGN_OBS_PATH);
    unlink(LOG_OBS_PATH);
    unlink(MC_SYSTEMCFG_DEFAULT);
    obs_put(NEST_OBS_PATH, 1, 0);
    wst = crash_child(SIGUSR2);
    unlink(NEST_OBS_PATH);
    hr = -1;
    CHECK(died_of(wst, SIGSEGV) && obs_get(CFGN_OBS_PATH, &hr, &hs) && hr == 1 &&
                    rsn() == MC_BOOTBY_SYS && sub() == SIGSEGV,
            "a SIGSEGV raised inside the pretimeout config save takes the fatal "
            "path, which persists SYS/%u and does NOT save the config a second "
            "time: the child dies of SIGSEGV after exactly one save (saw %ld)",
            (unsigned)SIGSEGV, hr);

    g_run = 1;
    g_sig = 0;
    unlink(RAIL_OBS_PATH);
    unlink(CFG_OBS_PATH);
    unlink(MC_SYSTEMCFG_DEFAULT);
    raise(SIGTERM);
    CHECK(g_run == 0 && g_sig == SIGTERM && !ini_exists() &&
                    obs_get(RAIL_OBS_PATH, &hr, &hs) == 0 && obs_get(CFG_OBS_PATH, &hr, &hs) == 0,
            "SIGTERM only stops the main loop: g_run is cleared and g_sig names "
            "the signal, with no rail cut and no persist from the handler — the "
            "orderly shutdown does both");

    g_run = 1;
    g_sig = 0;
    raise(SIGINT);
    CHECK(g_run == 0 && g_sig == SIGINT && !ini_exists() && obs_get(RAIL_OBS_PATH, &hr, &hs) == 0 &&
                    obs_get(CFG_OBS_PATH, &hr, &hs) == 0,
            "SIGINT is handled the same way: g_run is cleared and g_sig names "
            "SIGINT, with no rail cut and no persist from the handler");

    tst_cleanup();
    return tst_done();
}
