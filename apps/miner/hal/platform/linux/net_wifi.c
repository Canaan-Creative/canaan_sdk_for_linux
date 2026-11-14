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

#if IMPL_NET_IS_WIFI

#include "net_linux_common.h"
#include "osif.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int net_wifi_wait_ready(unsigned timeout_ms)
{
    unsigned polls = timeout_ms / 1000u;
    for (unsigned i = 0; i < polls; i++) {
        FILE *f = popen("wpa_cli -i " MM_NET_IFACE " status 2>/dev/null", "r");
        if (f) {
            char line[128];
            int  done = 0;
            while (fgets(line, sizeof line, f))
                if (strstr(line, "wpa_state=COMPLETED")) {
                    done = 1;
                    break;
                }
            pclose(f);
            if (done)
                return 0;
        }
        sleep(1);
    }
    osif_log(OSIF_LOG_WARN, "Network %s: WiFi not connected after %u s", MM_NET_IFACE, polls);
    return -1;
}

static const hal_net_ops_t wifi_ops = {
    .iface      = MM_NET_IFACE,
    .wait_ready = net_wifi_wait_ready,
    .dhcp       = net_linux_dhcp,
    .set_static = net_linux_set_static,
    .get_addr   = net_linux_get_addr,
};

const hal_net_ops_t *hal_net(void)
{
    return &wifi_ops;
}

#endif /* IMPL_NET_IS_WIFI */
