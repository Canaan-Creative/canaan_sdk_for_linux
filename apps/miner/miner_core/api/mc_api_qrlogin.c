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
#include "mc_api_qrlogin.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

#define QR_SESSIONS 16  /* QR login sessions, max count */
#define QR_TTL_S    300 /* QR login session lifetime, s */
static struct {
    char   token[33]; /* session token, hex */
    time_t expires;   /* expiry time, monotonic s */
    int    approved;
} sessions[QR_SESSIONS];
static pthread_mutex_t   g_qr_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int g_peer_trusted; /* API request arrived on the trusted channel */

static time_t now_s(void)
{
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? ts.tv_sec : 0;
}

static int valid_token(const char *s)
{
    return s && strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32;
}

int mc_api_qr_login_begin(const char *token)
{
    time_t now = now_s();
    if (!now || !valid_token(token))
        return 0;
    pthread_mutex_lock(&g_qr_lock);
    int slot = -1, oldest = -1, dup = 0;
    for (int i = 0; i < QR_SESSIONS && !dup; i++) {
        if (sessions[i].expires <= now) {
            if (slot < 0)
                slot = i;
            continue;
        }
        if (!strncmp(sessions[i].token, token, 8))
            dup = 1;
        else if (!sessions[i].approved &&
                (oldest < 0 || sessions[i].expires < sessions[oldest].expires))
            oldest = i;
    }
    if (slot < 0)
        slot = oldest;
    if (dup || slot < 0) {
        pthread_mutex_unlock(&g_qr_lock);
        return 0;
    }
    strcpy(sessions[slot].token, token);
    sessions[slot].expires  = now + QR_TTL_S;
    sessions[slot].approved = 0;
    pthread_mutex_unlock(&g_qr_lock);
    return 1;
}

static int pending_slot(const char *nonce)
{
    time_t now = now_s();
    if (!now || !nonce || strlen(nonce) != 8)
        return -1;
    for (int i = 0; i < QR_SESSIONS; i++)
        if (sessions[i].expires > now && !strncmp(sessions[i].token, nonce, 8))
            return i;
    return -1;
}

int mc_api_qr_login_pending(const char *nonce)
{
    pthread_mutex_lock(&g_qr_lock);
    int i = pending_slot(nonce);
    pthread_mutex_unlock(&g_qr_lock);
    return i >= 0;
}

int mc_api_qr_login_approve(const char *nonce)
{
    pthread_mutex_lock(&g_qr_lock);
    int i = pending_slot(nonce);
    if (i >= 0)
        sessions[i].approved = 1;
    pthread_mutex_unlock(&g_qr_lock);
    return i >= 0;
}

int mc_api_qr_login_take(const char *token)
{
    time_t now   = now_s();
    int    taken = 0;
    if (!now || !valid_token(token))
        return 0;
    pthread_mutex_lock(&g_qr_lock);
    for (int i = 0; i < QR_SESSIONS && !taken; i++) {
        if (sessions[i].expires > now && !strcmp(sessions[i].token, token) &&
                sessions[i].approved) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            taken = 1;
        }
    }
    pthread_mutex_unlock(&g_qr_lock);
    return taken;
}

void mc_api_set_peer_trusted(int trusted)
{
    g_peer_trusted = trusted ? 1 : 0;
}
int mc_api_peer_trusted(void)
{
    return g_peer_trusted;
}
