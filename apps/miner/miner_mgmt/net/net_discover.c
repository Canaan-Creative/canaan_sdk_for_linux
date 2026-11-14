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

#include "net_discover.h"
#include "net_state.h"
#include "net_log.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define NET_DISCOVER_GAP_MS 1000

/* Owned copy: the detached thread outlives the caller's string. */
static char g_disc_dev[64];

size_t net_discover_reply(const char *req, const char *dev, const char *ip, char *out, size_t cap)
{
    if (!req || !out || cap == 0)
        return 0;
    /* Prefix match: a probe with trailing bytes still gets a reply. */
    if (strncmp(req, NET_DISCOVER_CMD, strlen(NET_DISCOVER_CMD)) != 0)
        return 0;

    int n = snprintf(out, cap, "{\"device\":\"%s\",\"ip\":\"%s\"}", dev ? dev : "", ip ? ip : "");
    if (n < 0)
        return 0;
    /* snprintf returns the untruncated length; report what fits in out. */
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

int net_discover_local(const struct ifaddrs *ifs, struct in_addr src)
{
    for (const struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET)
            continue;
        in_addr_t a = ((const struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        in_addr_t m = ((const struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr;
        if (((src.s_addr ^ a) & m) == 0)
            return 1;
    }
    return 0;
}

int net_discover_rate_ok(net_discover_seen_t seen[NET_DISCOVER_SRC_MAX], struct in_addr src,
        int64_t now_ms)
{
    int free_slot = -1;

    for (int i = 0; i < NET_DISCOVER_SRC_MAX; i++) {
        if (seen[i].at_ms && now_ms - seen[i].at_ms < NET_DISCOVER_GAP_MS) {
            if (seen[i].addr == src.s_addr)
                return 0;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0)
        return 0;
    seen[free_slot].addr  = src.s_addr;
    seen[free_slot].at_ms = now_ms;
    return 1;
}

static int disc_src_local(struct in_addr src)
{
    struct ifaddrs *ifs;

    if (getifaddrs(&ifs) < 0)
        return 0;
    int ok = net_discover_local(ifs, src);
    freeifaddrs(ifs);
    return ok;
}

static void *disc_thread(void *arg)
{
    (void)arg;
    int                on = 1;
    struct sockaddr_in saddr;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        applog(LOG_ERR, "LAN discovery unavailable: cannot open a UDP socket");
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    memset(&saddr, 0, sizeof saddr);
    saddr.sin_family      = AF_INET;
    saddr.sin_port        = htons(NET_DISCOVER_PORT);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&saddr, sizeof saddr) < 0) {
        applog(LOG_ERR, "LAN discovery unavailable: UDP port %d is in use", NET_DISCOVER_PORT);
        close(fd);
        return NULL;
    }

    applog(LOG_DEBUG, "LAN discovery: answering on UDP port %d", NET_DISCOVER_PORT);
    net_discover_seen_t seen[NET_DISCOVER_SRC_MAX] = { 0 };
    for (;;) {
        char               buf[128] = { 0 };
        struct sockaddr_in raddr;
        socklen_t          rlen = sizeof raddr;

        int ret = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&raddr, &rlen);
        if (ret < 0) {
            sleep(1);
            continue;
        }
        buf[ret] = '\0';

        char ip[24] = { 0 };
        net_state_ip(ip, sizeof ip);

        char   reply[128];
        size_t n = net_discover_reply(buf, g_disc_dev, ip, reply, sizeof reply);
        if (n == 0 || !disc_src_local(raddr.sin_addr))
            continue;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (!net_discover_rate_ok(seen, raddr.sin_addr,
                    (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000))
            continue;
        /* The reply datagram includes the terminating NUL. */
        sendto(fd, reply, n + 1, 0, (struct sockaddr *)&raddr, rlen);
    }

    close(fd);
    return NULL;
}

void net_discover_start(const char *dev)
{
    pthread_t tid;

    snprintf(g_disc_dev, sizeof g_disc_dev, "%s", dev ? dev : "");
    if (pthread_create(&tid, NULL, disc_thread, NULL) != 0)
        applog(LOG_ERR, "LAN discovery unavailable: thread could not be started");
    else
        pthread_detach(tid);
}
