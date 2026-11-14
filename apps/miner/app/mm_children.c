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

#include "mm_spawn.h"
#include "osif.h"

#include <signal.h>
#include <sys/wait.h>

#define MM_CHILDREN_MAX 4
#define JOIN_POLL_MS    20u /* child reap poll interval, ms */

static int reap(pid_t *live, int n)
{
    int kept = 0;
    for (int i = 0; i < n; i++) {
        pid_t r = waitpid(live[i], NULL, WNOHANG);
        if (r == 0)
            live[kept++] = live[i];
    }
    return kept;
}

static int join(pid_t *live, int n, unsigned budget_ms)
{
    uint64_t deadline = osif_now_ms() + budget_ms;
    for (;;) {
        n = reap(live, n);
        if (n == 0)
            return 0;
        if (osif_now_ms() >= deadline)
            return n;
        osif_sleep_ms(JOIN_POLL_MS);
    }
}

static void signal_all(const pid_t *live, int n, int sig)
{
    for (int i = 0; i < n; i++)
        kill(live[i], sig);
}

int mm_children_stop(const pid_t *pids, int n, unsigned term_ms, unsigned kill_ms)
{
    pid_t live[MM_CHILDREN_MAX];
    int   nlive = 0;

    for (int i = 0; i < n && nlive < MM_CHILDREN_MAX; i++)
        if (pids[i] > 0)
            live[nlive++] = pids[i];
    if (nlive == 0)
        return 0;

    signal_all(live, nlive, SIGTERM);
    nlive = join(live, nlive, term_ms);
    if (nlive == 0)
        return 0;

    osif_log(OSIF_LOG_WARN,
            "%d child process(es) did not stop within %u ms "
            "— killing them",
            nlive, term_ms);
    signal_all(live, nlive, SIGKILL);
    nlive = join(live, nlive, kill_ms);
    if (nlive)
        osif_log(OSIF_LOG_ERR,
                "%d child process(es) still not gone after being "
                "killed",
                nlive);
    return nlive;
}
