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

#include "osif.h"
#include <signal.h>
#include <stdio.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

static void *worker(void *arg)
{
    int *p = (int *)arg;
    *p += 41;
    return (void *)(long)(*p);
}

static void *altstack_probe(void *arg)
{
    stack_t *out = (stack_t *)arg;

    sigaltstack(NULL, out);
    return NULL;
}

int main(void)
{
    int            shared = 1;
    osif_thread_t *t      = NULL;
    CHECK(osif_thread_create(&t, "osif-test", worker, &shared, 0) == OSIF_OK && t, "thread create");

    void *ret = NULL;
    CHECK(osif_thread_join(t, &ret) == OSIF_OK, "thread join");
    CHECK(shared == 42, "worker ran and mutated shared state (42)");
    CHECK((long)ret == 42, "join returned the worker's return value");

    CHECK(osif_thread_create(&t, "x", NULL, NULL, 0) == OSIF_ERR, "NULL fn rejected");

    stack_t ss = { .ss_flags = SS_DISABLE };
    CHECK(osif_thread_create(&t, "osif-alt", altstack_probe, &ss, 64 * 1024) == OSIF_OK && t,
            "thread create with 64 KiB stack");
    CHECK(osif_thread_join(t, NULL) == OSIF_OK, "altstack thread join");
    CHECK(!(ss.ss_flags & SS_DISABLE) && ss.ss_sp && ss.ss_size >= 64 * 1024,
            "new thread runs with an alternate signal stack installed");

    if (fails) {
        fprintf(stderr, "test_osif_thread: %d FAIL\n", fails);
        return 1;
    }
    printf("test_osif_thread: all pass\n");
    return 0;
}
