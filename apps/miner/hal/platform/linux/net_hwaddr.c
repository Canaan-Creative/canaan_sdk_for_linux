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

#define _DEFAULT_SOURCE
#include "net_hwaddr.h"
#include "boardconf.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>

int mc_api_identity_mac(char out[18])
{
    strcpy(out, "000000000000");
    static const char *const ifs[] = {
#if NET_WIRED_ALT
        NET_WIRED_IFACE,
#endif /* NET_WIRED_ALT */
        MM_NET_IFACE,
    };
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    for (unsigned i = 0; i < sizeof ifs / sizeof ifs[0]; i++) {
        struct ifreq r;
        memset(&r, 0, sizeof r);
        strncpy(r.ifr_name, ifs[i], IFNAMSIZ - 1);
        if (ioctl(fd, SIOCGIFHWADDR, &r) == 0) {
            unsigned char *m = (unsigned char *)r.ifr_hwaddr.sa_data;
            snprintf(out, 18, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            close(fd);
            return 0;
        }
    }
    close(fd);
    return 0;
}
