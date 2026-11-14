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

#include "core_ctrl.h"

#include "osif.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

extern osif_ipc_t *g_ipc_core_mm;

static pthread_mutex_t g_call_lock = PTHREAD_MUTEX_INITIALIZER; /* one command in flight */
static pthread_mutex_t g_ack_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ack_cond;
static pthread_once_t  g_ack_once = PTHREAD_ONCE_INIT;
static uint32_t        g_seq; /* seq of the command awaiting an answer, 0 none */
static int             g_answered;
static mc_ctrl_ack     g_ack;

static void ack_cond_init(void)
{
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_ack_cond, &attr);
    pthread_condattr_destroy(&attr);
}

void core_ctrl_ack(const void *data, int len)
{
    if (!data || len < (int)sizeof(mc_ctrl_ack))
        return;

    pthread_mutex_lock(&g_ack_lock);
    mc_ctrl_ack a;
    memcpy(&a, data, sizeof a);
    if (g_seq && a.seq == g_seq && !g_answered) {
        g_ack      = a;
        g_answered = 1;
        pthread_cond_signal(&g_ack_cond);
    }
    pthread_mutex_unlock(&g_ack_lock);
}

int core_ctrl_send(mc_ctrl_cmd *cmd, mc_ctrl_ack *ack)
{
    static uint32_t last;
    struct timespec ts;
    int             rc = 0;

    pthread_once(&g_ack_once, ack_cond_init);
    pthread_mutex_lock(&g_call_lock);

    if (++last == 0)
        last = 1;
    cmd->seq = last;
    pthread_mutex_lock(&g_ack_lock);
    g_seq      = last;
    g_answered = 0;
    pthread_mutex_unlock(&g_ack_lock);

    if (osif_ipc_send(g_ipc_core_mm, MSG_CTRL_CMD, cmd, (int)sizeof *cmd) != OSIF_OK) {
        rc = -1;
        goto out;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += CORE_CTRL_ACK_MS / 1000u;
    ts.tv_nsec += (long)(CORE_CTRL_ACK_MS % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_ack_lock);
    while (!g_answered && pthread_cond_timedwait(&g_ack_cond, &g_ack_lock, &ts) != ETIMEDOUT)
        ;
    if (g_answered)
        *ack = g_ack;
    else
        rc = -2;
    pthread_mutex_unlock(&g_ack_lock);

out:
    pthread_mutex_lock(&g_ack_lock);
    g_seq = 0;
    pthread_mutex_unlock(&g_ack_lock);
    pthread_mutex_unlock(&g_call_lock);
    return rc;
}
