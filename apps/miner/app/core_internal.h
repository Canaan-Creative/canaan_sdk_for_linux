/* SPDX-License-Identifier: MIT */
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

#ifndef CORE_INTERNAL_H
#define CORE_INTERNAL_H

#include "mc_api_types.h"
#include "miner_ipc.h"
#include "osif.h"
#include "pipeline.h"
#include "pool.h"
#include "process_version.h"
#include "stratum_net.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#define MHS_WIN_MAX_S      900 /* hashrate window length, s */
#define MHS_WIN_SLOTS      (MHS_WIN_MAX_S + 1)
#define SUBMIT_LOG         8       /* recent submits kept, count */
#define CORE_MAIN_STALL_MS 360000u /* main loop stall limit, ms */
/* Limit for dispatching one request on :4028 or on the trusted socket, ms */
#define CORE_API_STALL_MS 30000u
#define JOB_RING          4
#define POOL_USER_AGENT   "AvalonMiner/1.0"
#define POOL_PROBE_MS     30000 /* period of the probe of higher ranked Dead pools, ms */
/* A Dead pool ranked above the active one is switched back to after it has
 * answered every probe for this long, ms */
#define POOL_FALLBACK_MS 120000

typedef struct {
    int      id;                  /* stratum request id */
    uint32_t seq, n2, nonce, age; /* job seq, extranonce2, nonce, jobs newer than this one */
    uint8_t  roll;                /* ntime roll, s */
    char     jdiff[32];           /* job difficulty, text */
    uint32_t h32;                 /* top 32 bits of the share hash */
    double   sdiff;               /* share difficulty */
    uint64_t at_ms;               /* submit time, ms */
} submit_rec;

struct core_stratum {
    stratum_transport *t; /* transport, NULL without a session */
    mc_pipeline       *pipe;
    int                have_job;
    int                user_drop; /* session closed by a pool command */
    pthread_mutex_t    lock;      /* guards the members above, g_ring, g_job_seq and g_dead_seq */
    pthread_mutex_t    send;      /* submit send lock */
};

struct core_pool_pub {
    char url[POOL_URL_MAX], user[POOL_USER_MAX], pass[POOL_PASS_MAX];
};

/* per-pool statistics, guarded by g_stats_lock */
struct core_pool_stat {
    uint32_t acc;    /* accepted shares, count */
    uint32_t rej;    /* rejected shares, count */
    double   dacc;   /* accepted difficulty */
    double   drej;   /* rejected difficulty */
    double   wdiff;  /* work difficulty */
    double   lsdiff; /* last share difficulty */
    uint32_t ltime;  /* last share time, s */
    double   best;
    double   diff1; /* valid nonce difficulty */
    uint32_t bad;   /* jobs with under two merkle branches, count */
    uint32_t getworks;
    uint32_t bver; /* block version */
    uint32_t works;
    uint32_t getfail; /* established sessions ended, count */
    uint32_t rfail;   /* submit send failures, count */
    uint32_t height;
    uint32_t txcnt; /* lines sent, count */
    uint32_t txbytes;
    uint32_t rxcnt; /* lines received, count */
    uint32_t rxbytes;
    uint32_t stale;
    double   dstale;
    int      dead;
};

typedef struct {
    uint32_t seq;
    int      pool;
    int      xnonce2_size; /* extranonce2 size, bytes */
    uint8_t  hdr[80];      /* block header template */
    int      hdr_ok;
    char     version[16], prevhash[72], nbits[16], ntime[16]; /* notify fields, hex */
    /* extranonce1 and version mask hex, job id, difficulty */
    char xnonce1[68], mask[16], job_id[72], diff[32];
    char coinb1[MC_COINB_HEX_LEN], coinb2[MC_COINB_HEX_LEN]; /* coinbase halves, hex */
    char merkle[MC_MERKLE_HEX_LEN];                          /* merkle branches, hex */
} job_tmpl;

/* nonce and HW error counters, under g_stats_lock */
struct core_nonce_stat {
    uint32_t hw_errors;
    uint32_t hw_board[HBOARD_COUNT];
    uint32_t rx;            /* nonces received, count */
    uint32_t low_diff;      /* nonces below the share target, count */
    uint32_t hit_direct;    /* nonces matched as sent, count */
    uint32_t hit_repaired;  /* nonces matched after nonce2 reconstruction, count */
    uint32_t hw_verify;     /* HW errors, verify failed, count */
    uint32_t hw_nosession;  /* HW errors, no pool session, count */
    uint32_t hw_nomatch;    /* HW errors, job not in the ring, count */
    uint32_t hw_impossible; /* HW errors, job newer than the newest, count */
    uint32_t retired;       /* nonces for retired jobs, count */
    uint32_t aging;         /* nonces for the static aging work, count */
    uint32_t disc_base;     /* asic nonce_discarded at the last stats reset, count */
};

/* pool submit round trip timing, under g_stats_lock */
struct core_ping {
    uint32_t ms;       /* pool submit round trip, ms */
    uint64_t sent_ms;  /* time of the unanswered submit, ms */
    int      inflight; /* a submit awaits its pool reply */
};

/* hashrate window, under g_stats_lock */
struct core_hashwin {
    double slot[MHS_WIN_SLOTS]; /* difficulty per second, newest at .at */
    time_t at;                  /* time of the newest window slot, s */
    int    live;
};

/* current block and network state, under g_stats_lock */
struct core_block {
    char     hash[65]; /* current block hash, hex */
    double   net_diff;
    double   time;       /* last block change time, s */
    uint32_t net_blocks; /* block changes, count */
};

extern osif_ipc_t            *g_ipc_asic_core;
extern time_t                 g_start;
extern uint32_t               g_local_work;
extern pthread_mutex_t        g_stats_lock;
extern double                 g_diff_acc;
extern double                 g_diff_rej;
extern double                 g_diff1;
extern struct core_ping       g_ping;
extern struct core_nonce_stat g_nst;
extern struct core_hashwin    g_hashwin;
extern submit_rec             g_sub[SUBMIT_LOG];
extern asic_chip_telem        g_chip;
extern time_t                 g_chip_at;
extern uint64_t               g_mw[HBOARD_COUNT][MINER_MAX_CHIPS];
extern mm_periph              g_periph;
extern uint32_t               g_mm_state;
extern osif_ipc_t            *g_ipc_core_mm;
extern miner_version_state    g_mm_version;
extern _Atomic uint64_t       g_main_beat_ms;
extern struct core_stratum    g_stratum;
extern uint8_t                g_bootby_reason;
extern uint32_t               g_bootby_sub;
extern pthread_mutex_t        g_pools_lock;
extern struct core_pool_pub   g_pool_pub[POOL_VALID_NUM];
extern int                    g_pool_pub_n;
extern int                    g_pool_active;
extern int                    g_pool_enabled[POOL_VALID_NUM];
extern int                    g_pool_prio[POOL_VALID_NUM];
extern struct core_pool_stat  g_pool_st[POOL_VALID_NUM];
extern uint32_t               g_pool_failcnt;
extern uint32_t               g_netfail_time[8];
extern struct core_block      g_block;
extern double                 g_board_diff1[HBOARD_COUNT];
extern time_t                 g_dev_start;
extern uint32_t               g_last_valid_work;
extern job_tmpl               g_ring[JOB_RING];
extern uint32_t               g_job_seq;
extern uint32_t               g_dead_seq;
extern char                   g_user[POOL_USER_MAX];

uint64_t now_ms(void);
double   now_mono_s(void);
time_t   now_s(void);

void win_add_locked(time_t now, double dd);
void core_stats_push(void);
void api_snapshot(mc_api_stats *s, void *ctx);

void  nonce_cb(uint32_t type, const void *data, int len);
void  send_asic_template(const job_tmpl *t);
void *pump_thread(void *arg);

extern _Thread_local int g_trace_quiet;

void     pool_trace(int tx, const char *line);
void     pool_session_once(pool_set *ps, mc_pipeline *pipe);
uint32_t job_gap_s(void);

void  publish_pools(const pool_set *ps);
void  publish_active_pool(int idx);
void  sync_pool_policy(pool_set *ps, int *prio);
int   pool_outranked(int idx);
void  pool_probe_tick(uint64_t now);
void *pool_probe_thread(void *arg);
int   core_pool_cmds_register(const char *conf);

#endif /* CORE_INTERNAL_H */
