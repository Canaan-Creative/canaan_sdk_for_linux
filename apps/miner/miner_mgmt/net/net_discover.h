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
 * LAN discovery responder: an unauthenticated, read-only UDP service that
 * answers the probe NET_DISCOVER_CMD with the device name and IP, only to
 * senders in one of the device's own IPv4 subnets, at most once a second
 * per sender and to NET_DISCOVER_SRC_MAX senders a second in all.
 * The phone app's device search depends on it.
 */
#ifndef NET_DISCOVER_H
#define NET_DISCOVER_H

#include <stddef.h>
#include <stdint.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#define NET_DISCOVER_PORT    10001        /* UDP port of the responder */
#define NET_DISCOVER_CMD     "get_ip$$$$" /* probe prefix that gets a reply */
#define NET_DISCOVER_SRC_MAX 10           /* senders answered per second */

typedef struct {
    in_addr_t addr;  /* sender answered */
    int64_t   at_ms; /* CLOCK_MONOTONIC time of the reply, 0 = free */
} net_discover_seen_t;

size_t net_discover_reply(const char *req, const char *dev, const char *ip, char *out, size_t cap);
int    net_discover_local(const struct ifaddrs *ifs, struct in_addr src);
int    net_discover_rate_ok(net_discover_seen_t seen[NET_DISCOVER_SRC_MAX], struct in_addr src,
           int64_t now_ms);
void   net_discover_start(const char *dev);

#endif /* NET_DISCOVER_H */
