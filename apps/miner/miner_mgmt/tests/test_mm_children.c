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
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mm_children.c"
#include "osif.h"

#include "../../tests/include/tst_check.h"

static pid_t spawn_obedient(unsigned after_ms)
{
    pid_t p = fork();
    if (p == 0) {
        osif_sleep_ms(after_ms);
        _exit(0);
    }
    return p;
}

static pid_t fork_sigterm_ignoring(void)
{
    void (*prev)(int) = signal(SIGTERM, SIG_IGN);
    pid_t p           = fork();
    if (p == 0)
        return 0;
    signal(SIGTERM, prev);
    return p;
}

static pid_t spawn_slow(unsigned after_ms, int donefd)
{
    pid_t p = fork_sigterm_ignoring();
    if (p == 0) {
        osif_sleep_ms(after_ms);
        if (donefd >= 0) {
            ssize_t w = write(donefd, "D", 1);
            (void)w;
        }
        _exit(0);
    }
    return p;
}

static pid_t spawn_stubborn(void)
{
    pid_t p = fork_sigterm_ignoring();
    if (p == 0) {
        for (;;)
            osif_sleep_ms(1000);
    }
    return p;
}

static int unreaped(pid_t p)
{
    return kill(p, 0) == 0;
}

int main(void)
{
    {
        pid_t s = spawn_stubborn();
        kill(s, SIGTERM);
        osif_sleep_ms(100);
        CHECK(unreaped(s) && waitpid(s, NULL, WNOHANG) == 0,
                "the stubborn fixture really does ignore SIGTERM");
        kill(s, SIGKILL);
        waitpid(s, NULL, 0);

        pid_t o = spawn_obedient(60000u);
        kill(o, SIGTERM);
        osif_sleep_ms(100);
        int st = 0;
        CHECK(waitpid(o, &st, WNOHANG) == o && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
                "...and the obedient fixture really does die of SIGTERM");
    }

    {
        pid_t    pids[2] = { spawn_obedient(30), spawn_obedient(60) };
        uint64_t t0      = osif_now_ms();
        int      left    = mm_children_stop(pids, 2, 3000u, 500u);
        uint64_t dt      = osif_now_ms() - t0;

        CHECK(left == 0, "both children joined (got %d still up)", left);
        CHECK(!unreaped(pids[0]) && !unreaped(pids[1]),
                "...and both are REAPED, not left as zombies");
        CHECK(dt < 1000u,
                "...and it returned as soon as they went, not at the bound "
                "(%llu ms)",
                (unsigned long long)dt);
    }

    {
        int pfd[2];
        if (pipe(pfd) != 0) {
            perror("pipe");
            return 1;
        }
        pid_t pids[2] = { spawn_obedient(10), spawn_slow(2400, pfd[1]) };
        close(pfd[1]);

        uint64_t t0   = osif_now_ms();
        int      left = mm_children_stop(pids, 2, 3000u, 500u);
        uint64_t dt   = osif_now_ms() - t0;

        char c   = 0;
        int  got = (int)read(pfd[0], &c, 1);
        close(pfd[0]);

        CHECK(left == 0,
                "a child that takes 2.4 s — past 20 x 100 ms — is "
                "still joined (got %d still up)",
                left);
        CHECK(dt >= 2400u, "...and the call WAITED for it (%llu ms, not giving up at ~2000 ms)",
                (unsigned long long)dt);
        CHECK(got == 1 && c == 'D',
                "...and it ran its OWN shutdown to completion rather than being "
                "killed part-way through it");
    }

    {
        pid_t    pids[2] = { spawn_obedient(10), spawn_stubborn() };
        uint64_t t0      = osif_now_ms();
        int      left    = mm_children_stop(pids, 2, 200u, 300u);
        uint64_t dt      = osif_now_ms() - t0;

        CHECK(left == 0,
                "a SIGTERM-proof child is escalated to SIGKILL and joined anyway "
                "(got %d still up)",
                left);
        CHECK(!unreaped(pids[1]), "...and it is reaped, not left a zombie");
        CHECK(dt >= 200u,
                "...only AFTER the SIGTERM budget was spent (%llu ms >= 200) — "
                "SIGKILL is the fallback, not the first move",
                (unsigned long long)dt);
        CHECK(dt < 1500u,
                "...and the whole call stayed inside its bound (%llu ms) — a hang "
                "here would hold the rail hot and starve the watchdog",
                (unsigned long long)dt);

        if (unreaped(pids[1])) {
            kill(pids[1], SIGKILL);
            waitpid(pids[1], NULL, 0);
        }
    }

    {
        pid_t other = spawn_obedient(10);
        osif_sleep_ms(120);
        pid_t named = spawn_slow(400, -1);
        int   left  = mm_children_stop(&named, 1, 2000u, 200u);

        CHECK(left == 0, "the named child joined (got %d)", left);
        CHECK(!unreaped(named),
                "...and the NAMED child is REALLY gone when the call returns, not "
                "merely credited with an unrelated zombie's exit");
        CHECK(waitpid(other, NULL, WNOHANG) == other,
                "...and the UNRELATED child was neither signalled nor consumed");
    }

    {
        pid_t pids[3] = { 0, -1, spawn_obedient(10) };
        int   left    = mm_children_stop(pids, 3, 1000u, 200u);
        CHECK(left == 0, "0 and -1 slots are skipped and the real child joins (got %d)", left);
    }

    {
        pid_t pids[2] = { 0, 0 };
        CHECK(mm_children_stop(pids, 2, 1000u, 200u) == 0,
                "no children at all is 0 (clear to cut the rail)");
    }

    return tst_done();
}
