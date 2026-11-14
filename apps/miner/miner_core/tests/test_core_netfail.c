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
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "osif.h"
#include "miner_ipc.h"
#include "stratum_net.h"

/* Simulated wall clock returned by time(), s since the epoch */
static time_t fake_now = 1800000000;
/* test_connect() result; in-session time() calls with g_stats_lock free; in a session; runs */
static int connect_error, unlocked_updates, in_session, connection_number;
static int user_drop; /* Non-zero makes test_run() flag a user drop */

static time_t test_time(time_t *result);
static int    test_connect(stratum_transport *transport, const char *host, int port)
{
    (void)host;
    (void)port;
    memset(transport, 0, sizeof *transport);
    return connect_error;
}
static void test_close(stratum_transport *transport)
{
    (void)transport;
}
static int    test_run(sv1_client *client, stratum_transport *transport, const char *user,
           const char *pass, const char *agent, stratum_on_event on_event, void *context);
osif_status_t osif_ipc_init(osif_ipc_t **handle, osif_ipc_recv_cb callback, const char *path,
        int server)
{
    (void)callback;
    (void)path;
    (void)server;
    *handle = NULL;
    return OSIF_OK;
}
osif_status_t osif_ipc_send(osif_ipc_t *handle, uint32_t type, const void *data, int length)
{
    (void)handle;
    (void)type;
    (void)data;
    (void)length;
    return OSIF_OK;
}
void osif_log(osif_loglvl_t level, const char *format, ...)
{
    (void)level;
    (void)format;
}
void osif_sleep_ms(uint32_t milliseconds)
{
    (void)milliseconds;
}

#define time                test_time
#define stratum_tcp_connect test_connect
#define stratum_tcp_close   test_close
#define stratum_run         test_run
#define main                unused_core_main
#include "core_miner_tu.h"
#undef main
#undef stratum_run
#undef stratum_tcp_close
#undef stratum_tcp_connect
#undef time

static time_t test_time(time_t *result)
{
    if (in_session) {
        int status = pthread_mutex_trylock(&g_stats_lock);
        if (!status) {
            unlocked_updates++;
            pthread_mutex_unlock(&g_stats_lock);
        } else
            assert(status == EBUSY);
    }
    if (result)
        *result = fake_now;
    return fake_now;
}

static int test_run(sv1_client *client, stratum_transport *transport, const char *user,
        const char *pass, const char *agent, stratum_on_event on_event, void *context)
{
    (void)client;
    (void)transport;
    (void)user;
    (void)pass;
    (void)agent;
    (void)on_event;
    (void)context;
    if (connection_number) {
        int previous = ((connection_number - 1) % 4) * 2;
        assert(g_netfail_time[previous + 1] == (uint32_t)fake_now);
    }
    connection_number++;
    fake_now += 100;
    if (user_drop)
        g_stratum.user_drop = 1;
    return STRATUM_END_EOF;
}

int main(void)
{
    pool_set pools;
    pool_init(&pools, 1, 1);
    assert(pool_add(&pools, "stratum+tcp://example.invalid:3333", "worker", "x") == 0);
    publish_pools(&pools);
    mc_pipeline pipeline;
    mc_pipeline_init(&pipeline);
    for (int cycle = 0; cycle < 6; cycle++) {
        pool_mark_alive(&pools, 0);
        in_session = 1;
        pool_session_once(&pools, &pipeline);
        in_session = 0;
        int slot   = (cycle % 4) * 2;
        assert(g_pool_failcnt == (uint32_t)cycle + 1);
        assert(g_netfail_time[slot] == (uint32_t)fake_now);
        assert(g_netfail_time[slot + 1] == 0);
        mc_api_stats snapshot;
        api_snapshot(&snapshot, NULL);
        assert(snapshot.pool_failcnt == g_pool_failcnt);
        assert(!memcmp(snapshot.netfail_time, g_netfail_time, sizeof g_netfail_time));
        fake_now += 50;
    }
    connect_error = -1;
    pool_mark_alive(&pools, 0);
    pool_session_once(&pools, &pipeline);
    assert(g_pool_failcnt == 6);
    assert(g_netfail_time[3] == 0);
    assert(unlocked_updates == 0);

    connect_error     = 0;
    connection_number = 0;
    user_drop         = 1;
    uint32_t ring[8];
    memcpy(ring, g_netfail_time, sizeof ring);
    int open_slot       = g_netfail_index;
    ring[open_slot + 1] = (uint32_t)fake_now;
    pool_mark_alive(&pools, 0);
    pool_session_once(&pools, &pipeline);
    int next_slot       = (open_slot + 2) % 8;
    ring[next_slot]     = (uint32_t)fake_now;
    ring[next_slot + 1] = 0;
    assert(g_pool_failcnt == 7);
    assert(!memcmp(g_netfail_time, ring, sizeof ring));
    puts("pool disconnect/reconnect ring and locking passed");
    return 0;
}
