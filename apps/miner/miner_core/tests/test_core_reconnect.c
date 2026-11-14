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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osif.h"
#include "miner_ipc.h"
#include "stratum_net.h"
#include "pool.h"
#include "pipeline.h"

#include "../../tests/include/tst_check.h"

#define SLEEP_MAX 8 /* osif_sleep_ms() durations kept */

static char     g_log[2048];
static size_t   g_log_len;
static uint32_t g_slept[SLEEP_MAX]; /* Durations passed to osif_sleep_ms(), ms */
static int      g_slept_n;          /* osif_sleep_ms() calls, including those not kept */

static int      g_connect_rc;     /* Return of stratum_tcp_connect() */
static int      g_end;            /* End reason stratum_run() returns */
static int      g_established;    /* Value stratum_session_established() returns */
static int      g_got_job;        /* Non-zero makes a scripted session deliver a usable job */
static int      g_probe_up;       /* Non-zero makes a probed pool send a job */
static int      g_probe_n;        /* stratum_run() calls made by the pool probe */
static uint32_t g_probe_rx;       /* active pool rx lines counted during a probe */
static int      g_disable_active; /* Non-zero makes stratum_run() disable the active pool */
static int g_disable_on_connect;  /* Non-zero makes stratum_tcp_connect() disable the active pool */
static int g_run_n;               /* stratum_run() calls */
static int g_shutdown_n;          /* stratum_tcp_shutdown() calls */
/* sv1_clear_inflight() calls; those made after the session was dropped */
static int g_clear_n, g_clear_after_drop;
static int g_cfgwr_rc; /* Return of cfgwr_update_pools() */

static pool_set    g_ps;   /* Pools of the scripted session */
static mc_pipeline g_pipe; /* Pipeline passed to the scripted session */

osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server)
{
    (void)cb;
    (void)path;
    (void)is_server;
    if (h)
        *h = NULL;
    return OSIF_OK;
}
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len)
{
    (void)h;
    (void)type;
    (void)data;
    (void)len;
    return OSIF_OK;
}
void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;

    if (lvl != OSIF_LOG_DBG || g_log_len >= sizeof g_log - 1)
        return;
    va_start(ap, fmt);
    int n = vsnprintf(g_log + g_log_len, sizeof g_log - g_log_len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof g_log - 1 - g_log_len) {
        g_log_len = sizeof g_log - 1;
        return;
    }
    g_log_len += (size_t)n;
    g_log[g_log_len++] = '\n';
    g_log[g_log_len]   = '\0';
}
void osif_sleep_ms(uint32_t ms)
{
    if (g_slept_n < SLEEP_MAX)
        g_slept[g_slept_n] = ms;
    g_slept_n++;
}
static void disable_active(void);

void stratum_set_trace(stratum_trace_fn fn)
{
    (void)fn;
}
void stratum_trace_line(int tx, const char *line)
{
    (void)tx;
    (void)line;
}
const char *stratum_end_str(int reason)
{
    (void)reason;
    return "scripted";
}
int stratum_session_established(const sv1_client *c)
{
    (void)c;
    return g_established;
}
int stratum_parse_url(const char *url, char *host, size_t host_cap, int *port)
{
    (void)url;
    snprintf(host, host_cap, "pool.test");
    *port = 3333;
    return 0;
}
int stratum_tcp_connect(stratum_transport *t, const char *host, int port)
{
    (void)host;
    (void)port;
    if (t)
        memset(t, 0, sizeof *t);
    if (g_disable_on_connect)
        disable_active();
    return g_connect_rc;
}
void stratum_tcp_close(stratum_transport *t)
{
    (void)t;
}
void stratum_tcp_shutdown(stratum_transport *t)
{
    (void)t;
    g_shutdown_n++;
}

uint32_t test_sv1_clear_inflight(sv1_client *c, double *diff);

/* Routes the product's sv1_clear_inflight() through the counting wrapper */
#define sv1_clear_inflight test_sv1_clear_inflight
/* Routes the product's cfgwr_update_pools() to the scripted result */
#define cfgwr_update_pools test_cfgwr_update_pools
#define main               core_miner_main_unused
#include "core_miner_tu.h"
#undef main
#undef cfgwr_update_pools
#undef sv1_clear_inflight

int stratum_run(sv1_client *c, stratum_transport *t, const char *user, const char *pass,
        const char *user_agent, stratum_on_event on_event, void *ctx)
{
    (void)t;
    (void)user;
    (void)pass;
    (void)user_agent;
    if (on_event != on_sv1_event) {
        sv1_event ev = { .type = SV1_EV_NOTIFY };
        int       pa = core_pool_active();
        uint32_t  rx = pa >= 0 ? g_pool_st[pa].rxcnt : 0;

        g_probe_n++;
        pool_trace(0, "{\"method\":\"mining.notify\"}");
        g_probe_rx += pa >= 0 ? g_pool_st[pa].rxcnt - rx : 0;
        if (!g_probe_up)
            return STRATUM_END_TIMEOUT;
        return on_event(ctx, c, &ev) ? STRATUM_END_STOPPED : STRATUM_END_EOF;
    }
    g_run_n++;
    if (g_got_job)
        session_got_job(ctx, core_pool_active());
    if (g_disable_active)
        disable_active();
    return g_end;
}

uint32_t test_sv1_clear_inflight(sv1_client *c, double *diff)
{
    g_clear_n++;
    if (!g_stratum.t && !g_stratum.have_job && g_dead_seq == g_job_seq)
        g_clear_after_drop++;
    return sv1_clear_inflight(c, diff);
}

int test_cfgwr_update_pools(const char *path, pool_config_update_fn update, void *ctx)
{
    (void)path;
    (void)update;
    (void)ctx;
    return g_cfgwr_rc;
}

static void disable_active(void)
{
    char         buf[512];
    mc_api_stats st;
    memset(&st, 0, sizeof st);
    disablepool_handler("disablepool|0", &st, 0, buf, sizeof buf, NULL);
}

static void script_session(int npools, int connect_rc, int end, int established)
{
    pool_init(&g_ps, 1000, 60000);
    for (int i = 0; i < npools; i++) {
        char url[64], user[16];
        snprintf(url, sizeof url, "stratum+tcp://p%d:3333", i);
        snprintf(user, sizeof user, "u%d", i);
        pool_add(&g_ps, url, user, "x");
    }
    g_connect_rc  = connect_rc;
    g_end         = end;
    g_established = established;
    g_got_job     = established;
    g_slept_n     = 0;
}

int main(void)
{
    mc_pipeline_init(&g_pipe);

    script_session(2, 0, STRATUM_END_EOF, 0);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.pools[0].alive == 0, "session ended un-authorised -> pool 0 dead (alive=%d)",
            g_ps.pools[0].alive);
    CHECK(g_pool_st[0].dead == 1, "...and the API shadow agrees with the pool_set");
    CHECK(g_pool_st[0].getfail == 0, "...without a Get Failure: the session was never up (%u)",
            g_pool_st[0].getfail);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.active == 1, "...so the next iteration picks pool 1 (picked %d)", g_ps.active);

    script_session(2, 0, STRATUM_END_EOF, 1);
    g_pool_st[0].dead = 0;
    uint32_t gf       = g_pool_st[0].getfail;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.pools[0].alive == 1,
            "session ended after authorise -> pool 0 stays a candidate (alive=%d)",
            g_ps.pools[0].alive);
    CHECK(g_pool_st[0].dead == 0, "...and the API keeps it Alive while it reconnects (dead=%d)",
            g_pool_st[0].dead);
    CHECK(g_pool_st[0].getfail == gf + 1, "...while the interruption is one Get Failure (%u -> %u)",
            gf, g_pool_st[0].getfail);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.active == 0, "...and the next iteration re-picks it (picked %d)", g_ps.active);

    {
        mc_api_stats st;
        char        *line;

        script_session(1, 0, STRATUM_END_EOF, 1);
        mc_pipeline_zero_stats(&g_pipe);
        uint32_t ps0 = g_pool_st[0].stale;
        double   pd0 = g_pool_st[0].dstale;
        line         = sv1_build_submit(&g_pipe.sv1, "u0", "j", "00", "00", "00", NULL);
        free(line);
        sv1_inflight_set_diff(g_pipe.sv1.next_id - 1, 4096.0);
        line = sv1_build_submit(&g_pipe.sv1, "u0", "j", "01", "00", "00", NULL);
        free(line);
        sv1_inflight_set_diff(g_pipe.sv1.next_id - 1, 2048.0);
        pool_session_once(&g_ps, &g_pipe);
        api_snapshot(&st, &g_pipe);
        CHECK(st.stale == 2, "shares in flight when the session ends are summary Stale (%u)",
                st.stale);
        CHECK(g_pool_st[0].stale == ps0 + 2 && g_pool_st[0].dstale == pd0 + 6144.0,
                "...and pool 0's Stale and Difficulty Stale (%u, %.0f)", g_pool_st[0].stale - ps0,
                g_pool_st[0].dstale - pd0);
        script_session(1, 0, STRATUM_END_EOF, 1);
        line = sv1_build_submit(&g_pipe.sv1, "u0", "j", "02", "00", "00", NULL);
        free(line);
        sv1_inflight_set_diff(g_pipe.sv1.next_id - 1, 512.0);
        pool_session_once(&g_ps, &g_pipe);
        api_snapshot(&st, &g_pipe);
        CHECK(st.stale == 3, "...and accumulate across sessions (%u)", st.stale);
        CHECK(g_pool_st[0].stale == ps0 + 3 && g_pool_st[0].dstale == pd0 + 6656.0,
                "...per pool as well (%u, %.0f)", g_pool_st[0].stale - ps0,
                g_pool_st[0].dstale - pd0);
    }

    script_session(1, 0, STRATUM_END_TIMEOUT, 1);
    pool_session_once(&g_ps, &g_pipe);
    pool_session_once(&g_ps, &g_pipe);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_slept_n == 3, "three sessions -> three backoff waits (%d)", g_slept_n);
    CHECK(g_slept[0] == 1000 && g_slept[1] == 1000 && g_slept[2] == 1000,
            "each authorised session returns the wait to base: %u/%u/%u ms", g_slept[0], g_slept[1],
            g_slept[2]);

    script_session(3, 0, STRATUM_END_EOF, 0);
    pool_session_once(&g_ps, &g_pipe);
    pool_session_once(&g_ps, &g_pipe);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_slept_n == 3 && g_slept[0] == 1000 && g_slept[1] == 2000 && g_slept[2] == 4000,
            "no authorised session -> the wait doubles: %u/%u/%u ms", g_slept[0], g_slept[1],
            g_slept[2]);

    script_session(2, -1, STRATUM_END_EOF, 1);
    gf                = g_pool_st[0].getfail;
    g_pool_st[0].dead = 0;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.pools[0].alive == 0, "failed connect -> pool 0 dead (alive=%d)",
            g_ps.pools[0].alive);
    CHECK(g_pool_st[0].dead == 1 && g_pool_st[0].getfail == gf,
            "...shown Dead, without a Get Failure (dead=%d, %u -> %u)", g_pool_st[0].dead, gf,
            g_pool_st[0].getfail);
    CHECK(g_slept_n == 0, "...and does not wait out the backoff (%d waits)", g_slept_n);

    script_session(2, 0, STRATUM_END_EOF, 0);
    pool_mark_dead(&g_ps, 0);
    pool_mark_dead(&g_ps, 1);
    g_pool_st[0].dead = g_pool_st[1].dead = 1;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.pools[0].alive == 1 && g_ps.pools[1].alive == 1,
            "every pool dead -> the whole set is revived");
    CHECK(g_pool_st[0].dead == 1 && g_pool_st[1].dead == 1,
            "...but the API keeps showing them Dead until one delivers a job "
            "(dead %d %d)",
            g_pool_st[0].dead, g_pool_st[1].dead);
    CHECK(g_slept_n == 1, "...after one backoff wait, with no session run (%d)", g_slept_n);

    script_session(1, -1, STRATUM_END_EOF, 0);
    pool_session_once(&g_ps, &g_pipe);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_slept_n == 1 && g_pool_st[0].dead == 1,
            "the only pool unreachable -> Dead through the backoff (%d waits, dead=%d)", g_slept_n,
            g_pool_st[0].dead);
    g_connect_rc  = 0;
    g_established = 1;
    g_got_job     = 1;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_pool_st[0].dead == 0, "...and Alive once it delivers a job (dead=%d)",
            g_pool_st[0].dead);

    script_session(1, 0, STRATUM_END_EOF, 1);
    g_clear_n = g_clear_after_drop = 0;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_clear_n == 1 && g_clear_after_drop == 1,
            "in-flight shares are cleared only after the session is dropped "
            "(%d of %d clears)",
            g_clear_after_drop, g_clear_n);

    script_session(2, 0, STRATUM_END_EOF, 1);
    publish_pools(&g_ps);
    gf               = g_pool_st[0].getfail;
    uint32_t gf1     = g_pool_st[1].getfail;
    g_shutdown_n     = 0;
    g_disable_active = 1;
    uint32_t failcnt = g_pool_failcnt;
    pool_session_once(&g_ps, &g_pipe);
    g_disable_active = 0;
    CHECK(g_pool_enabled[0] == 0 && g_shutdown_n == 1,
            "disablepool on the active pool drops its session (enabled=%d, %d shutdown)",
            g_pool_enabled[0], g_shutdown_n);
    CHECK(g_pool_st[0].getfail == gf, "...without a Get Failure: the user ended it (%u -> %u)", gf,
            g_pool_st[0].getfail);
    CHECK(g_pool_failcnt == failcnt + 1, "...but counted as a pool switch in failcnt (%u -> %u)",
            failcnt, g_pool_failcnt);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_pool_st[1].getfail == gf1 + 1,
            "...and the next interrupted session, on pool 1, is one again (%u -> %u)", gf1,
            g_pool_st[1].getfail);

    g_pool_enabled[0] = 1;
    script_session(2, 0, STRATUM_END_EOF, 1);
    publish_pools(&g_ps);
    publish_active_pool(0);
    disable_active();
    CHECK(g_pool_enabled[0] == 0, "disablepool 0 between sessions disables it");
    g_pool_enabled[0] = 1;
    gf                = g_pool_st[0].getfail;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_pool_st[0].getfail == gf + 1,
            "...and leaves no mark on the next session of that pool (%u -> %u)", gf,
            g_pool_st[0].getfail);

    g_pool_enabled[0] = 1;
    script_session(2, 0, STRATUM_END_EOF, 1);
    publish_pools(&g_ps);
    publish_active_pool(-1);
    gf                   = g_pool_st[0].getfail;
    failcnt              = g_pool_failcnt;
    int runs             = g_run_n;
    g_disable_on_connect = 1;
    pool_session_once(&g_ps, &g_pipe);
    g_disable_on_connect = 0;
    CHECK(g_pool_enabled[0] == 0 && g_run_n == runs,
            "disablepool while pool 0 connects runs no session on it "
            "(enabled=%d, %d sessions)",
            g_pool_enabled[0], g_run_n - runs);
    CHECK(g_pool_st[0].getfail == gf && g_pool_failcnt == failcnt + 1,
            "...and counts a pool switch, not a Get Failure (getfail %u -> %u, failcnt %u -> %u)",
            gf, g_pool_st[0].getfail, failcnt, g_pool_failcnt);
    CHECK(g_slept_n == 0, "...and goes straight to the next pool (%d waits)", g_slept_n);
    g_pool_enabled[0] = 1;

    script_session(2, 0, STRATUM_END_EOF, 0);
    publish_pools(&g_ps);
    g_ps.backoff_ms  = 32000;
    failcnt          = g_pool_failcnt;
    g_disable_active = 1;
    pool_session_once(&g_ps, &g_pipe);
    g_disable_active = 0;
    CHECK(g_slept_n == 0 && g_ps.backoff_ms == 32000,
            "disablepool on the active pool skips the backoff wait and keeps "
            "its length (%d waits, %d ms)",
            g_slept_n, g_ps.backoff_ms);
    CHECK(g_pool_failcnt == failcnt + 1 && g_ps.pools[0].alive == 1,
            "...still counted in failcnt, pool 0 not dead (%u -> %u, alive=%d)", failcnt,
            g_pool_failcnt, g_ps.pools[0].alive);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.active == 1 && g_slept_n == 1 && g_slept[0] == 60000 && g_ps.backoff_ms == 60000,
            "...while a failed session on pool 1 still doubles the wait "
            "(picked %d, %d waits, %u ms)",
            g_ps.active, g_slept_n, g_slept[0]);
    g_pool_enabled[0] = 1;

    script_session(2, 0, STRATUM_END_EOF, 1);
    publish_pools(&g_ps);
    g_pool_prio[0] = 1;
    g_pool_prio[1] = 0;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.active == 1,
            "the session loop picks the pool with the lowest running prio (picked %d)",
            g_ps.active);
    publish_pools(&g_ps);

    script_session(1, 0, STRATUM_END_EOF, 1);
    pthread_mutex_lock(&g_stats_lock);
    g_ping.inflight = 1;
    g_ping.sent_ms  = now_ms();
    pthread_mutex_unlock(&g_stats_lock);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ping.inflight == 0, "a dropped session times no submit across the reconnect (%d)",
            g_ping.inflight);

    char         buf[512];
    mc_api_stats st;
    memset(&st, 0, sizeof st);
    script_session(3, 0, STRATUM_END_EOF, 1);
    publish_pools(&g_ps);
    publish_active_pool(0);
    g_cfgwr_rc = -1;

    disablepool_handler("disablepool|1", &st, 0, buf, sizeof buf, NULL);
    CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=53,") &&
                    strstr(buf, "Msg=config write error,") && g_pool_enabled[1] == 0,
            "disablepool with the config write failing disables pool 1 and replies "
            "E,53 (%d: %s)",
            g_pool_enabled[1], buf);

    enablepool_handler("enablepool|1", &st, 0, buf, sizeof buf, NULL);
    CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=53,") &&
                    strstr(buf, "Msg=config write error,") && g_pool_enabled[1] == 1,
            "enablepool with the config write failing enables pool 1 and replies "
            "E,53 (%d: %s)",
            g_pool_enabled[1], buf);

    g_pool_enabled[2] = 0;
    pthread_mutex_lock(&g_pools_lock);
    g_pool_revive = 0;
    pthread_mutex_unlock(&g_pools_lock);
    switchpool_handler("switchpool|2", &st, 0, buf, sizeof buf, NULL);
    CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=53,") &&
                    strstr(buf, "Msg=config write error,") && g_pool_enabled[2] == 1 &&
                    g_pool_prio[2] == 0 && g_pool_prio[0] == 1 && g_pool_prio[1] == 2 &&
                    (g_pool_revive & (1u << 2)),
            "switchpool with the config write failing enables, revives and ranks "
            "pool 2 first and replies E,53 (en %d prio %d %d %d revive %#x: %s)",
            g_pool_enabled[2], g_pool_prio[0], g_pool_prio[1], g_pool_prio[2], g_pool_revive, buf);

    poolpriority_handler("poolpriority|1,0", &st, 0, buf, sizeof buf, NULL);
    CHECK(strstr(buf, "STATUS=E,") && strstr(buf, "Code=53,") &&
                    strstr(buf, "Msg=config write error,") && g_pool_prio[1] == 0 &&
                    g_pool_prio[0] == 1 && g_pool_prio[2] == 2,
            "poolpriority with the config write failing reorders the pools and "
            "replies E,53 (prio %d %d %d: %s)",
            g_pool_prio[0], g_pool_prio[1], g_pool_prio[2], buf);
    g_cfgwr_rc = 0;

    script_session(1, -1, STRATUM_END_EOF, 0);
    publish_pools(&g_ps);
    g_pool_st[0].dead = 0;
    pool_session_once(&g_ps, &g_pipe);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_pool_st[0].dead == 1 && g_ps.pools[0].alive == 1,
            "the only pool failing to connect stays Dead while it is retried "
            "(dead=%d, alive=%d)",
            g_pool_st[0].dead, g_ps.pools[0].alive);
    script_session(1, 0, STRATUM_END_EOF, 1);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_pool_st[0].dead == 0, "...until a session on it delivers a job (dead=%d)",
            g_pool_st[0].dead);

    script_session(2, 0, STRATUM_END_TIMEOUT, 1);
    publish_pools(&g_ps);
    g_got_job         = 0;
    g_pool_st[0].dead = 0;
    g_ps.backoff_ms   = 4000;
    gf                = g_pool_st[0].getfail;
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.pools[0].alive == 0 && g_pool_st[0].dead == 1 && g_pool_st[0].getfail == gf,
            "a pool that authorises but never delivers a usable job is Dead, "
            "without a Get Failure (alive=%d, dead=%d, getfail %u -> %u)",
            g_ps.pools[0].alive, g_pool_st[0].dead, gf, g_pool_st[0].getfail);
    CHECK(g_slept_n == 1 && g_slept[0] == 8000,
            "...and does not reset the backoff (%d waits, %u ms)", g_slept_n, g_slept[0]);
    pool_session_once(&g_ps, &g_pipe);
    CHECK(g_ps.active == 1, "...so the next session tries pool 1 (picked %d)", g_ps.active);

    {
        sv1_client       c;
        sv1_event        ev  = { .type = SV1_EV_PING };
        core_session_ctx ctx = { .pipe = &g_pipe, .user = "u0" };

        sv1_init(&c);
        c.state      = SV1_AUTHORIZED;
        ctx.start_ms = now_ms();
        CHECK(on_sv1_event(&ctx, &c, &ev) == 0,
                "an authorised session without a job yet keeps running");
        ctx.start_ms = now_ms() - STRATUM_RECV_TIMEOUT_S * 1000u;
        CHECK(on_sv1_event(&ctx, &c, &ev) == 1,
                "...and is ended once it has had no usable job for %d s", STRATUM_RECV_TIMEOUT_S);
        ctx.got_job = 1;
        CHECK(on_sv1_event(&ctx, &c, &ev) == 0,
                "a session that delivered a job is never ended for its age");
        sv1_free(&c);
    }

    {
        stratum_transport live;

        script_session(3, 0, STRATUM_END_EOF, 1);
        publish_pools(&g_ps);
        for (int i = 0; i < POOL_VALID_NUM; i++) {
            g_pool_enabled[i] = 1;
            g_pool_prio[i]    = i;
            g_pool_st[i].dead = 0;
        }
        pthread_mutex_lock(&g_pools_lock);
        g_pool_revive = 0;
        pthread_mutex_unlock(&g_pools_lock);
        g_pool_st[0].dead = 1;
        g_pool_st[2].dead = 1;
        publish_active_pool(1);
        memset(&live, 0, sizeof live);
        g_stratum.t         = &live;
        g_stratum.user_drop = 0;
        g_shutdown_n        = 0;
        g_probe_n           = 0;
        g_probe_rx          = 0;
        g_probe_up          = 1;

        uint64_t t0 = 1000000;
        pool_probe_tick(t0);
        CHECK(g_probe_n == 1,
                "the probe tries only the Dead pool ranked above the active one "
                "(%d probes)",
                g_probe_n);
        CHECK(g_probe_rx == 0, "...without counting its lines on the active pool (%u)", g_probe_rx);
        pool_probe_tick(t0 + POOL_FALLBACK_MS - 1);
        CHECK(g_shutdown_n == 0 && !(g_pool_revive & 1u) && g_pool_st[0].dead == 1,
                "...and does not switch back before it has answered for %d s "
                "(%d shutdown, revive %#x)",
                POOL_FALLBACK_MS / 1000, g_shutdown_n, g_pool_revive);
        pool_probe_tick(t0 + POOL_FALLBACK_MS);
        CHECK(g_shutdown_n == 1 && g_stratum.user_drop == 1 && (g_pool_revive & 1u),
                "...then revives it and drops the session on pool 1 "
                "(%d shutdown, user_drop %d, revive %#x)",
                g_shutdown_n, g_stratum.user_drop, g_pool_revive);

        pthread_mutex_lock(&g_pools_lock);
        g_pool_revive = 0;
        pthread_mutex_unlock(&g_pools_lock);
        g_stratum.user_drop = 0;
        g_shutdown_n        = 0;
        pool_probe_tick(t0);
        g_probe_up = 0;
        pool_probe_tick(t0 + POOL_PROBE_MS);
        g_probe_up = 1;
        pool_probe_tick(t0 + 2 * POOL_PROBE_MS);
        pool_probe_tick(t0 + POOL_FALLBACK_MS + POOL_PROBE_MS);
        CHECK(g_shutdown_n == 0 && !(g_pool_revive & 1u),
                "a failed probe restarts the %d s count (%d shutdown)", POOL_FALLBACK_MS / 1000,
                g_shutdown_n);

        publish_active_pool(0);
        g_probe_n = 0;
        pool_probe_tick(t0);
        publish_active_pool(-1);
        pool_probe_tick(t0);
        CHECK(g_probe_n == 0, "nothing is probed on the top pool or between sessions (%d probes)",
                g_probe_n);
        g_stratum.t = NULL;
    }

    {
        sv1_client c;
        char      *line;

        sv1_init(&c);
        g_log_len = 0;
        g_log[0]  = '\0';
        line      = sv1_build_authorize(&c, "u0", "s3cretPW");
        pool_trace(1, line);
        free(line);
        CHECK(strstr(g_log, "mining.authorize") && !strstr(g_log, "s3cretPW"),
                "a traced authorize request logs its method, not the pool password (%s)", g_log);
        g_log_len = 0;
        g_log[0]  = '\0';
        line      = sv1_build_subscribe(&c, "agent/1.0");
        pool_trace(1, line);
        CHECK(strstr(g_log, line) != NULL, "...while other requests are traced in full (%s)",
                g_log);
        free(line);
        sv1_free(&c);
    }

    if (fails) {
        fprintf(stderr, "test_core_reconnect: %d FAIL\n", fails);
        return 1;
    }
    printf("test_core_reconnect: all pass\n");
    return 0;
}
