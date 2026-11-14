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

#define _GNU_SOURCE
#include "osif.h"
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <limits.h>

#define OSIF_THREAD_ALTSTACK (64 * 1024) /* minimum per-thread sigaltstack size, bytes */

struct osif_thread {
    pthread_t t;
};

struct osif_thread_start {
    osif_thread_fn fn;
    void          *arg;
};

static void altstack_release(void *mem)
{
    stack_t ss = { .ss_flags = SS_DISABLE };

    sigaltstack(&ss, NULL);
    free(mem);
}

static void *osif_thread_trampoline(void *p)
{
    struct osif_thread_start st = *(struct osif_thread_start *)p;
    size_t                   sz = OSIF_THREAD_ALTSTACK;
    void                    *mem, *ret;

    free(p);
    if ((size_t)SIGSTKSZ > sz)
        sz = SIGSTKSZ;
    mem = malloc(sz);
    if (mem) {
        stack_t ss = { .ss_sp = mem, .ss_size = sz };

        if (sigaltstack(&ss, NULL) != 0) {
            free(mem);
            mem = NULL;
        }
    }

    ret = st.fn(st.arg);
    if (mem)
        altstack_release(mem);
    return ret;
}

osif_status_t osif_thread_create(osif_thread_t **t, const char *name, osif_thread_fn fn, void *arg,
        size_t stack_bytes)
{
    if (!fn)
        return OSIF_ERR;
    osif_thread_t *o = calloc(1, sizeof *o);
    if (!o)
        return OSIF_ERR;
    struct osif_thread_start *st = malloc(sizeof *st);
    if (!st) {
        free(o);
        return OSIF_ERR;
    }
    st->fn  = fn;
    st->arg = arg;

    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack_bytes) {
        if (stack_bytes < (size_t)PTHREAD_STACK_MIN)
            stack_bytes = PTHREAD_STACK_MIN;
        pthread_attr_setstacksize(&a, stack_bytes);
    }

    int rc = pthread_create(&o->t, &a, osif_thread_trampoline, st);
    pthread_attr_destroy(&a);
    if (rc != 0) {
        free(st);
        free(o);
        return OSIF_ERR;
    }

    if (name)
        pthread_setname_np(o->t, name);
    *t = o;
    return OSIF_OK;
}

osif_status_t osif_thread_join(osif_thread_t *t, void **ret)
{
    if (!t)
        return OSIF_ERR;
    int rc = pthread_join(t->t, ret);
    free(t);
    return rc == 0 ? OSIF_OK : OSIF_ERR;
}
