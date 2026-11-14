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

#include "net_ops.h"
#include "boardconf.h"

#if IMPL_NET_IS_WIRED

#include "net_linux_common.h"
#include "osif.h"

#include <unistd.h>

#define NET_WIRED_CARRIER MM_SYS_NET_DIR MM_NET_IFACE "/carrier"

static int net_wired_wait_ready(unsigned timeout_ms)
{
    unsigned polls = timeout_ms / 1000u;
    for (unsigned i = 0; i < polls; i++) {
        if (net_linux_carrier_up(NET_WIRED_CARRIER))
            return 0;
        sleep(1);
    }
    osif_log(OSIF_LOG_WARN, "Network %s: no cable link after %u s", MM_NET_IFACE, polls);
    return -1;
}

static const hal_net_ops_t wired_ops = {
    .iface      = MM_NET_IFACE,
    .wait_ready = net_wired_wait_ready,
    .dhcp       = net_linux_dhcp,
    .set_static = net_linux_set_static,
    .get_addr   = net_linux_get_addr,
};

const hal_net_ops_t *hal_net(void)
{
    return &wired_ops;
}

#endif /* IMPL_NET_IS_WIRED */
