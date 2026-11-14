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
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "  FAIL: %s\n", m); \
            fails++; \
        } else \
            printf("  ok: %s\n", m); \
    } while (0)

static osif_ipc_t  *g_srvA;               /* server of channel A, acks every message it receives */
static volatile int gotA, gotB;           /* 1 once server A / server B received a message */
static uint32_t     typeA, typeB;         /* type of the last message on channel A / channel B */
static char         dataA[64], dataB[64]; /* payload of the last message on channel A / channel B */
static volatile int child_ack; /* 1 once the client of channel A received the 0xACC ack */

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void srvA_cb(uint32_t t, const void *d, int l)
{
    typeA = t;
    if (l > 0 && l < (int)sizeof dataA)
        memcpy(dataA, d, (size_t)l);
    gotA = 1;
    osif_ipc_send(g_srvA, 0xACCu, "ack", 4);
}
static void srvB_cb(uint32_t t, const void *d, int l)
{
    typeB = t;
    if (l > 0 && l < (int)sizeof dataB)
        memcpy(dataB, d, (size_t)l);
    gotB = 1;
}

static void cliA_cb(uint32_t t, const void *d, int l)
{
    (void)d;
    (void)l;
    if (t == 0xACCu)
        child_ack = 1;
}
static void cliB_cb(uint32_t t, const void *d, int l)
{
    (void)t;
    (void)d;
    (void)l;
}

int main(void)
{
    char pathA[64], pathB[64];
    snprintf(pathA, sizeof pathA, "/tmp/osif_ipc_multi_a.%d.sock", (int)getpid());
    snprintf(pathB, sizeof pathB, "/tmp/osif_ipc_multi_b.%d.sock", (int)getpid());
    unlink(pathA);
    unlink(pathB);

    pid_t pid = fork();
    if (pid == 0) {
        osif_ipc_t *cA = NULL, *cB = NULL;
        msleep(150);
        if (osif_ipc_init(&cA, cliA_cb, pathA, 0) != OSIF_OK)
            _exit(2);
        if (osif_ipc_init(&cB, cliB_cb, pathB, 0) != OSIF_OK)
            _exit(2);
        for (int i = 0; i < 100 && !(osif_ipc_connected(cA) && osif_ipc_connected(cB)); i++)
            msleep(10);
        if (!(osif_ipc_connected(cA) && osif_ipc_connected(cB))) {
            osif_ipc_deinit(cA);
            osif_ipc_deinit(cB);
            _exit(3);
        }
        osif_ipc_send(cA, 0xAAu, "aaa", 4);
        osif_ipc_send(cB, 0xBBu, "bbb", 4);
        for (int i = 0; i < 100 && !child_ack; i++)
            msleep(10);
        int rc = child_ack ? 0 : 4;
        osif_ipc_deinit(cA);
        osif_ipc_deinit(cB);
        _exit(rc);
    }

    int         fails = 0;
    osif_ipc_t *sB    = NULL;
    if (osif_ipc_init(&g_srvA, srvA_cb, pathA, 1) != OSIF_OK) {
        fprintf(stderr, "srvA init failed\n");
        return 2;
    }
    if (osif_ipc_init(&sB, srvB_cb, pathB, 1) != OSIF_OK) {
        fprintf(stderr, "srvB init failed\n");
        return 2;
    }
    struct stat stA, stB;
    int         mode_ok = stat(pathA, &stA) == 0 && stat(pathB, &stB) == 0 &&
            (stA.st_mode & 07777) == 0700 && (stB.st_mode & 07777) == 0700;
    for (int i = 0; i < 300 && !(gotA && gotB); i++)
        msleep(10);
    int st = 0;
    waitpid(pid, &st, 0);
    osif_ipc_deinit(g_srvA);
    osif_ipc_deinit(sB);

    CHECK(gotA && typeA == 0xAAu && strcmp(dataA, "aaa") == 0,
            "channel A delivered its own message");
    CHECK(gotB && typeB == 0xBBu && strcmp(dataB, "bbb") == 0,
            "channel B delivered its own message");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
            "client got reverse ACK on channel A (server->client send)");
    CHECK(mode_ok, "bound socket is 0700, not umask-derived");

    if (fails) {
        fprintf(stderr, "test_osif_ipc_multi: %d FAIL\n", fails);
        return 1;
    }
    printf("test_osif_ipc_multi: all pass\n");
    return 0;
}
