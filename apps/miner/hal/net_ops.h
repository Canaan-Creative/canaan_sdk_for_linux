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

#ifndef NET_OPS_H
#define NET_OPS_H

typedef struct hal_net_ops {
    const char *iface;
    int (*wait_ready)(unsigned timeout_ms); /* wait for link or association, 0 when ready */
    int (*dhcp)(void);
    int (*set_static)(const char *ip, const char *mask, const char *gw, const char *dns);
    /* current IPv4 address as a dotted string, 0 when one is assigned */
    int (*get_addr)(char *buf, unsigned cap);
} hal_net_ops_t;

const hal_net_ops_t *hal_net(void);

#endif /* NET_OPS_H */
