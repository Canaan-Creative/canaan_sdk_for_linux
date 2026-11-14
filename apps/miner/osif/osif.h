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
 * The only OS interface lay3 (miner_core, miner_mgmt) may use. Bound at
 * build time to linux/.
 */
#ifndef OSIF_H
#define OSIF_H

#include <stdint.h>
#include <stddef.h>

#define OSIF_OK  0
#define OSIF_ERR (-1)

/* Largest payload; descriptor-sized senders _Static_assert against it. */
#define OSIF_IPC_MAX 65536

typedef int                osif_status_t;
typedef struct osif_thread osif_thread_t;
typedef void *(*osif_thread_fn)(void *arg);
typedef struct osif_lock osif_lock_t;
typedef struct osif_ipc  osif_ipc_t;
typedef void (*osif_ipc_recv_cb)(uint32_t type, const void *data, int len);

/*
 * log: lines are "[YYYY-MM-DD HH:MM:SS.mmm] [LVL] process: text". Levels above
 * $MINER_LOG_LEVEL (ERR|WARN|INFO|DBG, default INFO) are dropped; the enum
 * order is the threshold order, do not reorder.
 */
typedef enum {
    OSIF_LOG_OP,   /* operator event, 0; tag OP */
    OSIF_LOG_ERR,  /* error, 1; tag ERR */
    OSIF_LOG_WARN, /* warning, 2; tag WARN */
    OSIF_LOG_INFO, /* informational, 3; tag INFO */
    OSIF_LOG_DBG   /* debug, 4; tag DBG */
} osif_loglvl_t;

osif_status_t osif_thread_create(osif_thread_t **t, const char *name, osif_thread_fn fn, void *arg,
        size_t stack_bytes);
osif_status_t osif_thread_join(osif_thread_t *t, void **ret);
osif_status_t osif_lock_create(osif_lock_t **l);
void          osif_lock_acquire(osif_lock_t *l);
void          osif_lock_release(osif_lock_t *l);
uint64_t      osif_now_ms(void);
void          osif_sleep_ms(uint32_t ms);
osif_status_t osif_ipc_init(osif_ipc_t **h, osif_ipc_recv_cb cb, const char *path, int is_server);
int           osif_ipc_connected(osif_ipc_t *h);
unsigned      osif_ipc_conn_seq(osif_ipc_t *h);
osif_status_t osif_ipc_send(osif_ipc_t *h, uint32_t type, const void *data, int len);
void          osif_ipc_deinit(osif_ipc_t *h);
void osif_log(osif_loglvl_t lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  osif_log_op_sink_open(void);

#endif /* OSIF_H */
