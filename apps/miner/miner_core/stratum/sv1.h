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

/*
 * Stratum V1 client. Builders turn intent into outbound JSON lines and
 * sv1_feed_line() turns inbound lines into events; no network I/O.
 */
#ifndef SV1_H
#define SV1_H

#include <stdint.h>
#include <stddef.h>

#define SV1_MAX_MERKLE  32 /* merkle branches per job, count */
#define SV1_XNONCE1_MAX 33 /* hex chars + NUL */
#define SV1_ID_MAX      64 /* job id buffer, bytes */
#define SV1_ERR_MAX     96 /* pool error or message text, bytes */

/*
 * Rollable version bits (BIP310: 13..28). Requested in mining.configure and
 * also a hard clamp on what the pool grants: the base version is derived as
 * version & ~mask, so a wider mask would corrupt it.
 */
#define SV1_VERSION_MASK_MAX 0x1fffe000u

/* Advances as the matching responses arrive. */
typedef enum {
    SV1_INIT = 0,   /* nothing negotiated */
    SV1_CONFIGURED, /* version rolling negotiated */
    SV1_SUBSCRIBED, /* xnonce1 / xnonce2_size known */
    SV1_AUTHORIZED,
    SV1_MINING, /* authorized and at least one job received */
} sv1_state;

/* mining.notify job; strings stay valid until the next notify or sv1_free(). */
typedef struct {
    char   job_id[SV1_ID_MAX];
    char  *prevhash;               /* previous block hash (hex) */
    char  *coinb1;                 /* coinbase part 1 (hex) */
    char  *coinb2;                 /* coinbase part 2 (hex) */
    char  *merkle[SV1_MAX_MERKLE]; /* merkle branches (hex) */
    size_t n_merkle;
    char   version[16]; /* block version (hex) */
    char   nbits[16];   /* difficulty bits (hex) */
    char   ntime[16];   /* timestamp (hex) */
    int    clean_jobs;  /* 1 if earlier jobs are void */
} sv1_job;

typedef enum {
    SV1_EV_NONE = 0,
    SV1_EV_CONFIGURED,       /* mining.configure answered */
    SV1_EV_SUBSCRIBED,       /* mining.subscribe answered */
    SV1_EV_AUTHORIZED,       /* mining.authorize accepted */
    SV1_EV_SET_DIFFICULTY,   /* mining.set_difficulty */
    SV1_EV_SET_EXTRANONCE,   /* mining.set_extranonce */
    SV1_EV_SET_VERSION_MASK, /* mining.set_version_mask */
    SV1_EV_NOTIFY,           /* mining.notify */
    /* The pool drops a client that ignores these: reply with
     * sv1_build_pong / sv1_build_version for ev.req_id. */
    SV1_EV_PING,             /* mining.ping */
    SV1_EV_GET_VERSION,      /* client.get_version */
    SV1_EV_SHOW_MESSAGE,     /* text in err_msg */
    SV1_EV_RECONNECT,        /* reported, not acted on */
    SV1_EV_SUBMIT_RESULT,    /* answer to an issued submit */
    SV1_EV_SUBMIT_UNTRACKED, /* answer matching no issued submit */
    /* malformed line or params, or authorize refused */
    SV1_EV_ERROR,
} sv1_event_type;

typedef struct {
    sv1_event_type type;
    int            submit_accepted; /* SUBMIT_RESULT */
    int            submit_id;       /* 0 if absent; also UNTRACKED */
    int            submit_lost;     /* matched by id only */
    int            req_id;          /* id to answer for PING / GET_VERSION */
    /* Reject reason; err_msg lives until the next sv1_feed_line. */
    int         err_code; /* 0 if none */
    const char *err_msg;  /* "" if none */
} sv1_event;

typedef struct {
    sv1_state state;
    int       next_id; /* last issued JSON-RPC id + 1 */
    int       id_configure;
    int       id_subscribe;
    int       id_authorize;
    int       id_xnsub;                 /* mining.extranonce.subscribe */
    char      xnonce1[SV1_XNONCE1_MAX]; /* extranonce1 (hex) */
    int       xnonce2_size;             /* bytes */
    int       extranonce_subscribe;     /* #xnsub in the pool URL */
    uint32_t  version_mask;             /* <= SV1_VERSION_MASK_MAX; 0 = none */
    int       version_rolling;
    /*
     * Difficulty in force. set_difficulty only stages next_diff; it takes
     * effect from the next notify. Can be fractional.
     */
    double   difficulty;
    double   next_diff;  /* staged for the next notify, 0 = none */
    double   diff_after; /* staged behind next_diff */
    sv1_job  job;
    int      has_job;
    char     prev_hash[72];        /* changed = clean notify */
    char     err_msg[SV1_ERR_MAX]; /* backs sv1_event.err_msg */
    uint32_t accepted;
    uint32_t rejected;
    uint32_t untracked; /* answers matching no submit, count */
    uint32_t stale;     /* in flight when the session died */
} sv1_client;

uint32_t sv1_clear_inflight(sv1_client *c, double *diff);
void     sv1_inflight_set_diff(int id, double diff);
void     sv1_init(sv1_client *c);
void     sv1_free(sv1_client *c);
char    *sv1_build_configure(sv1_client *c);
char    *sv1_build_subscribe(sv1_client *c, const char *user_agent);
char    *sv1_build_authorize(sv1_client *c, const char *user, const char *pass);
char    *sv1_build_extranonce_subscribe(sv1_client *c);
char    *sv1_build_pong(int req_id);
char    *sv1_build_version(int req_id, const char *user_agent);
char    *sv1_build_submit(sv1_client *c, const char *user, const char *job_id, const char *xnonce2,
           const char *ntime, const char *nonce, const char *version_bits);
int      sv1_feed_line(sv1_client *c, const char *line, sv1_event *ev);
uint32_t sv1_inflight_count(void);
uint32_t sv1_stale_shares(void);

#endif /* SV1_H */
