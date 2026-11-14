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
#include "net_util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "osif.h"
#include "fault_report.h"
#include "hex.h"

#define EXEC_ERR_CAP 256 /* child stderr capture buffer size, bytes */

static fault_report_t g_exec_err_rep; /* dedup state for the command-failure WARN */

static uint32_t exec_err_code(const char *cmd, const char *msg)
{
    uint32_t h = 2166136261u;

    while (*cmd)
        h = (h ^ (unsigned char)*cmd++) * 16777619u;
    while (*msg)
        h = (h ^ (unsigned char)*msg++) * 16777619u;

    return h ? h : 1u;
}

static void exec_err_report(const char *cmd, char *msg, int status)
{
    fault_report_verdict v;
    int                  n;

    for (n = 0; msg[n]; n++) {
        unsigned char c = (unsigned char)msg[n];
        if ('\n' == c || '\r' == c || '\t' == c)
            msg[n] = ' ';
        else if (c < 0x20 || c > 0x7e)
            msg[n] = '?';
    }
    while (n > 0 && ' ' == msg[n - 1])
        msg[--n] = '\0';
    if (0 == n) {
        fault_report_step(&g_exec_err_rep, 0);
        return;
    }

    v = fault_report_step(&g_exec_err_rep, exec_err_code(cmd, msg));
    if (FAULT_REPORT_CHANGED == v)
        osif_log(OSIF_LOG_WARN, "Network command %s failed (exit %d): %s", cmd, status, msg);
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int exec_argv(const char *const argv[], char *res, int res_size)
{
    return exec_argv_timeout(argv, res, res_size, -1);
}

int exec_argv_timeout(const char *const argv[], char *res, int res_size, int timeout_ms)
{
    int           fd[2], efd[2];
    pid_t         pid;
    int           used = 0, eused = 0, status = 0, expired = 0, code;
    char          err[EXEC_ERR_CAP];
    char          drop[256];
    struct pollfd pf[2];
    int64_t       deadline = 0;

    if (NULL == argv || NULL == argv[0] || NULL == res || res_size <= 0)
        return -1;

    memset(res, 0, (size_t)res_size);
    err[0] = '\0';

    /*
     * Close-on-exec, so that a long-lived process another thread forks before
     * fd[1] is closed below cannot hold the write end open and hang poll().
     */
    if (pipe2(fd, O_CLOEXEC) != 0)
        return -1;
    if (pipe2(efd, O_CLOEXEC) != 0) {
        close(fd[0]);
        close(fd[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        close(efd[0]);
        close(efd[1]);
        return -1;
    }

    if (0 == pid) {
        close(fd[0]);
        close(efd[0]);
        if (dup2(fd[1], STDOUT_FILENO) < 0 || dup2(efd[1], STDERR_FILENO) < 0)
            _exit(127);
        /* dup2() onto itself keeps close-on-exec; clear it on 1 and 2 */
        if (fcntl(STDOUT_FILENO, F_SETFD, 0) < 0 || fcntl(STDERR_FILENO, F_SETFD, 0) < 0)
            _exit(127);
        if (fd[1] != STDOUT_FILENO && fd[1] != STDERR_FILENO)
            close(fd[1]);
        if (efd[1] != STDOUT_FILENO && efd[1] != STDERR_FILENO)
            close(efd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(fd[1]);
    close(efd[1]);

    pf[0].fd     = fd[0];
    pf[1].fd     = efd[0];
    pf[0].events = pf[1].events = POLLIN;
    if (timeout_ms >= 0)
        deadline = mono_ms() + timeout_ms;

    while (pf[0].fd >= 0 || pf[1].fd >= 0) {
        int i, wait_ms = -1, pr;

        if (timeout_ms >= 0) {
            int64_t left = deadline - mono_ms();

            if (left <= 0) {
                expired = 1;
                break;
            }
            wait_ms = (int)left;
        }
        pf[0].revents = pf[1].revents = 0;
        pr                            = poll(pf, 2, wait_ms);
        if (pr < 0) {
            if (EINTR == errno)
                continue;
            break;
        }
        if (0 == pr)
            continue;

        for (i = 0; i < 2; i++) {
            char   *buf = (0 == i) ? res : err;
            int    *len = (0 == i) ? &used : &eused;
            int     cap = (0 == i) ? res_size : (int)sizeof(err);
            ssize_t n;

            if (pf[i].fd < 0 || 0 == pf[i].revents)
                continue;

            if (*len < cap - 1) {
                n = read(pf[i].fd, buf + *len, (size_t)(cap - 1 - *len));
                if (n > 0)
                    *len += (int)n;
            } else {
                n = read(pf[i].fd, drop, sizeof(drop));
            }
            if (n > 0)
                continue;
            if (n < 0 && EINTR == errno)
                continue;

            close(pf[i].fd);
            pf[i].fd = -1;
        }
    }

    if (pf[0].fd >= 0)
        close(pf[0].fd);
    if (pf[1].fd >= 0)
        close(pf[1].fd);

    res[used]  = '\0';
    err[eused] = '\0';

    if (expired) {
        kill(pid, SIGKILL);
        snprintf(err, sizeof(err), "timed out after %d ms", timeout_ms);
    }

    while (waitpid(pid, &status, 0) < 0 && EINTR == errno)
        ;

    code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exec_err_report(argv[0], err, code);

    return expired ? -1 : code;
}

int str_to_hex(char *s, int l, char *d, int dcap)
{
    if (NULL == s || NULL == d || l < 0 || dcap <= 0)
        return -1;

    if (l > (dcap - 1) / 2)
        return -1;

    hex_encode_upper(d, s, (size_t)l);
    return 0;
}

int hex_to_str(char *s, int l, char *d, int dcap)
{
    int n;

    if (NULL == s || NULL == d || l < 0 || dcap <= 0)
        return -1;

    n = l / 2;
    if (n + 1 > dcap)
        return -1;

    if (!hex_valid(s, (size_t)n * 2))
        return -1;

    hex_decode((uint8_t *)d, s, (size_t)n);
    d[n] = '\0';
    return 0;
}
