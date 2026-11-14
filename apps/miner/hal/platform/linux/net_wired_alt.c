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

#include "net_wired_alt.h"
#include "boardconf.h"

#if NET_WIRED_ALT

#include "net_linux_common.h"
#include "osif.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

#define WIRED_SYS_CARRIER MM_SYS_NET_DIR NET_WIRED_IFACE "/carrier"
#define WIRED_SYS_FLAGS   MM_SYS_NET_DIR NET_WIRED_IFACE "/flags"
#define MAIN_SYS_CARRIER  MM_SYS_NET_DIR MM_NET_IFACE "/carrier"

int wired_link_up(void)
{
    return net_linux_carrier_up(WIRED_SYS_CARRIER);
}

pid_t wired_dhcp_pid(void)
{
    return net_linux_udhcpc_pid(WIRED_DHCP_PIDF);
}

static void wired_spawn(char *const argv[], int detach)
{
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        if (detach && fork() != 0)
            _exit(0);
        if (detach)
            setsid();
        execvp(argv[0], argv);
        _exit(127);
    }
    (void)waitpid(pid, NULL, 0);
}

int wired_flags(unsigned *out)
{
    FILE *f = fopen(WIRED_SYS_FLAGS, "re");
    if (!f)
        return -1;
    unsigned v   = 0;
    int      got = fscanf(f, "%x", &v) == 1;
    fclose(f);
    if (!got)
        return -1;
    *out = v;
    return 0;
}

void wired_link_set_up(void)
{
    char *const up[] = { "ip", "link", "set", NET_WIRED_IFACE, "up", NULL };
    wired_spawn(up, 0);
}

void wired_dhcp_start(void)
{
    char *const dhcp[] = { "udhcpc", "-i", NET_WIRED_IFACE, "-p", (char *)WIRED_DHCP_PIDF, NULL };
    wired_spawn(dhcp, 1);
    osif_log(OSIF_LOG_OP, "Wired network %s: link up, requesting an IP address", NET_WIRED_IFACE);
}

static void wired_dns_handback(const char *static_dns)
{
    pid_t p;

    if (!net_linux_carrier_up(MAIN_SYS_CARRIER))
        return;
    p = net_linux_udhcpc_pid(MAIN_DHCP_PIDF);
    if (p > 0) {
        if (kill(p, SIGUSR1) == 0)
            osif_log(OSIF_LOG_INFO,
                    "%s: renewing its IP address and switching DNS back "
                    "to it",
                    MM_NET_IFACE);
    } else if (static_dns) {
        char ndns[INET_ADDRSTRLEN];

        net_linux_write_resolv(static_dns);
        osif_log(OSIF_LOG_INFO, "DNS switched back to %s static server %s", MM_NET_IFACE,
                net_linux_ipv4_norm(static_dns, ndns) == 0 ? ndns : static_dns);
    }
}

void wired_dhcp_stop(const char *static_dns)
{
    char *const flush[] = { "ip", "-4", "addr", "flush", "dev", NET_WIRED_IFACE, NULL };

    net_linux_udhcpc_stop(WIRED_DHCP_PIDF);
    wired_spawn(flush, 0);
    osif_log(OSIF_LOG_OP, "Wired network %s: link down, IP address released", NET_WIRED_IFACE);
    wired_dns_handback(static_dns);
}

#endif /* NET_WIRED_ALT */
