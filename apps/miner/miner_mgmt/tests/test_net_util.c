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
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "net_util.h"
#include "../../tests/include/tst_check.h"

#define CALLS        300 /* Number of timed calls made while the spawner thread runs */
#define SPAWN_GAP_MS 1   /* Pause between two spawns of the background spawner thread, ms */
#define CALL_MAX_MS  150 /* "true" runs in a few ms; a held pipe waits ~300 */

extern char **environ;

static volatile int stop; /* Set non-zero to make the spawner thread exit */

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void *spawner(void *arg)
{
    pid_t live[512];
    int   n    = 0;
    char *av[] = { "sleep", "0.3", NULL };

    (void)arg;
    while (!stop) {
        pid_t p;

        for (int i = 0; i < n;)
            if (waitpid(live[i], NULL, WNOHANG) == live[i])
                live[i] = live[--n];
            else
                i++;
        if (n < (int)(sizeof live / sizeof live[0]) &&
                posix_spawnp(&p, "sleep", NULL, NULL, av, environ) == 0)
            live[n++] = p;
        sleep_ms(SPAWN_GAP_MS);
    }
    while (n > 0)
        waitpid(live[--n], NULL, 0);
    return NULL;
}

static void test_hex_codec(void)
{
    char src[] = { 'a', (char)0xfe, 0x00, 0x1b };
    char d[16];

    printf("str_to_hex / hex_to_str\n");

    CHECK(str_to_hex(src, 4, d, 9) == 0 && !strcmp(d, "61FE001B"),
            "2.1 str_to_hex writes uppercase and a NUL (got \"%s\")", d);
    memset(d, 'x', sizeof d);
    CHECK(str_to_hex(src, 4, d, 8) == -1 && d[0] == 'x',
            "2.2 str_to_hex refuses a cap one short of 2l+1 and writes nothing");
    CHECK(str_to_hex(src, 0, d, 1) == 0 && d[0] == '\0', "2.3 str_to_hex of 0 bytes is \"\"");

    memset(d, 'x', sizeof d);
    CHECK(hex_to_str("41a2", 4, d, 3) == 0 && !memcmp(d, "A\xa2", 3),
            "2.4 hex_to_str decodes either case and adds a NUL");
    memset(d, 'x', sizeof d);
    CHECK(hex_to_str("4142F", 5, d, 3) == 0 && !memcmp(d, "AB", 3),
            "2.5 hex_to_str drops an odd tail");
    memset(d, 'x', sizeof d);
    CHECK(hex_to_str("4142", 4, d, 2) == -1 && d[0] == 'x',
            "2.6 hex_to_str refuses a cap with no room for the NUL");
    memset(d, 'x', sizeof d);
    CHECK(hex_to_str("41g2", 4, d, 16) == -1 && d[0] == 'x' && d[1] == 'x',
            "2.7 hex_to_str leaves d untouched on a non-hex char");
    memset(d, 'x', sizeof d);
    CHECK(hex_to_str("4142Fz", 5, d, 16) == 0 && !memcmp(d, "AB", 3),
            "2.8 hex_to_str ignores the dropped tail character");
    memset(d, 'x', sizeof d);
    CHECK(hex_to_str("410042", 6, d, 16) == 0 && !memcmp(d, "A\0B", 4),
            "2.9 hex_to_str decodes a 0x00 byte");
}

static int count_in_file(int fd, const char *needle)
{
    char    buf[8192];
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    int     c = 0;

    if (n < 0)
        return -1;
    buf[n] = '\0';
    for (const char *p = buf; (p = strstr(p, needle)) != NULL; p++)
        c++;
    return c;
}

static void test_exec_err_relog(void)
{
    const char *const fail[] = { "sh", "-c", "echo boom >&2; exit 1", NULL };
    const char *const ok[]   = { "sh", "-c", "true", NULL };
    const char       *needle = "Network command sh failed (exit 1): boom";
    char              tmpl[] = "/tmp/test_net_util_XXXXXX";
    char              out[16];
    int               fd    = mkstemp(tmpl);
    int               saved = dup(STDERR_FILENO);

    printf("exec_argv: a failure that recurs after a success is logged again\n");

    CHECK(fd >= 0 && saved >= 0, "3.0 capture file and stderr copy");
    if (fd < 0 || saved < 0)
        return;
    unlink(tmpl);
    fflush(stderr);
    dup2(fd, STDERR_FILENO);

    exec_argv(fail, out, sizeof out);
    fflush(stderr);
    int first = count_in_file(fd, needle);
    exec_argv(fail, out, sizeof out);
    fflush(stderr);
    int repeat = count_in_file(fd, needle);
    exec_argv(ok, out, sizeof out);
    exec_argv(fail, out, sizeof out);
    fflush(stderr);
    int again = count_in_file(fd, needle);

    dup2(saved, STDERR_FILENO);
    close(saved);
    close(fd);

    CHECK(first == 1, "3.1 the first failure is logged (%d)", first);
    CHECK(repeat == 1, "3.2 the same failure repeated is not logged again (%d)", repeat);
    CHECK(again == 2, "3.3 after a success the same failure is logged again (%d)", again);
}

static void test_exec_timeout(void)
{
    const char *const hang[]  = { "sh", "-c", "echo early; exec sleep 5", NULL };
    const char *const quick[] = { "echo", "ok", NULL };
    char              out[32];
    long              t0, took;
    int               rc;

    printf("exec_argv_timeout: a hung command is killed at the deadline\n");

    t0   = now_ms();
    rc   = exec_argv_timeout(hang, out, sizeof out, 200);
    took = now_ms() - t0;
    CHECK(rc == -1 && took < 1000,
            "4.1 a command still running at 200 ms returns -1 at once (rc %d, %ld ms)", rc, took);
    CHECK(!strcmp(out, "early\n"), "4.2 output written before the deadline is kept (got \"%s\")",
            out);
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && ECHILD == errno, "4.3 the killed child is reaped");

    CHECK(exec_argv_timeout(quick, out, sizeof out, 1000) == 0 && !strcmp(out, "ok\n"),
            "4.4 a command that ends before the deadline returns 0 with its output");
}

static void test_exec_status(void)
{
    const char *const three[]   = { "sh", "-c", "echo out; exit 3", NULL };
    const char *const killed[]  = { "sh", "-c", "kill -9 $$", NULL };
    const char *const missing[] = { "/nonexistent/cmd", NULL };
    char              out[32];
    int               rc;

    printf("exec_argv: the child's exit status is returned\n");

    rc = exec_argv(three, out, sizeof out);
    CHECK(rc == 3 && !strcmp(out, "out\n"), "5.1 exit 3 returns 3 with the output (rc %d, \"%s\")",
            rc, out);
    rc = exec_argv(killed, out, sizeof out);
    CHECK(rc == -1, "5.2 a child killed by a signal returns -1 (rc %d)", rc);
    rc = exec_argv(missing, out, sizeof out);
    CHECK(rc == 127, "5.3 a command that cannot be run returns 127 (rc %d)", rc);
}

int main(void)
{
    const char *const argv[] = { "echo", "ok", NULL };
    char              out[16];
    long              worst = 0;
    pthread_t         tid;

    test_hex_codec();
    test_exec_err_relog();
    test_exec_timeout();
    test_exec_status();

    printf("exec_argv: a concurrent spawn cannot hold the output pipe open\n");

    CHECK(exec_argv(argv, out, sizeof out) == 0 && !strcmp(out, "ok\n"),
            "1.1 stdout is captured (got \"%s\")", out);

    CHECK(pthread_create(&tid, NULL, spawner, NULL) == 0, "1.2 spawner thread starts");
    for (int i = 0; i < CALLS; i++) {
        long t0 = now_ms();

        exec_argv(argv, out, sizeof out);
        if (now_ms() - t0 > worst)
            worst = now_ms() - t0;
    }
    stop = 1;
    pthread_join(tid, NULL);
    CHECK(worst < CALL_MAX_MS,
            "1.3 %d calls beside a spawning thread each finish fast (worst %ld ms)", CALLS, worst);
    CHECK(!strcmp(out, "ok\n"), "1.4 stdout still captured under load");

    return tst_done();
}
