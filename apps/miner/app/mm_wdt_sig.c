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

#define _GNU_SOURCE
#include "mm_wdt_sig.h"
#include "board_sensors.h"
#include "mc_api_bootby.h"
#include "mm_cfgstore.h"
#include "mm_internal.h"
#include "mm_system.h"
#include "osif.h"
#include "wdt_flags.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define MM_WDT_SIG_BT_MAX   32          /* most stack frames written on SIGSEGV */
#define MM_WDT_SIG_ALTSTACK (64 * 1024) /* alternate signal stack size, bytes */
#define MM_WDT_SIG_SAVE_MS  5000        /* longest fatal exit wait for the save thread, ms */

static char g_wdt_sig_altstack[MM_WDT_SIG_ALTSTACK]; /* main thread's stack for fatal handlers */

/* Packed gate (wdt_flags_pack()) last published by mm_wdt_thread; read in signal context */
static volatile sig_atomic_t g_wdt_sig_gate;

/* 1 once a fatal or failed-pretimeout exit has claimed the save sequence; atomic exchange */
static volatile sig_atomic_t g_wdt_sig_dying;

/* 1: mm_cfg_flush() already ran on this exit, so the fatal exit rewrites the store only */
static volatile sig_atomic_t g_wdt_sig_cfg_saved;

static int            g_wdt_sig_req[2]  = { -1, -1 };
static int            g_wdt_sig_done[2] = { -1, -1 };
static volatile pid_t g_wdt_sig_saver_pid;

void mm_wdt_sig_usr2_set(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGUSR2);
}

void mm_wdt_sig_publish(uint32_t gate)
{
    g_wdt_sig_gate = (sig_atomic_t)gate;
}

static size_t mm_wdt_sig_dec(char *dst, int v)
{
    char   tmp[12];
    size_t n = 0;
    size_t i;

    if (v <= 0) {
        dst[0] = '0';
        return 1;
    }
    while (v > 0 && n < sizeof tmp) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];
    return n;
}

static void mm_wdt_sig_report(const char *pre, size_t prelen, int sig, const char *missing)
{
    static const char mid[] = ", WDT gate missing: ";
    char              line[192];
    const char       *p   = line;
    size_t            len = prelen;
    size_t            n;

    memcpy(line, pre, len);
    if (sig >= 0)
        len += mm_wdt_sig_dec(line + len, sig);
    memcpy(line + len, mid, sizeof mid - 1);
    len += sizeof mid - 1;

    n = strlen(missing);
    if (n > sizeof line - len - 1)
        n = sizeof line - len - 1;
    memcpy(line + len, missing, n);
    len += n;
    line[len++] = '\n';

    while (len > 0) {
        ssize_t w = write(STDERR_FILENO, p, len);
        if (w <= 0)
            return;
        p += w;
        len -= (size_t)w;
    }
}

static void mm_wdt_sig_missing(uint32_t gate, char *buf, size_t cap)
{
    wdt_flags_t f;

    wdt_flags_unpack(&f, gate);
    wdt_flags_missing(&f, buf, cap);
}

static void mm_wdt_sig_backtrace(void)
{
    void *bt[MM_WDT_SIG_BT_MAX];
    int   n;

    n = backtrace(bt, MM_WDT_SIG_BT_MAX);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
}

static void mm_wdt_sig_stop(int sig)
{
    g_sig = sig;
    g_run = 0;
}

static void mm_wdt_sig_save(int sig)
{
    (void)board_power_rail_off();
    (void)mm_bootby_stage(MC_BOOTBY_SYS, (uint32_t)sig);
    if (!g_wdt_sig_cfg_saved) {
        g_wdt_sig_cfg_saved = 1;
        mm_cfg_flush();
    } else {
        (void)mm_cfgstore_flush();
    }
    sync();
}

static void *mm_wdt_sig_saver(void *arg)
{
    int     sig;
    ssize_t n;

    (void)arg;
    do
        n = read(g_wdt_sig_req[0], &sig, sizeof sig);
    while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof sig)
        mm_wdt_sig_save(sig);
    n = write(g_wdt_sig_done[1], "", 1);
    (void)n;
    return NULL;
}

static void mm_wdt_sig_die(int sig)
{
    if (!__atomic_exchange_n(&g_wdt_sig_dying, 1, __ATOMIC_SEQ_CST)) {
        if (g_wdt_sig_saver_pid == getpid() &&
                write(g_wdt_sig_req[1], &sig, sizeof sig) == (ssize_t)sizeof sig) {
            struct pollfd pfd = { .fd = g_wdt_sig_done[0], .events = POLLIN };

            while (poll(&pfd, 1, MM_WDT_SIG_SAVE_MS) < 0 && errno == EINTR)
                ;
        } else {
            mm_wdt_sig_save(sig);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void mm_wdt_sig_saver_start(void)
{
    sigset_t  all, old;
    pthread_t tid;

    g_wdt_sig_saver_pid = 0;
    if (pipe2(g_wdt_sig_req, O_CLOEXEC) != 0)
        return;
    if (pipe2(g_wdt_sig_done, O_CLOEXEC) != 0)
        return;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    if (pthread_create(&tid, NULL, mm_wdt_sig_saver, NULL) == 0) {
        pthread_detach(tid);
        g_wdt_sig_saver_pid = getpid();
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void mm_wdt_sig_handler(int sig)
{
    static const char pre[] = "[ERR] mm_miner: fatal signal ";
    char              missing[64];

    mm_wdt_sig_missing((uint32_t)g_wdt_sig_gate, missing, sizeof missing);
    mm_wdt_sig_report(pre, sizeof pre - 1, sig, missing);
    if (sig == SIGSEGV)
        mm_wdt_sig_backtrace();
    mm_wdt_sig_die(sig);
}

void mm_wdt_sig_pretimeout(void)
{
    static const char pre[] = "[ERR] mm_miner: WDT pretimeout";
    char              missing[64];
    uint32_t          gate = (uint32_t)g_wdt_sig_gate;
    wdt_flags_t       f;
    sigset_t          usr2;

    mm_wdt_sig_missing(gate, missing, sizeof missing);
    mm_wdt_sig_report(pre, sizeof pre - 1, -1, missing);
    (void)board_power_rail_off();
    wdt_flags_unpack(&f, gate);
    osif_log(OSIF_LOG_OP, "WDT timeout from %s; rebooting (0x%08x of 0x%08x)",
            missing[0] ? missing : "(none)", f.mask, f.enabled);
    g_wdt_sig_cfg_saved = 1;
    (void)mm_system_reboot(MC_BOOTBY_WDT, gate);
    mm_wdt_sig_die(SIGUSR2);
    mm_wdt_sig_usr2_set(&usr2);
    pthread_sigmask(SIG_UNBLOCK, &usr2, NULL);
}

void mm_wdt_sig_install(void)
{
    static const int fatal[] = { SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS };
    struct sigaction sa;
    sigset_t         usr2;
    stack_t          ss;
    void            *bt[1];
    size_t           i;

    signal(SIGPIPE, SIG_IGN);
    mm_wdt_sig_saver_start();

    (void)backtrace(bt, 1);
    memset(&ss, 0, sizeof ss);
    ss.ss_sp   = g_wdt_sig_altstack;
    ss.ss_size = sizeof g_wdt_sig_altstack;
    sigaltstack(&ss, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = mm_wdt_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    for (i = 0; i < sizeof fatal / sizeof fatal[0]; i++)
        sigaction(fatal[i], &sa, NULL);

    mm_wdt_sig_usr2_set(&usr2);
    pthread_sigmask(SIG_BLOCK, &usr2, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = mm_wdt_sig_stop;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}
