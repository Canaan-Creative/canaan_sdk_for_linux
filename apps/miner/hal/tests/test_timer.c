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
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "timer.h"
#include "tst_board_expect.h"
#include "tst_tach.h"

#define open tst_tach_open /* routes timer.c tachometer opens to the test nodes */
#include "../platform/linux/timer.c"
#undef open

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: " __VA_ARGS__); \
            printf("\n"); \
            fails++; \
        } else { \
            printf("  ok: " __VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static int               fails;
static const char *const g_dir = TST_TACH_ROOT; /* directory holding the fake tachometer nodes */
static char              g_cap[192];            /* file stderr is captured to */
static char        g_slot[WANT_SLOTS][160];     /* fake tachometer node path per declared counter */
static int         g_saved = -1;                /* saved stderr descriptor, -1 when not capturing */
static char        g_out[8192];                 /* captured stderr text, NUL-terminated */
static char        g_copy[8192];                /* copy of the captured text split into lines */
static const char *g_line[32];                  /* start of each line in g_copy, first 32 */
static int         g_lines;

static void point_slot(int i)
{
    snprintf(g_slot[i], sizeof g_slot[i], "%s/tach%d", g_dir, i);
}

static void make_counter(int i, unsigned int hz)
{
    FILE *f = fopen(g_slot[i], "wb");
    assert(f);
    size_t w = fwrite(&hz, sizeof hz, 1, f);
    assert(w == 1);
    fclose(f);
}

static void cap_begin(void)
{
    fflush(stderr);
    g_saved = dup(STDERR_FILENO);
    assert(g_saved >= 0);
    int fd = open(g_cap, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    int rc = dup2(fd, STDERR_FILENO);
    assert(rc >= 0);
    close(fd);
}

static const char *cap_end(void)
{
    fflush(stderr);
    int rc = dup2(g_saved, STDERR_FILENO);
    assert(rc >= 0);
    close(g_saved);
    g_saved  = -1;
    FILE  *f = fopen(g_cap, "r");
    size_t n = f ? fread(g_out, 1, sizeof g_out - 1, f) : 0;
    if (f)
        fclose(f);
    g_out[n] = '\0';
    return g_out;
}

static void split_lines(const char *out)
{
    g_lines = 0;
    snprintf(g_copy, sizeof g_copy, "%s", out);
    char *p = g_copy;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        if (g_lines < 32)
            g_line[g_lines++] = p;
        if (!nl)
            break;
        p = nl + 1;
    }
}

static const char *err_body(const char *line)
{
    const char *t = strstr(line, "] [ERR] ");
    return t ? t + 8 : NULL;
}

static int names_slot(const char *body, int slot, const char *path)
{
    const char *at = strstr(body, path);
    if (!at)
        return 0;
    char   rest[512], idx[8];
    size_t head = (size_t)(at - body);
    if (head >= sizeof rest)
        return 0;
    memcpy(rest, body, head);
    snprintf(rest + head, sizeof rest - head, "%s", at + strlen(path));
    snprintf(idx, sizeof idx, "%d", slot);
    return strstr(rest, idx) != NULL;
}

static int err_count(void)
{
    int n = 0;
    for (int i = 0; i < g_lines; i++)
        if (err_body(g_line[i]))
            n++;
    return n;
}

static int slot_reports(int slot)
{
    int n = 0;
    for (int i = 0; i < g_lines; i++) {
        const char *b = err_body(g_line[i]);
        if (b && names_slot(b, slot, board_fans[slot].tach_node))
            n++;
    }
    return n;
}

static void scenario_dead(void)
{
    for (int i = 0; i < WANT_SLOTS; i++)
        point_slot(i);

    cap_begin();
    int         rc  = timer_init(1);
    const char *out = cap_end();

    CHECK(rc == -1, "%s: no counter node opens -> timer_init returns -1", BOARD_NAME);
    split_lines(out);
    CHECK(err_count() == WANT_SLOTS, "%s: %d dead counter(s) reported at ERR, want %d", BOARD_NAME,
            err_count(), WANT_SLOTS);
    for (int s = 0; s < WANT_SLOTS; s++)
        CHECK(slot_reports(s) == 1, "slot %d reported once, naming index and node %s", s,
                board_fans[s].tach_node);

    cap_begin();
    int         rc2  = timer_init(1);
    const char *out2 = cap_end();
    CHECK(rc2 == -1, "the retry still returns -1");
    split_lines(out2);
    CHECK(err_count() == WANT_SLOTS, "the retry reports %d dead counter(s) at ERR, want %d",
            err_count(), WANT_SLOTS);
    for (int s = 0; s < WANT_SLOTS; s++)
        CHECK(slot_reports(s) == 1, "the retry reports slot %d once", s);

    make_counter(0, 43);
    cap_begin();
    int          rc3  = timer_init(1);
    const char  *out3 = cap_end();
    unsigned int v    = 0;
    CHECK(rc3 == 0, "1 of %d counter(s) open -> timer_init returns 0", WANT_SLOTS);
    CHECK(get_timer_count(&v, 0) == (int)sizeof v && v == 43, "slot 0 counts (%u Hz)", v);
    if (WANT_SLOTS > 1) {
        unsigned int dead = 7;
        CHECK(get_timer_count(&dead, 1) == -1, "the still-dead slot 1 reports no reading");
    }
    split_lines(out3);
    CHECK(err_count() == WANT_SLOTS - 1, "the third call reports %d dead counter(s), want %d",
            err_count(), WANT_SLOTS - 1);
    CHECK(slot_reports(0) == 0, "the third call does not report the opened slot 0");
    for (int s = 1; s < WANT_SLOTS; s++)
        CHECK(slot_reports(s) == 1, "the third call reports the still-dead slot %d once", s);
    unlink(g_slot[0]);
}

static void scenario_healthy(void)
{
    for (int i = 0; i < WANT_SLOTS; i++) {
        point_slot(i);
        make_counter(i, 100 + (unsigned int)i);
    }

    cap_begin();
    int         rc  = timer_init(1);
    const char *out = cap_end();

    CHECK(rc == 0, "%s: all %d declared counter(s) open -> 0", BOARD_NAME, WANT_SLOTS);
    CHECK(out[0] == '\0', "a healthy counter set logs NOTHING (got %zu bytes)", strlen(out));
    for (int i = 0; i < WANT_SLOTS; i++) {
        unsigned int v = 0;
        int          n = get_timer_count(&v, i);
        CHECK(n == (int)sizeof v && v == 100 + (unsigned int)i,
                "slot %d reads its OWN counter (%u)", i, v);
    }
    for (int i = 0; i < WANT_SLOTS; i++)
        unlink(g_slot[i]);
}

int main(void)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    int clean = system(cmd);
    assert(clean == 0);
    int mkrc = mkdir(g_dir, 0755);
    assert(mkrc == 0);
    snprintf(g_cap, sizeof g_cap, "%s/stderr", g_dir);
    unsetenv("MINER_LOG_LEVEL");

    printf("board: %s (%d tach counter(s))\n", BOARD_NAME, WANT_SLOTS);

    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        scenario_dead();
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
    int   st = 0;
    pid_t w  = waitpid(pid, &st, 0);
    assert(w == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "dead-counter scenario passed (exit %d)",
            WEXITSTATUS(st));

    scenario_healthy();

    unlink(g_cap);
    rmdir(g_dir);
    if (fails) {
        printf("test_timer: %d FAIL\n", fails);
        return 1;
    }
    printf("test_timer: all pass (%s, %d slot(s))\n", BOARD_NAME, WANT_SLOTS);
    return 0;
}
