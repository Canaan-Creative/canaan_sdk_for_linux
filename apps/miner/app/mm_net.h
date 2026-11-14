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

#ifndef MM_NET_H
#define MM_NET_H

#include <stddef.h>
#include <stdint.h>

#define MM_CONSOLE_PATH  "/dev/console" /* receives the "mm_miner: IP" line */
#define MM_NET_DEV_WIRED 0              /* mm_net_devtype(): wired Ethernet */
#define MM_NET_DEV_WIFI  1              /* mm_net_devtype(): WiFi */

void mm_dev_name(char *out, size_t n);
int  mm_net_devtype(void);
int  mm_net_present(void);
int  mm_net_wired_present(void);
void mm_net_wired_rule(void *ctx, uint64_t now_ms);
void mm_net_ip_show_rule(void *ctx, uint64_t now_ms);

#endif /* MM_NET_H */
